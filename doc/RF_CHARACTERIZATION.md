# RF characterization — measuring the mesh's on-air behavior

*A measurement methodology and running log for characterizing what a DECTstrous
node actually puts on the air. This is the near-term regulatory rigor behind the
project — the concrete data an RF-certification review needs — captured with an
SDR and open tooling.*

## Purpose &amp; honest scope

The Æther/HONR mesh runs on the **DECT NR+ PHY** with a **custom MAC** — it is an
**experimental protocol, not a certified product**, and nothing here asserts
compliance. What this document does is **measure the emissions** so that the people
who *do* own certification can assess regime applicability and gaps against real
numbers instead of hand-waving.

- **In scope:** center frequency, occupied bandwidth, spectral behavior, transmit
  duty cycle, burst timing, listen-before-talk (LBT) behavior, and relative power —
  measured at defined operating points, reproducibly.
- **Out of scope / for the experts:** the compliance determination itself,
  calibrated absolute power / EIRP to a standard, and the choice of applicable rule.
  An SDR gives *relative* and *behavioral* truth cheaply; **absolute-power and
  formal spectral-mask verification need calibrated lab gear** — flagged inline.

**Regulatory context (unsettled — which is the point).** Two bands are in play:

| Band | Carriers | Frequency | US regime | Status |
|------|----------|-----------|-----------|--------|
| **Band 4** | 525–551 | ~902–928 MHz (538 ≈ 915 MHz) | overlaps ISM 902–928 **and** LTE B8; Nordic flags it **R&D-evaluation only** on the nRF9151, no clean unlicensed home | bench / experimentation |
| **Band 9** | 1657–1711 (grp 0) | ~1920–1930 MHz (UPCS) | **47 CFR Part 15.323** (UPCS) | the DECT-native US band |

Band 4's US unlicensed status is genuinely unsettled — *exactly* why we characterize
the signal and take it to the RF-cert specialists rather than guessing. Keep TX power
low and operation on the bench until that review happens.

## The signal under test

What a node transmits (see `../doc/NET_AETHER_SPEC.md`, `../dect_mesh/prj.conf`, and
aephyr's DECT PHY driver):

- **Modulation / bandwidth:** DECT NR+ OFDM. On-bench RTL-SDR capture shows a
  **~1.7 MHz occupied block** — confirm and quantify (§ Occupied bandwidth).
- **Center frequency:** `Fc[MHz] = 450.144 + carrier × 0.864`. Carrier **538 →
  ~914.976 MHz**; carrier 1711 → ~1928.6 MHz. `band_group_index` 1 = ~1 GHz, 0 = ~2 GHz.
- **Power:** `CONFIG_AETHER_DECT_TX_POWER` is a **power-table index** (ETSI TS 103
  636-4 Table 6.2.1-3), **not dBm**; band 4 is **power class 3**. The index→dBm
  mapping is itself something to measure.
- **Traffic pattern (this is the interesting part — duty cycle is *emergent*):**
  - **Idle:** a periodic HELLO/beacon, ~1 ms on every ~5 s per node → **~0.02% duty**
    at idle. It needs a fast waterfall / max-hold to even catch.
  - **Active:** event-driven chat/data bursts, reliable-unicast **ARQ** (retransmits
    on loss), and **flooded broadcast** (each interior node re-transmits). Aggregate
    channel occupancy scales with **node count × traffic × retransmissions** — so it
    must be characterized at defined operating points, not as a single number.

## What we measure, and why

| Parameter | Why it matters | Method (§) | Needs cal gear? |
|-----------|----------------|-----------|-----------------|
| Center frequency &amp; accuracy | frequency error vs the Fc formula; stays in-band | § Center frequency | SDR (after ppm cal) |
| Occupied bandwidth (99% OBW) | channel width; adjacent-channel spacing | § Occupied bandwidth | SDR if BW ≥ signal |
| −20 dB / −26 dB emission BW | emission-mask inputs | § Occupied bandwidth | SDR |
| Out-of-band / spurious emissions | spectral mask; interference to LTE B8 etc. | § Spectral mask | **calibrated** for pass/fail |
| Conducted / radiated power (dBm, EIRP) | power limits; the index→dBm map | § Power | **calibrated** |
| Transmit duty cycle | duty-cycle limits; the headline mesh metric | § Duty cycle | SDR |
| Burst on-time &amp; cadence (PRI) | timing structure; beacon vs load | § Duty cycle | SDR |
| LBT / CCA behavior | politeness; medium sharing; back-off | § LBT | SDR + node stats |
| Channel occupancy (aggregate) | fleet-level medium utilization | § Duty cycle | SDR |

## Measurement setup (SDR + open tooling — no Python)

**Receive SDR.** An **RTL-SDR** (R820T2, ~2.4–3.2 MHz usable) is enough for center
frequency, duty cycle, cadence, and LBT behavior, and *just* spans the ~1.7 MHz
block for a rough OBW. For clean **OBW, spectral mask, and wideband spurious**, use a
wider, better-behaved SDR (**HackRF** ~20 MHz, or a **USRP**); for **absolute power
and formal mask pass/fail**, use the RF lab's **calibrated analyzer** (this is the
natural hand-off to the cert specialists).

