# DECTstrous — a DECT NR+ mesh you can `cat`

A self-organizing, self-healing multi-hop radio mesh on Nordic **nRF9151**, built
directly on the **DECT NR+ physical layer** (ETSI TS 103 636-2) — and wrapped so
that its **entire control surface is a 9P filesystem**: live mesh state, firmware,
link health, **cryptographic identity**, and control are all just *files* you read and
write with generic, decades-old tooling, over USB / BLE / UART / TCP / or across the mesh itself.

> **Why "DECTstrous"?** The name is a three-way wink — **DECT** + *disastrous* + *dexterous*.
> Two MCUs, four transports, a self-healing multi-hop mesh, over-the-air updates, and live
> fleet telemetry — a pile of moving parts with every right to be the middle word. It's the
> last one instead, and the reason has a name: **9P.** The same read/write verbs compose a
> console, a firmware slot, a chat room, a coredump stream, cryptographic identity, and a
> node two hops away — one hand, many tricks.  **DECTstrous by name, dexterous by 9P.**

Two chips on a Thingy:91 X, two apps:

- **`DECTstrous Mesh`** (nRF9151) — the Æther/HONR mesh on the DECT NR+ PHY, with an
  in-process **9P server** (`/net/aether` = live mesh, `/dev/firmware` = its own DFU).
- **`DECTstrous Relay`** (nRF5340) — a **9P aggregator**: a 9P *client* to the 9151
  over the inter-chip UART, and a 9P *server* to the host over USB-CDC. It re-exports
  the 9151's firmware and adds its own control/health/OTA nodes — so a chip with **no
  USB** is fully manageable, and field-updatable, over USB.

```
Host (Mac / cyberdeck) ── plan9port: 9p, socat
   │
   │  USB-C · 3× CDC ACM
   ▼
nRF5340 · “DECTstrous Relay” · 9P AGGREGATOR (server to host ⇄ client to 9151)
   ├─ cdc0 → nRF9151 console (bridged)
   ├─ cdc1 → 9P server → /dev/fw5340      (this chip's own firmware, USB self-DFU)
   │                     /dev/fw9151      (the 9151's firmware, proxied — OTA)
   │                     /dev/link9151    (live 5340⇄9151 link health)
   │                     /dev/reboot9151  /dev/confirm9151  /dev/fw9151auto
   ├─ cdc2 → console + interactive shell  (kernel · device · mflt · mflt_nrf)
   └─ 9P client ────────┐  over uart1
                        │  9P over UART
                        ▼
nRF9151 · “DECTstrous Mesh” · 9P SERVER (in-process with the mesh)
   ├─ /net/aether    datagram service + identity: clone → per-conversation ctl/data,
   │                 addr (self-certifying CGA) · prove (sign a challenge to own it)
   │                 (Plan 9 /net-style; doc/NET_AETHER_SPEC.md) — proxied to the host
   ├─ /dev/aether    node state: addr · rank · tree · neighbors · routes · chat
   ├─ /dev/firmware  its own MCUboot DFU  (re-exported by the relay as /dev/fw9151)
   ├─ Æther / HONR self-organizing, self-healing routing
   └─ DECT NR+ PHY radio  )))  ─────▶  other DECTstrous nodes
```

> **The full picture** — firmware per chip/core (5340 app + net, 9151 app + modem), the
> software stack, and every transport — is diagrammed in [`doc/ARCHITECTURE.md`](doc/ARCHITECTURE.md).

## Why 9P? (this is the multiplier)

Everything above is a **file** in a 9P namespace — not as a metaphor, but literally:
real files served by the same tiny protocol (~a dozen message types), reachable with
stock tooling (`9p read`, `9p write`, `9p ls` from plan9port; `mount` on Plan 9 / Linux).
That one decision pays off four ways:

**1. One interface, not N.** Telemetry, firmware, and control would normally be three
separate subsystems — a custom RPC for mesh state, mcumgr/SMP for DFU, shell commands
for control. Here they are one namespace:

```sh
9p read  /dev/aether/neighbors      # live mesh state
9p ls    /net/aether                # datagram service: clone · status · addr
9p write /dev/fw9151 < image.signed.bin   # firmware update (just a file write; doc/OTA.md)
9p read  /dev/link9151              # link health
9p write /dev/reboot9151            # control
```

Add a capability → add a file. No new protocol, no new client. `ls /dev` *is* the
feature list — the control plane is self-describing.

