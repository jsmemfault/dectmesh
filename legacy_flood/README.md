# DECT NR+ Mesh (example)

A small **DECT NR+** example that turns a handful of nRF9151 DKs into a
**multi-hop mesh** and reports health to a **Memfault** fleet.

Every node is a peer. Each originates periodic broadcast messages and relays
messages it has not seen before, bounded by a time-to-live (TTL) hop count and a
duplicate-suppression cache. This is a *controlled flood* built directly on the
DECT NR+ **PHY** layer — see [Scope](#scope-what-this-is-and-is-not) for why.

Built and guided with the Nordic MCP server against **nRF Connect SDK v3.3.0**.

---

## Scope: what this is (and is not)

DECT NR+ true mesh is **not** part of the open Nordic stack. The DECT NR+ modem
firmware (MAC v2.0.0) implements a **star** topology (a Fixed Termination parent
with Portable Termination children); commercial multi-hop stacks (for example
Wirepas 5G Mesh) are licensed separately and are not in the nRF Connect SDK.

So this example builds its **own** application-layer mesh on top of the open
**DECT NR+ PHY API** (`nrf_modem_dect_phy`), the same API used by the SDK's
`dect/dect_phy/hello_dect` sample. It demonstrates real multi-hop forwarding
(flood + TTL + dedup + a neighbor table) using only components you can build
today. It is a teaching example, not a production mesh:

- Flooding, not routing — no path selection or unicast acknowledgement.
- Half-duplex cadence — a node transmits, then opens a receive window; it is not
  listening while transmitting.
- No link-layer security or encryption at the application layer.

## Hardware and prerequisites

- One or more **nRF9151 DK** boards (nRF9161 DK or Thingy:91 X also have the
  DECT NR+ capable SiP).
- **nRF Connect SDK v3.3.0** and its toolchain (installed here under
  `/opt/nordic/ncs/v3.3.0`).
- **DECT NR+ PHY modem firmware**, flashed to the modem core. It is distributed
  through Nordic sales (regulatory reasons) — request it via Nordic Contact Us.
  The application links against the PHY binary that ships in nrfxlib, but the
  modem needs the PHY firmware to actually transmit and receive.
- A **Memfault** account and project key (for uploading exported chunks).

> **Regulatory notice.** DECT NR+ operates on free but **regulated** channels.
> Availability and rules vary by country. It is your responsibility to operate
> within local regulations at every site. Only build with `overlay-eu.conf` /
> `overlay-us.conf` if you are permitted to operate on the DECT band and can
> meet the access rules; otherwise ensure there are no emissions (RF chamber).

## Project layout

```
.
├── CMakeLists.txt
├── Kconfig                                  # DECT_MESH_* options
├── prj.conf                                 # DECT PHY + Memfault config
├── overlay-eu.conf / overlay-us.conf        # regional carrier
├── config/
│   ├── memfault_platform_config.h           # heartbeat interval
│   └── memfault_metrics_heartbeat_config.def# custom mesh metrics
└── src/
    ├── main.c            # thin orchestration: PHY up, then TX/RX cadence
    ├── dect_phy.c/.h     # DECT NR+ PHY wrapper (init, blocking TX, RX window)
    ├── mesh.c/.h         # flood + TTL + dedup + neighbor table
    └── mesh_metrics.c/.h # Memfault device identity + custom metrics
```

## Build and flash

Use the v3.3.0 toolchain environment (via `nrfutil toolchain-manager launch`,
the nRF Connect for VS Code extension, or a sourced `zephyr-env.sh`).

```sh
# EU carrier
west build -p -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-eu.conf

# US carrier
west build -p -b nrf9151dk/nrf9151/ns -- -DEXTRA_CONF_FILE=overlay-us.conf

west flash
```

Set your Memfault project key at build time (or edit `prj.conf`):

```sh
west build -p -b nrf9151dk/nrf9151/ns -- \
  -DEXTRA_CONF_FILE=overlay-eu.conf \
  -DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"YOUR_PROJECT_KEY\"
```

To build several distinct node IDs for a readable test topology, override
`CONFIG_DECT_MESH_NODE_ID` per board (for example `-DCONFIG_DECT_MESH_NODE_ID=1`,
`=2`, `=3`). Leaving it at `0` derives a unique ID from each board's hardware ID.

## Running a mesh

Flash two or more boards and open a serial terminal (115200 8N1) on each. You
will see originations, relays, and deliveries:

```
[00:00:00] <inf> main: DECT NR+ mesh node starting
[00:00:00] <inf> dect_phy: DECT NR+ PHY ready, device ID 4661, carrier 1677
[00:00:00] <inf> mesh: Mesh node 0x1235 up (TTL 4, frame max 32, payload max 20)
[00:00:10] <inf> mesh: Originate msg_id 1 -> 0xffff (ttl 4): "node 1235 #1"
[00:00:13] <inf> mesh: Deliver msg_id 7 from 0x00a3 (2 hops, RSSI -72 dBm): "node 00a3 #7"
[00:00:13] <dbg> mesh: Relay msg_id 7 (ttl 2, hop 2)
```

To prove multi-hop, place a third node out of radio range of the first so it can
only be reached via the middle node, and watch the hop count climb on delivery.
Tune `CONFIG_DECT_MESH_TX_POWER` down to force shorter links on a bench.

## Memfault integration and the fleet

Because a DECT NR+ PHY node has **no IP stack**, Memfault data cannot be posted
over HTTP. Data is collected on-device and exported out-of-band — the standard
pattern for non-IP and mesh devices:

1. **Collect on-device.** `CONFIG_MEMFAULT=y` enables reboot tracking,
   RAM-backed coredumps, log capture, and heartbeat metrics — including the
   custom mesh metrics below.
2. **Export chunks over the shell.** On a node's serial console:
   ```
   mflt export
   ```
   prints base64 chunks. Test the path first with `mflt test assert` (triggers a
   coredump on the next boot), then `mflt get_core` and `mflt export`.
3. **Upload to the fleet.** Pipe the chunks to Memfault with the project key,
   for example via `memfault-cli`'s `post-chunk`, or the chunks HTTP API. The
   device appears in your project on first upload.

The *mesh-native* path (left as an extension): designate one node as a
**gateway** (`CONFIG_DECT_MESH_ORIGINATE_INTERVAL_SECONDS=0`), relay each node's
chunks hop-by-hop to it, and have the gateway dump them over UART to a host that
forwards them to the chunks API.

### Custom metrics (`config/memfault_metrics_heartbeat_config.def`)

| Metric             | Type     | Meaning                                       |
| ------------------ | -------- | --------------------------------------------- |
| `dect_originated`  | counter  | messages this node originated per interval    |
| `dect_relayed`     | counter  | messages relayed onward per interval          |
| `dect_delivered`   | counter  | messages delivered to this node per interval  |
| `dect_dups_dropped`| counter  | duplicate frames suppressed per interval      |
| `dect_tx_failures` | counter  | failed PHY transmissions per interval         |
| `dect_neighbors`   | gauge    | one-hop neighbors seen within the timeout     |
| `dect_max_hops`    | gauge    | highest hop count observed                    |
| `dect_rx_rssi_dbm` | gauge    | RSSI of the most recently received frame      |

The heartbeat interval is **60 s** for the demo
(`config/memfault_platform_config.h`); a production fleet typically uses 3600 s.

## Key configuration (`menuconfig` → "DECT NR+ Mesh")

| Option                                  | Default | Purpose                              |
| --------------------------------------- | ------- | ------------------------------------ |
| `DECT_MESH_CARRIER`                     | 0       | regional channel (set via overlay)   |
| `DECT_MESH_NETWORK_ID`                  | 91      | shared mesh network ID               |
| `DECT_MESH_NODE_ID`                     | 0       | node address (0 = from hardware ID)  |
| `DECT_MESH_DEFAULT_TTL`                 | 4       | max relay hops                       |
| `DECT_MESH_ORIGINATE_INTERVAL_SECONDS`  | 10      | origination cadence (0 = relay only) |
| `DECT_MESH_RX_WINDOW_SECONDS`           | 3       | receive window per cycle             |
| `DECT_MESH_DEDUP_CACHE_SIZE`            | 32      | loop-suppression memory              |
| `DECT_MESH_TX_POWER`                    | 13      | transmit power index                 |

## References

- nRF Connect SDK: DECT NR+ protocol overview and `dect/dect_phy/hello_dect`.
- nrfxlib DECT NR+ PHY API (`nrf_modem_dect_phy.h`).
- Memfault: Nordic nRF Connect SDK integration guide; MCU test commands.
