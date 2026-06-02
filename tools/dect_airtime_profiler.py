#!/usr/bin/env python3
# Copyright (c) 2026 Jon Sharp
# SPDX-License-Identifier: MIT
"""
DECT NR+ PHY airtime profiler — rough, uncalibrated, but honest timing.

An SDR's sample clock measures *time* without needing amplitude calibration, so
even an RTL-SDR gives trustworthy numbers for the metrics that actually gate
FCC 47 CFR 15.323 certification:

  * burst (transmission) duration       <- max TX time limits
  * duty cycle                          <- coexistence / etiquette
  * inter-burst gaps & frame periodicity <- TDMA slot timing
  * occupied bandwidth                   <- ~1.728 MHz per DECT NR+ channel

What it CANNOT tell you (these need a calibrated analyzer + lab/OTA):
  * absolute power / EIRP (the 10 dBm cap)
  * spurious / out-of-band emissions to spec
  * OFDM payload demodulation (DECT-2020 NR is OFDM, not classic-DECT GFSK)

------------------------------------------------------------------------------
BAND NOTE: a stock RTL-SDR (R820T2, incl. Blog V3/V4) tops out ~1.7 GHz, so it
CANNOT see the US 1.9 GHz DECT band (1920-1930 MHz). Use it on the 902-928 MHz
sub-GHz NR+ band (where it's native and a 1.728 MHz channel fits the ~2.4 MHz
capture), or use a HackRF / PlutoSDR for 1.9 GHz.
------------------------------------------------------------------------------

Capture IQ first (8-bit interleaved, the rtl_sdr default), ~2 seconds:

    rtl_sdr -f 915000000 -s 2400000 -g 40 capture.iq      # Ctrl-C after ~2 s

Then profile:

    python3 dect_airtime_profiler.py --file capture.iq --rate 2.4e6

Or validate the measurement pipeline against a synthetic TDMA signal with known
burst length / period / duty cycle (no hardware needed):

    python3 dect_airtime_profiler.py --selftest

Or capture + analyze live (needs pyrtlsdr / librtlsdr):

    python3 dect_airtime_profiler.py --live --freq 915e6 --seconds 2

Dependencies: numpy (required); matplotlib + scipy (optional, for plots/PSD);
pyrtlsdr (optional, for --live).
"""

import argparse
import sys

import numpy as np

try:
    import matplotlib
    matplotlib.use("Agg")  # headless: always write PNGs
    import matplotlib.pyplot as plt
    HAVE_MPL = True
except Exception:
    HAVE_MPL = False


# ---------------------------------------------------------------------------
# IQ acquisition
# ---------------------------------------------------------------------------

