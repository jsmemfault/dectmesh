# tools/ — host-side 9P clients, proofs, and demos

Everything here runs on the **host** (macOS/Linux) and talks to a DECTstrous node's
9P server over a USB-CDC serial port bridged to a Unix socket with `socat` (see the
top-level `README.md` for the bridge pattern). C tools are single-file; build with
`cc -O2 -o <name> <name>.c` (compiled binaries are git-ignored, sources are committed).

## 9P clients (C)

| Tool | What it does |
|------|--------------|
| `p9do.c` | Persistent-session 9P client — holds **one** connection (version+attach) and runs a series of `rd:`/`ws:`/`wf:` ops on it. The workhorse for reads, writes, and OTA. |
| `aether_conv.c` | Fid-holding 9P client that drives the stateful `/net/aether` datagram service (clone → ctl → `N/data`), plus `--put` (OTA streaming), `--sendn`/`--crecv` (directed datagrams), `--bridge`, `--probe`. |
| `aether_test.c` | `/net/aether` conformance harness — exercises the clone/ctl/data contract and checks conversation lifecycle. |
| `aether_verify.c` | **Independently verifies a node's CGA ownership proof** (OpenSSL): checks `SHA256(pubkey)[:6] == addr` and the ECDSA-P256 signature over a challenge. No trust in the node. |

## Proof & test scripts

| Script | What it proves |
|--------|----------------|
| `run_aether_suite.sh` | Comprehensive, host-driven `/net/aether` regression suite (conformance, reliability, and the forced multi-hop Phases 5–6). The single "does it all still work" run. |
| `multihop_proof.sh` | Forces a linear A–DK–B topology (`aether deny`) and shows chat still crosses — the standalone multi-hop certainty proof (see `doc/PROOF.md`). |
| `aether_prove.sh` | Challenges a node to **prove it owns its CGA** — drives `p9do` + `aether_verify` end to end (`net/aether/prove`). |
| `aether_chat_test.sh` | End-to-end party-line chat test across the full host→relay→9151 path. |
| `rel_sweep.sh` | Characterizes reliable-datagram delivery under load. |

## Observability

| Tool | What it does |
|------|--------------|
| `aether_forwarder.c` | **The hardened, run-continuously telemetry forwarder (recommended).** A single long-lived C daemon that **owns the gateway serial port directly** (termios raw — no `socat`, so the macOS CDC driver is never churned/wedged), speaks 9P over it, drains the gateway's own chunks **and every in-range mesh peer's `dev/mflt` over the air**, and POSTs to Memfault. Built for 24/7 with roaming nodes: per-read timeouts (never hangs), auto-reconnect across gateway power-cycles, peers rediscovered each cycle by CGA with `+ appeared` / `- silent` come/go logging, and uart-wedge detection. `MEMFAULT_PROJECT_KEY=<key> aether_forwarder <port|dev> [interval_s]`. Supervise it with `aether_forwarder.plist.example` (launchd). See `doc/OBSERVABILITY.md`. |
| `mflt_forward.sh` | Bash prototype (superseded by `aether_forwarder`): drains both chips' chunks via a `socat`-bridged connection and POSTs them. Kept for quick interactive one-shots (`… once`) and `field_monitor.sh`; the socat cycling makes it fragile for long continuous runs. `MEMFAULT_PROJECT_KEY=<key> mflt_forward.sh <relay 9P port> [interval\|once]`. |
| `mflt_upload_symbols.sh` | Uploads a `zephyr.elf` to Memfault (3-step presigned-upload REST flow, curl) so coredumps/traces symbolicate. Run once per software type+version. Needs an **Org Auth Token** via `MEMFAULT_ORG_TOKEN` (pass at runtime, never commit). `MEMFAULT_ORG_TOKEN=<oat> mflt_upload_symbols.sh <org> <project> <sw-type> <version> <elf>`. |

## Native chat client — `achat/`

A native plan9port Aether chat client (opens the USB-CDC port directly via
`lib9pclient`, no FUSE/mount): `achat_core.c` (headless), `achat_gui.c` (libdraw GUI),
`serial.c` (raw-mode port open), `aether.rc` (a pure-`rc` variant). `build.sh` builds
them; `mkapp.sh` bundles a macOS `.app`.

## Field & demo utilities

| Script | What it does |
|--------|--------------|
| `field_metrics.sh` | Samples a tethered field node's mesh + PHY state on a timer (field-test logging). |
| `mesh9p_demo.sh` | A clean, disciplined 9P-over-mesh demo (browse a 2-hop-away node's filesystem). |
| `mount-finder.sh`, `mount-term.sh` | Bridge + FUSE-mount a node's 9P namespace. *Reference only* — the mount path is superseded by direct `socat`+`9p` and `achat`. |