**2. Bridging is free — it's what 9P was born to do.** The nRF9151 has no USB; the
nRF5340 does. So the relay **mounts** the 9151's namespace (it's a 9P client over the
inter-chip UART) and **re-exports** it to the host (it's a 9P server over USB). The host
sees `/dev/fw9151` and never knows it's being proxied across a UART to a second chip.
Per-process namespaces and transparent re-export are the *original* Plan 9 design — so
the whole gateway falls out for free. Want the mesh tree on the host too? Proxy one more
subtree; it's the same three lines.

> The headline this buys us: **full firmware OTA of a chip that has no USB, over USB,
> with zero DFU protocol — just 9P proxying a file write.** Write the image to
> `/dev/fw9151`, write `/dev/reboot9151`, and the relay verifies the new version came up
> healthy and **auto-confirms** it. If it didn't, MCUboot rolls back. No J-Link, no
> mcumgr, no bespoke transfer code — and the same `write` updates the local chip
> (`/dev/fw5340`) and the remote chip (`/dev/fw9151`) identically.
>
> A plain `9p write` does it — the relay rate-matches the two transports internally
> (it paces the raw inter-chip UART so the host never has to), so there's no client-side
> chunking or throttling. **Full step-by-step recipe + verification:
> [`doc/OTA.md`](doc/OTA.md).**

**3. Transport-agnostic — the radio becomes a mount point.** The same 9P runs over UART,
USB-CDC, BLE L2CAP, TCP, and across the DECT mesh. Swap the transport; the semantics
don't move. `/net/aether` served over LoRa, DECT NR+, or a future sub-GHz NR+ is the
*same mount* to the client — the PHY is an implementation detail below the filesystem.

**4. Generic tooling, zero custom host app.** `ls`, `cat`, `mount`, a shell pipe. No SDK,
no app to ship, no schema to version. A protocol from 1992 drives a microcontroller radio
mesh because "everything is a file" never went out of style.

*Could you build all this another way?* Of course — bespoke RPC **plus** mcumgr **plus**
shell glue. But that's three protocols, three client tools, and three things to keep in
sync as the system grows. 9P collapses them into one namespace with one small verb set,
and hands you proxying, transport-swapping, and standard tooling as **properties of the
model** rather than features you have to write. That's the multiplier.

## What you can actually do (today, over USB, no debugger)

```sh
# one socat per op:  socat UNIX-LISTEN:/tmp/9p.sock /dev/cu.usbmodem*103,rawer &
9p -a unix!/tmp/9p.sock ls /dev                 # the control surface
9p -a unix!/tmp/9p.sock read  /dev/link9151      # link: up · relinks · last_contact
9p -a unix!/tmp/9p.sock read  /dev/fw9151        # the 9151's running/pending version
9p -a unix!/tmp/9p.sock write /dev/fw9151 < dect_mesh/build_thingy/dect_mesh/zephyr/zephyr.signed.bin  # OTA
9p -a unix!/tmp/9p.sock write /dev/reboot9151    # → swap → relay auto-confirms if healthy
```

> **Firmware OTA is a file write too** — `9p write /dev/fw9151 < image` then
> `9p write /dev/reboot9151`. The relay paces the raw inter-chip UART internally (it chunks
> the forward), so a plain `9p write` streams through with no client-side throttling. Full
> recipe + verification: **[`doc/OTA.md`](doc/OTA.md)**.

Plus an interactive shell on cdc2 (the `*105` port) — `kernel`, `device list`, and the
full Memfault group (`mflt export`, `mflt get_core`, `mflt get_reboot_reason`, …).

And the punchline: **the exact same `/dev` tree is served over BLE L2CAP at the same time**
(CoC, PSM `0x0080`). Point a BLE L2CAP 9P client at it and you get the *identical* namespace
with zero relay-side changes — one filesystem, two live transports. That's not a second
implementation; it's the same `fw_sysfs` handed to a second session pool. (The relay registers
both at boot: `9P re-export on USB (cdc_acm_uart1) AND BLE L2CAP (PSM 0x0080)`.)

## Talking to it from the host (socat + plan9port)

The relay serves **raw 9P over a USB-CDC ACM port** — there's no IP layer, so you bridge that serial
port to a Unix socket with **socat**, then point **plan9port's `9p`** client at the socket.