def load_iq_file(path, rate, seconds):
    """Read rtl_sdr's uint8 interleaved IQ, return complex64 centered at 0."""
    max_bytes = int(rate * seconds) * 2 if seconds else None
    raw = np.fromfile(path, dtype=np.uint8, count=max_bytes if max_bytes else -1)
    raw = raw[: (len(raw) // 2) * 2]
    iq = (raw[0::2].astype(np.float32) - 127.5) + 1j * (raw[1::2].astype(np.float32) - 127.5)
    return iq / 127.5


def capture_live(freq, rate, seconds, gain):
    """Capture via pyrtlsdr."""
    from rtlsdr import RtlSdr
    sdr = RtlSdr()
    sdr.sample_rate = rate
    sdr.center_freq = freq
    sdr.gain = gain if gain is not None else "auto"
    n = int(rate * seconds)
    iq = sdr.read_samples(n)          # already complex, ~[-1, 1]
    sdr.close()
    return np.asarray(iq, dtype=np.complex64)


def synth_tdma(rate, seconds, burst_us, period_us, snr_db):
    """Synthetic on/off TDMA burst train with known timing, for --selftest."""
    n = int(rate * seconds)
    burst = int(burst_us * 1e-6 * rate)
    period = int(period_us * 1e-6 * rate)
    env = np.zeros(n, dtype=np.float32)
    for start in range(0, n - burst, period):
        env[start:start + burst] = 1.0
    sig_amp = 0.5
    carrier = sig_amp * env * np.exp(1j * 2 * np.pi * 0.0 * np.arange(n))
    noise_amp = sig_amp / (10 ** (snr_db / 20.0))
    noise = noise_amp * (np.random.randn(n) + 1j * np.random.randn(n)) / np.sqrt(2)
    truth = dict(burst_us=burst_us, period_us=period_us,
                 duty=burst / period)
    return (carrier + noise).astype(np.complex64), truth


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

def moving_avg(x, w):
    if w <= 1:
        return x
    c = np.cumsum(np.insert(x, 0, 0.0))
    out = (c[w:] - c[:-w]) / w
    pad = np.full(w - 1, out[0])
    return np.concatenate([pad, out])


def detect_bursts(power, rate, thr, min_burst_us, hyst=0.7):
    """Return list of (start_idx, end_idx) runs above thr (with light hysteresis)."""
    min_len = int(min_burst_us * 1e-6 * rate)
    on = power > thr
    bursts = []
    i, n = 0, len(power)
    while i < n:
        if on[i]:
            start = i
            # extend while above the lower hysteresis level
            while i < n and power[i] > thr * hyst:
                i += 1
            if (i - start) >= min_len:
                bursts.append((start, i))
        else:
            i += 1
    return bursts


def occupied_bw(iq, rate):
    """99%-power occupied bandwidth and -3/-20 dB widths from a Welch-ish PSD."""
    nfft = 4096
    nseg = max(1, len(iq) // nfft)
    acc = np.zeros(nfft)
    win = np.hanning(nfft)
    for k in range(nseg):
        seg = iq[k * nfft:(k + 1) * nfft]
        if len(seg) < nfft:
            break
        acc += np.abs(np.fft.fftshift(np.fft.fft(seg * win))) ** 2
    psd = acc / max(1, nseg)
    freqs = np.fft.fftshift(np.fft.fftfreq(nfft, d=1.0 / rate))
    total = psd.sum()
    csum = np.cumsum(psd)
    lo = freqs[np.searchsorted(csum, 0.005 * total)]
    hi = freqs[np.searchsorted(csum, 0.995 * total)]
    peak = psd.max()
    above3 = freqs[psd > peak / 2.0]
    above20 = freqs[psd > peak / 100.0]
    return dict(
        occ99_hz=hi - lo,
        bw3_hz=(above3.max() - above3.min()) if len(above3) else 0.0,
        bw20_hz=(above20.max() - above20.min()) if len(above20) else 0.0,
        peak_offset_hz=freqs[np.argmax(psd)],
        psd=psd, freqs=freqs,
    )


def dominant_period(presence, rate):
    """First strong autocorrelation peak of the on/off presence signal -> period."""
    p = presence.astype(np.float32)
    p = p - p.mean()
    if np.allclose(p, 0):
        return None
    ac = np.correlate(p, p, mode="full")[len(p) - 1:]
    ac /= ac[0] if ac[0] != 0 else 1.0
    # ignore the zero-lag main lobe
    min_lag = int(50e-6 * rate)
    if min_lag >= len(ac):
        return None
    seg = ac[min_lag:]
    peak = np.argmax(seg) + min_lag
    if ac[peak] < 0.2:                 # too weak to call periodic
        return None
    return peak / rate


def analyze(iq, rate, smooth_us, thr_db, min_burst_us):
    power = np.abs(iq) ** 2
    w = max(1, int(smooth_us * 1e-6 * rate))
    psm = moving_avg(power, w)

    noise = np.percentile(psm, 20)     # robust floor (assumes <80% duty)
    thr = noise * (10 ** (thr_db / 10.0))
    bursts = detect_bursts(psm, rate, thr, min_burst_us)

    dur_us = np.array([(e - s) / rate * 1e6 for s, e in bursts])
    starts = np.array([s for s, _ in bursts])
    gaps_us = (np.diff(starts) / rate * 1e6 - dur_us[:-1]) if len(bursts) > 1 else np.array([])
    intervals_us = (np.diff(starts) / rate * 1e6) if len(bursts) > 1 else np.array([])
    on_samples = sum(e - s for s, e in bursts)
    duty = on_samples / len(iq) if len(iq) else 0.0

    presence = np.zeros(len(psm), dtype=np.uint8)
    for s, e in bursts:
        presence[s:e] = 1
    period_s = dominant_period(presence, rate)

    bw = occupied_bw(iq, rate)

    return dict(
        rate=rate, n=len(iq), dur_s=len(iq) / rate,
        psm=psm, thr=thr, noise=noise, bursts=bursts,
        dur_us=dur_us, gaps_us=gaps_us, intervals_us=intervals_us,
        duty=duty, period_s=period_s, bw=bw,
    )


# ---------------------------------------------------------------------------
# Report + plots
# ---------------------------------------------------------------------------

def fmt(x, u=""):
    return "n/a" if x is None or (isinstance(x, float) and np.isnan(x)) else f"{x:.3f}{u}"


def report(a, truth=None):
    nb = len(a["bursts"])
    print("\n" + "=" * 66)
    print("  DECT NR+ AIRTIME PROFILE  (uncalibrated timing — rough but real)")
    print("=" * 66)
    print(f"  capture          : {a['dur_s']:.3f} s  @ {a['rate']/1e6:.3f} Msps "
          f"({a['n']:,} samples)")
    print(f"  bursts detected  : {nb}")
    if nb:
        d = a["dur_us"]
        print(f"  burst duration   : mean {d.mean():.1f}  median {np.median(d):.1f}  "
              f"min {d.min():.1f}  max {d.max():.1f}  us")
        if len(a["gaps_us"]):
            g = a["gaps_us"]
            print(f"  inter-burst gap  : mean {g.mean():.1f}  median {np.median(g):.1f}  us")
        if len(a["intervals_us"]):
            iv = a["intervals_us"]
            print(f"  start interval   : mean {iv.mean():.1f}  median {np.median(iv):.1f}  us")
    print(f"  duty cycle       : {a['duty']*100:.2f} %")
    print(f"  frame period     : "
          + (f"{a['period_s']*1e3:.3f} ms  (autocorr)" if a["period_s"] else "n/a"))
    bw = a["bw"]
    print(f"  occupied BW(99%) : {bw['occ99_hz']/1e6:.3f} MHz   "
          f"(-3dB {bw['bw3_hz']/1e6:.3f}, -20dB {bw['bw20_hz']/1e6:.3f})")
    print(f"  peak freq offset : {bw['peak_offset_hz']/1e3:+.1f} kHz from tuned center")
    print("-" * 66)
    print("  15.323 case-making — compare these to the spec criteria:")
    print(f"    max TX burst     : {fmt(a['dur_us'].max() if nb else None, ' us')}")
    print(f"    duty cycle       : {a['duty']*100:.2f} %")
    print(f"    channel BW       : {bw['occ99_hz']/1e6:.3f} MHz (target ~1.728)")
    print("    LBT/deferral     : run a busy-channel test + re-profile to confirm back-off")
    print("=" * 66)
    if truth:
        print("  SELF-TEST vs ground truth:")
        print(f"    burst  meas {a['dur_us'].mean() if nb else float('nan'):.1f} us "
              f"vs true {truth['burst_us']:.1f} us")
        print(f"    period meas {a['period_s']*1e6 if a['period_s'] else float('nan'):.1f} us "
              f"vs true {truth['period_us']:.1f} us")
        print(f"    duty   meas {a['duty']*100:.2f} % vs true {truth['duty']*100:.2f} %")
        print("=" * 66)


def make_plots(a, out_prefix):
    if not HAVE_MPL:
        print("  (matplotlib not available — skipping plots)")
        return
    rate = a["rate"]
    t = np.arange(len(a["psm"])) / rate * 1e3  # ms
    # decimate for a sane plot size
    step = max(1, len(t) // 200000)
    fig, ax = plt.subplots(3, 1, figsize=(11, 9))

    ax[0].plot(t[::step], 10 * np.log10(a["psm"][::step] + 1e-12), lw=0.4)
    ax[0].axhline(10 * np.log10(a["thr"] + 1e-12), color="r", ls="--", lw=0.8, label="threshold")
    for s, e in a["bursts"][:2000]:
        ax[0].axvspan(s / rate * 1e3, e / rate * 1e3, color="g", alpha=0.15)
    ax[0].set(xlabel="time (ms)", ylabel="power (dB, rel)", title="Power envelope + detected bursts")
    ax[0].legend(loc="upper right", fontsize=8)

    if len(a["dur_us"]):
        ax[1].hist(a["dur_us"], bins=60)
        ax[1].set(xlabel="burst duration (us)", ylabel="count", title="Burst duration distribution")
    bw = a["bw"]
    psd_db = 10 * np.log10(bw["psd"] + 1e-12)
    ax[2].plot(bw["freqs"] / 1e6, psd_db - psd_db.max(), lw=0.6)
    ax[2].axhline(-3, color="orange", ls=":", lw=0.8)
    ax[2].axhline(-20, color="r", ls=":", lw=0.8)
    ax[2].set(xlabel="offset (MHz)", ylabel="PSD (dB, rel peak)",
              title=f"Occupied BW ~{bw['occ99_hz']/1e6:.3f} MHz")
    fig.tight_layout()
    png = f"{out_prefix}.png"
    fig.savefig(png, dpi=110)
    print(f"  plots -> {png}")


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="DECT NR+ airtime profiler (RTL-SDR/IQ)")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--file", help="rtl_sdr uint8 IQ capture")
    src.add_argument("--live", action="store_true", help="capture live via pyrtlsdr")
    src.add_argument("--selftest", action="store_true", help="synthetic signal sanity check")
    ap.add_argument("--rate", type=float, default=2.4e6, help="sample rate (default 2.4e6)")
    ap.add_argument("--freq", type=float, default=915e6, help="center freq for --live")
    ap.add_argument("--seconds", type=float, default=2.0, help="capture/analyze duration")
    ap.add_argument("--gain", type=float, default=None, help="RTL gain dB (--live)")
    ap.add_argument("--smooth-us", type=float, default=5.0, help="envelope smoothing window")
    ap.add_argument("--thr-db", type=float, default=8.0, help="burst threshold above noise floor")
    ap.add_argument("--min-burst-us", type=float, default=30.0, help="reject bursts shorter than this")
    ap.add_argument("--out", default="dect_airtime", help="output PNG prefix")
    args = ap.parse_args()

    truth = None
    if args.selftest:
        print("  self-test: synth TDMA burst=200us period=1000us @20dB SNR")
        iq, truth = synth_tdma(args.rate, args.seconds, burst_us=200,
                               period_us=1000, snr_db=20)
    elif args.live:
        try:
            iq = capture_live(args.freq, args.rate, args.seconds, args.gain)
        except Exception as e:
            print(f"  live capture failed ({e}). Install pyrtlsdr, or use "
                  f"rtl_sdr + --file.", file=sys.stderr)
            return 1
    else:
        iq = load_iq_file(args.file, args.rate, args.seconds)

    if len(iq) < int(args.rate * 1e-3):
        print("  not enough samples to analyze", file=sys.stderr)
        return 1

    a = analyze(iq, args.rate, args.smooth_us, args.thr_db, args.min_burst_us)
    report(a, truth)
    make_plots(a, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