**Tooling (all non-Python):**
- **`rtl_power`** — swept power-vs-time CSV over a band (occupancy, coarse duty).
- **`rtl_sdr`** — raw IQ capture for offline envelope/FFT analysis.
- **`kalibrate-rtl` (`kal`)** — measure the RTL-SDR's ppm error against GSM *before*
  any frequency-accuracy claim (the dongle's own error dwarfs the DUT's otherwise).
- **GNU Radio Companion** — author `.grc` flowgraphs (no hand-written Python): a
  power-envelope / duty-cycle detector and an averaged-PSD / OBW measurement (blocks
  below). Drive via `gnuradio-companion` or headless `grcc`-generated flowgraphs.
- **`soapy_power` / SoapySDR** — the same sweeps for a HackRF/USRP.

**Calibration &amp; hygiene.** Run `kal` for ppm first. RTL-SDR power is
**uncalibrated** — treat all power as *relative* unless referenced to a known source;
note gain settings and disable AGC. Prefer **conducted** capture (SMA + a known
attenuator, antenna port → SDR) over radiated for repeatability; if radiated, fix and
record geometry. Record the noise floor with the DUT **off**.

**Driving known TX patterns from the node** (over the 9151 console, `aether` / `dect`
shells):

```sh
dect info                 # carrier, band group, TX-power index in use
dect stats                # tx ok / tx err / LBT-busy counters (before + after a run)
aether hello              # emit a single beacon frame (clean, isolated burst)
aether chat <msg>         # a broadcast data burst (flooded by the mesh)
# idle scenario  = don't type anything; only the ~5 s beacon transmits
# loaded scenario = scripted bursts, e.g. tools/rel_sweep.sh / run_aether_suite.sh
```

Sweeping `CONFIG_AETHER_DECT_TX_POWER` across builds (or via `dect` shell if exposed)
maps the power **index → measured dBm**. Sweeping `CONFIG_AETHER_DECT_CARRIER` moves
the center per the Fc formula — a good end-to-end sanity check of the whole chain.

## Methods

### Center frequency
1. `kal -s GSM850 -g 40` (or nearest band) → record ppm; apply with `-p <ppm>`.
2. `rtl_sdr -f 915000000 -s 2400000 -p <ppm> -g 30 -n 24000000 cap.iq` while the node
   beacons; or a GRC flowgraph: *osmocom/RTL source → FFT/QT sink*, max-hold on.