**Prereqs:** [plan9port](https://9fans.github.io/plan9port/) (provides the `9p` tool) and `socat`.

**Ports** (the Thingy re-enumerates as `11xx` *or* `21xx` across reboots — match by suffix):

| Port suffix | Role |
|---|---|
| `cu.usbmodem*103` | **9P server** — the `/dev` tree |
| `cu.usbmodem*105` | the 5340 console + interactive shell |
| `cu.usbmodem*101` | the 9151 console (bridged) |

**The bridge — one socat per operation.** The server starts a fresh 9P session (RX state + fid
namespace) per connection, so don't run a long-lived `fork` server; bridge, do one op, tear down:

```sh
NINEP=~/src/plan9port/bin/9p          # adjust to your plan9port path

np() {                                 # np ls /dev   ·   np read /dev/link9151   ·   np write ...
  local c; c=$(ls /dev/cu.usbmodem*103 | head -1)
  rm -f /tmp/9p.sock
  socat UNIX-LISTEN:/tmp/9p.sock "$c",rawer & local sp=$!
  sleep 1
  "$NINEP" -a unix!/tmp/9p.sock "$@"
  kill "$sp" 2>/dev/null
}

np ls   /dev                                          # the control surface
np read /dev/link9151                                 # link health
np read /dev/fw9151                                   # running / pending version
np write /dev/fw9151 < dect_mesh/build_thingy/dect_mesh/zephyr/zephyr.signed.bin   # OTA (relay paces the link)
echo 1 | np write /dev/reboot9151                     # → swap → relay auto-confirms if healthy
```

**Gotchas:** the `,rawer` on the socat address matters (no tty line-discipline mangling the 9P
bytes); use **one socat per op** (a persistent `fork` server desyncs the per-session RX); and
**macOS has no `timeout`** command (it's `gtimeout`) — don't wrap serial captures in it or they
silently no-op. The 5340 console/shell (`*105`) is a normal terminal: `screen /dev/cu.usbmodem*105`
or any serial monitor.

### Holding a `/net/aether` conversation (`tools/aether_conv.c`)

`/net/aether` is a Plan 9 `/net`-style datagram service: you **walk `clone`** to allocate a
conversation, then **keep that ctl fid open** while you walk `<N>/data` and read/write datagrams
(doc/NET_AETHER_SPEC.md). plan9port's `9p` opens-and-clunks per invocation, so it can't hold the
fid — it's fine for `ls /net/aether`, `read addr`, `read status`, but not the stateful dance. The
relay re-exports the whole dynamic tree from the 9151 transparently (a `remote_fs` union mount), so
a single fid-holding client drives it end to end:

```sh
cc -O2 -o /tmp/aether_conv tools/aether_conv.c
# ONE persistent connection (not one-socat-per-op) for a fid-holding client:
socat UNIX-LISTEN:/tmp/9p.sock,fork "$(ls /dev/cu.usbmodem*103|head -1)",rawer &
/tmp/aether_conv /tmp/9p.sock 00:00:00:00:00:01     # clone→ctl→walk N/data→connect→send
```

It validates the full path: `clone` allocates conversation N, holding the ctl fid keeps it alive
(`status` reports `1/4`), the deep walk to `N/data` succeeds, a `connect` ctl command + a datagram
`write` go out over the DECT mesh, and clunking ctl tears the conversation down cleanly (`0/4`).

### Proving a node owns its address (`net/aether/prove`)

A node's durable address is a **Cryptographically Generated Address**: `node_eui = SHA256(pubkey)[:6]`,
bound to a P-256 key the node generates once and persists in NVS. It's **self-certifying** — to trust
that an address belongs to a key, you hash the key; no PKI, no CA. And proving it is, of course, a file:
write a challenge to `net/aether/prove`, read back the node's public key and a signature over your nonce.

```sh
cc -O2 -o tools/aether_verify tools/aether_verify.c \
   -I$(brew --prefix openssl@3)/include -L$(brew --prefix openssl@3)/lib -lcrypto
tools/aether_prove.sh 1303        # write a nonce → read pubkey+signature → verify independently
# → address binding: MATCH · key possession: VALID · ✓ PROOF VALID
```

The verifier (`tools/aether_verify.c`) trusts nothing the node says — it checks `SHA256(pubkey)[:6]`
against the claimed address **and** the ECDSA-P256 signature. An impostor presenting a valid proof under
*another* node's address fails the binding; a replayed signature fails the challenge. Signing runs on the
9151's on-die **PSA/Oberon** crypto. Full transcripts (multi-hop, identity persistence, spoof rejection):
**[`doc/PROOF.md`](doc/PROOF.md)**.

## Layout

| Path | What |
|---|---|
| **`dect_mesh/`** | nRF9151 app (*DECTstrous Mesh*): Æther/HONR mesh on the DECT NR+ PHY + the `/net/aether` & `/dev/firmware` 9P server + Memfault |
| **`dect_relay/`** | nRF5340 app (*DECTstrous Relay*): the 9P aggregator — USB-CDC 9P server, 9P client to the 9151, OTA/self-heal/auto-confirm, shell |
| **`flash-thingy.sh`** | One-time J-Link bring-up of a new node (`relay` / `mesh`); after this, nodes update over USB/OTA |
| **`doc/ARCHITECTURE.md`** | **The moving parts** — system diagram, firmware per chip/core (5340 + 9151 + modem), the software stack, and transports |
| **`doc/OTA.md`** | **Firmware OTA over 9P** — the headline "update = a file write" capability: value prop, the plain-`9p write` recipe (the relay rate-matches the inter-chip UART), verification, and gotchas |
| **`doc/NET_AETHER_SPEC.md`** | The `/net/aether` datagram-service spec (Plan 9 `/net`-style clone/ctl/data) |
| **`doc/PROOF.md`** | Proof dossier — the on-hardware transcripts behind every claim (multi-hop, CGA persistence, ownership proof + spoof rejection) |
| **`doc/RF_CHARACTERIZATION.md`** | SDR measurement methodology + log — on-air behavior (duty cycle, occupied bandwidth, LBT) as the near-term regulatory rigor toward certification |
| **`doc/OBSERVABILITY.md`** | Fleet observability spec — the Memfault metrics/attributes/traces mapped to each claim, the 9P-chunk upload pipeline, and the dashboard a reviewer sees |
| **`tools/`** | Host-side 9P clients, proof/test scripts, and the native chat client (`tools/README.md`) |
| **`dectfw/`** | Licensed DECT NR+ PHY modem firmware (access-gated — **never commit**) |
| **`NRplus-mesh-*.md`** | Pitch / role / demo-script — the project one-pager and how I'd lead it |

Both apps consume two out-of-tree Zephyr modules: **aephyr** (the Æther/HeyMac/HONR mesh
stack + DECT NR+ PHY driver) and **9p4z** (the 9P library: UART, USB, BLE L2CAP, TCP).

## Build & flash

Built with the NCS v3.3.0 toolchain; both apps pull in the `aephyr` and `9p4z`
modules via `ZEPHYR_EXTRA_MODULES`:

```sh
# nRF9151 mesh  → build_thingy/
west build -d build_thingy -b thingy91x/nrf9151/ns dect_mesh \
  -- -DZEPHYR_EXTRA_MODULES="<path>/aephyr;<path>/9p4z"

# nRF5340 relay → build/   (sysbuild also builds the net-core controller)
west build -d build -b thingy91x/nrf5340/cpuapp --sysbuild dect_relay \
  -- -DZEPHYR_EXTRA_MODULES="<path>/aephyr;<path>/9p4z"

# First-time bring-up of a node, over J-Link (set SW2 per target):
./flash-thingy.sh <jlink-serial> mesh     # nRF9151 app  (SW2 → nRF91; program only)
./flash-thingy.sh <jlink-serial> relay    # nRF5340 app  (SW2 → nRF53; --recover if fresh)
```

After that one J-Link flash, a node never needs a debugger again: the relay self-DFUs over
USB (`/dev/fw5340`) and the mesh node updates over the air via 9P (`/dev/fw9151`). The DECT
NR+ PHY modem firmware (`dectfw/`) is a separate, deliberate one-time SWD step — and
`flash-thingy.sh` **refuses to `recover` the 9151** so it can never wipe that licensed image.

## Terminology & regulatory (it matters)

This uses only the **DECT NR+ PHY** (Nordic's PHY-only modem firmware,
`nrf_modem_dect_phy`). The MAC, routing (HONR), and self-organization are the **Æther**
stack — a *custom* protocol, **not** the DECT-2020 NR MAC, and therefore **not** a
certifiable "DECT NR+" product. See the regulatory section in the aephyr README.
