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