3. Find the OFDM block's center from the averaged FFT; compare to `450.144 +
   carrier×0.864`. Report error in Hz and ppm (after removing the SDR's own ppm).

### Occupied bandwidth
GRC flowgraph: *SDR source → Stream-to-Vector (Nfft) → FFT → Complex-to-Mag² →
Integrate/average (long dwell, DUT keyed continuously or max-held over many bursts) →
File/Vector sink*. Offline, integrate the PSD and find the **99% power** band (OBW),
plus the **−20 dB** and **−26 dB** points. If using RTL-SDR, verify the block fits
inside the capture BW (it barely does at ~1.7 MHz / 2.4 Msps — prefer HackRF here).

### Spectral mask &amp; spurious
Wideband **max-hold** over a span several × the channel (HackRF/USRP, or lab
analyzer): `soapy_power -f 850M:1000M -O mask.csv` (band 4) with the DUT bursting, and
a **DUT-off reference** sweep. Overlay against the applicable emission mask — a
**pass/fail** claim needs the **calibrated analyzer**; the SDR sweep is for finding
spurs and shaping the lab session.

### Power (relative, then calibrated)
Relative: fixed gain, conducted through a known attenuator, record peak PSD per
TX-power index → the index→relative-dBm curve. **Absolute dBm / EIRP is a calibrated
measurement** — hand the setup + the index curve to the RF lab to anchor.

### Duty cycle &amp; burst timing
The headline mesh metric, and *emergent*. GRC flowgraph: *freq-xlating FIR (center on
the block) → Complex-to-Mag² → single-pole IIR (smooth) → Threshold (above noise
floor) → to a time-tagged File sink*. Offline, compute **on-time / total-time** over a
fixed window, and the **burst on-time** and **inter-burst interval (PRI)**.
Cross-check the count against `dect stats` `tx ok` delta over the same window.

Characterize at **defined operating points** (record node count each time):

| Operating point | Setup | Expect |
|-----------------|-------|--------|
| **Idle (1 node)** | no traffic; beacon only | ~0.02% duty, ~5 s PRI |
| **Idle (N nodes)** | N nodes, no traffic | ~N × beacon; note aggregate |
| **Light chat** | scripted 1 msg / few s | beacon + sparse bursts |
| **Saturated** | `run_aether_suite.sh` / `rel_sweep.sh` | ARQ + flood; worst-case duty |

Report **per-node** and **aggregate** duty cycle — a regime may cap per-device, and
the mesh's flooding multiplies channel occupancy with node count.

### LBT / CCA behavior
DECT NR+ does listen-before-talk. Bring up a **contending transmitter** on the same
carrier and, on the DUT, watch `dect stats` **LBT-busy** climb; on the SDR, correlate
the DUT's deferrals against observed channel-busy intervals. Characterize the **CCA
threshold** (raise the contender's power until the DUT starts deferring) and the
**back-off behavior**. On carrier 538 idle, baseline is `60 tx ok / 0 err / 0
LBT-busy` — the contended run is where LBT proves itself.

## Measurement log

Fill one row per capture. Keep the raw `.iq` / `.csv` alongside (git-ignored) and
reference by filename.

| Date | FW ver | Band/carrier | TXpow idx | Nodes | Scenario | SDR + settings (gain, sr, ppm) | Metric | Value | Capture file | Notes |
|------|--------|--------------|-----------|-------|----------|--------------------------------|--------|-------|--------------|-------|
| 2026-07-24 | 0.7.38 | b4 / 538 | 10 | 1 | idle | RTL, g30 sr2.4M ppm? | center freq | *(tbd)* | | example row |
| | | | | | | | 99% OBW | | | |
| | | | | | | | duty (idle) | | | |
| | | | | | | | LBT-busy Δ | | | |

## What this produces

A **characterization report** — the measured emissions profile (center freq &amp;
accuracy, OBW, spectral behavior, duty cycle across operating points, LBT behavior,
relative power curve) — that the **RF-certification specialists** use to assess which
regime applies and where the gaps are. This doc gathers the evidence; the compliance
determination, the calibrated pass/fail measurements, and the certification path are
theirs to own, with this as the concrete starting artifact.

## References

- **ETSI TS 103 636-2** — DECT NR+ physical layer.
- **ETSI TS 103 636-4** — MAC; power classes, **Table 6.2.1-3** (TX-power index).
- **47 CFR Part 15.323** — UPCS (1920–1930 MHz, band 9); **Part 15.247/15.249** —
  the ISM 902–928 rules that band 4 overlaps (applicability unsettled — for the experts).
- Nordic DECT NR+ PHY documentation (`%XRFTEST` band ranges; band-4 R&D-evaluation status).
- Project: `../README.md`, `doc/ARCHITECTURE.md`, `doc/NET_AETHER_SPEC.md`; carrier/band
  facts and the Fc formula are exercised on-device via `dect bands` / `dect info`.
