# DECT NR+ PHY mesh — demo project

A self-organizing, self-healing multi-hop mesh on Nordic **nRF9151** radios,
built directly on the **DECT NR+ physical layer** (ETSI TS 103 636‑2) — plus a
**9P gateway** that exposes the mesh as a filesystem (`/net/aether`) over
Bluetooth LE, and **Memfault** fleet telemetry.

> **Terminology (it matters):** this uses only the **DECT NR+ PHY** (Nordic's
> PHY‑only modem firmware, `nrf_modem_dect_phy`). The MAC, routing (HONR), and
> self‑organization are the **Æther** stack — a *custom* protocol, **not** the
> DECT‑2020 NR MAC, and therefore not a certifiable "DECT NR+" product. See the
> regulatory section in the aephyr README.

## Layout

| Path | What |
|---|---|
| **`dect_mesh/`** | nRF9151 app: Æther/HONR mesh over the DECT NR+ PHY + a 9P `/net/aether` server + Memfault metrics |
| **`dect_relay/`** | nRF5340 app: BLE L2CAP↔UART byte relay carrying the 9151's 9P server to clients (plan C) |
| **`dectfw/`** | Licensed DECT NR+ PHY modem firmware (access‑gated — never commit) |
| **`legacy_flood/`** | The original standalone flood+TTL Memfault example, superseded by `dect_mesh` (kept for reference) |
| **`NRplus-mesh-*.md`** | Pitch / role / demo‑script docs |

## Dependencies (Zephyr modules)

Both apps consume two out‑of‑tree modules:
- **aephyr** — the Æther/HeyMac/HONR mesh stack + DECT NR+ PHY driver (the library).
- **9p4z** — the 9P protocol library (UART, TCP, and Bluetooth L2CAP transports).

## Build

```bash
# nRF9151 mesh + 9P + Memfault
west build -p -b thingy91x/nrf9151/ns dect_mesh -- \
  -DZEPHYR_EXTRA_MODULES="/path/to/aephyr;/path/to/9p4z"

# nRF5340 BLE relay (sysbuild also builds the net-core HCI controller)
west build -p -b thingy91x/nrf5340/cpuapp dect_relay --sysbuild
```

The DECT NR+ PHY modem firmware (`dectfw/`) is flashed to the nRF9151's modem
core once (via an external SWD probe); the app images then update over USB.

## Architecture (plan C: the 9P gateway)

```
  client (cyberdeck)            Thingy:91 X
  9P over BLE L2CAP  <----->  [nRF5340 relay]  <-- UART -->  [nRF9151]
   mount /net/aether          L2CAP <-> uart1               Æther/HONR mesh
                                                            + 9P /net/aether server
                                                            + DECT NR+ PHY radio
```

The 9P server runs **in‑process with the mesh** on the nRF9151, so `/net/aether`
is live mesh state (address, rank, neighbors, routes, tree, party‑line `chat`).
The nRF5340 is a transparent byte relay (no 9P parsing), so the same tree is
reachable over BLE, UART, or — via the aephyr stack — across the mesh itself.
The PHY is, in effect, a mount point: the same `/net/aether` can be served over
LoRa, DECT NR+, or future sub‑GHz NR+ without the client changing.
