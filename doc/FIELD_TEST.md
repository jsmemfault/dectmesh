# Field test cheat-sheet — prove multi-hop (Milestone 1)

Keep this on the laptop/cyberdeck during the run. Goal: demonstrate a **relayed 2-hop
path** — the project's headline-unproven claim — over real neighborhood distance.

**Deployed firmware (all 3 nodes):** universal `dect_mesh` image, **TX power 13** (band-4
max; 14/15 error out), with the `dect txpower` runtime lever and multi-hop broadcast/chat.
Self-organizing — no per-node config.

## Topology
```
  Node 1 (Thingy B692)            DK (relay)              Node 3 (Thingy 6F18)
  desk window, end of street      stop sign ~1/4 mi       cross-street ~1/4 mi
  USB-tethered (live vantage)     external battery        battery + cyberdeck
        \________ ~1/4 mi ________/   \____ ~1/4 mi, around the wooded hill ____/
                  (in range)                      (in range)
        \_______________ NOT in range (hillside isolates the ends) _____________/
```
The **wooded hillside** isolating Node 1 from Node 3 is what forces the relay — outdoor
line-of-sight barely attenuates, so rely on terrain/buildings, not distance alone. The DK
in the middle is the **cut vertex**: whoever wins root, the DK relays between the two ends.

## ⚠️ THE GOLDEN RULE (or you'll fight ghosts all day)
**One `socat` bridge, held — never cycle it.** Bring it up once, run every command over it,
tear down once. Do NOT `pkill socat; socat …` between commands. Repeated open/close toggles
DTR, wedges the USB-CDC peripheral, and gives `operation not permitted` / timeouts / `bad
rpc tag` that *look* like mesh failures but are local-port abuse. Same for the console —
don't hammer it with reconnects. If a port wedges, **power-cycle that node.** (See
`doc/OTA.md`.)

```sh
NINEP=~/src/plan9port/bin/9p
P=/dev/cu.usbmodem*3            # Node 1's 9P port (the *3 one); pin it, don't glob loosely
socat UNIX-LISTEN:/tmp/9p.sock,fork "$P",rawer &   # ONE bridge, leave it up
```

## Setup
1. Power on all three. Give ~60 s to self-organize.
2. From Node 1's **console** (`screen /dev/cu.usbmodem*5` or the `*1` 9151 console): `aether tree`.
3. Walk the DK out to the stop sign, then Node 3 around the cross-street/hill. Re-check `aether tree` as they join.

## The proofs (observe from Node 1 + cyberdeck; the DK can stay dark)

**1. Multi-hop routing** — from Node 1:
```
aether status
```
- ✅ **neighbors list does NOT include Node 3** (the ends can't hear each other), but includes the DK.
- ✅ **routes shows Node 3 at `hops 2`** (via the DK). ← multi-hop, proven.

**2. Party-line chat across the relay** (the money shot) — from Node 1 console:
```
aether chat hello from the desk
```
- ✅ appears on **Node 3** (cyberdeck: `9p read dev/aether/chat`) — impossible without the DK relaying, since the ends are isolated.
- Reverse it: from the cyberdeck, `echo 'hi from the corner' | 9p write dev/aether/chat` → ✅ shows on Node 1.

**3. Datagram reliability over 2 hops** — from Node 1's 9P (held bridge):
```
tools/aether_conv /tmp/9p.sock <node3's dev/aether/addr (HONR routing address)>  # send; ARQ ACKs round-trip via the DK
# NOT node3's net/aether/addr (durable identity) -- unicast dst must be the
# HONR/routing address; the identity address won't route.
```
- ✅ high delivery % over the relayed path; note retries.

**4. Self-heal** — pull the **DK's** power:
- ✅ Node 3 loses its only path, **orphans, and re-elects** (watch Node 3 via cyberdeck / Node 1's tree re-form). Time it. Restore the DK → re-converges.

**5. Link metrics** (run the whole time, on Node 1):
```
tools/field_metrics.sh /dev/cu.usbmodem*1 10 field_run.csv
```
- Logs RSSI/SNR, routes/hops, **`fwd`** (DK relay count when you read it back), tx errors. Timestamp-correlate with position/WiFi-AP logs.

## Tuning (if the topology won't form)
- **Ends still hear each other** (no relay forced) → `dect txpower 11` (or lower) on the nodes; reposition behind more obstruction.
- **A hop won't close** (Node can't reach the DK) → `dect txpower 13` (max) and/or move it closer.
- `dect txpower` is **not persisted** — re-set after any power-cycle (or it's the baked-in 13).

## Capturing location (simplified — log, don't resolve)
At each node's spot, have the cyberdeck record **(its GPS position, visible WiFi AP BSSIDs+RSSI, the node's mesh state)**. Offline, correlate AP scans → position. No live nRF Cloud needed for Milestone 1.

## If something wedges
- Port `operation not permitted` / hangs → you cycled it; **power-cycle the node**, then one held bridge.
- A node dropped off mesh → check `dect stats` (tx err should be 0 at pwr 13; LBT `busy` is normal), confirm carrier 538.
- Node 3's inter-chip 9P link is the flaky one under load — fine for meshing; only matters if you try to OTA/observe it over USB while it's busy.

---
*Reference: `doc/OTA.md` (port discipline), `dect_mesh/` (firmware), `tools/field_metrics.sh`, `tools/aether_conv.c`.*
