# Firmware OTA over 9P — *the update mechanism is a file write*

One of the project's headline capabilities, and a deliberate showcase of what 9P buys
you: **full firmware update of a chip that has no USB, over USB, with no DFU protocol at
all.** You don't speak mcumgr/SMP, you don't ship a transfer tool, you don't write host
code. You **write a file** — and the same `write` updates the local chip (`/dev/fw5340`)
and the remote, USB-less chip (`/dev/fw9151`) *identically*, because to 9P they're just
two files in one namespace.

```
host  ──9P/USB──▶  nRF5340 relay  ──9P/UART──▶  nRF9151 mesh node
                   (9P server)      (9P client)   (/dev/firmware = its MCUboot DFU)
                        └── re-exports the 9151's DFU as /dev/fw9151 ──┘
```

Write the image to `/dev/fw9151`, write `/dev/reboot9151`, and the relay watches the new
version boot, verifies it's healthy, and **auto-confirms** it. If it didn't come up,
MCUboot rolls back. No debugger, no mcumgr, no bespoke transfer code. (This is the same
"everything is a file" model that carries **observability** next — health, metrics, and
topology read as files the same way firmware is written as one.)

---

## The recipe — just write the file

Plain `9p write` works. **No chunking, no `--put`, no throttling on the client side** — the
relay rate-matches the transports internally (see [How it stays reliable](#how-it-stays-reliable)).
Requires **relay ≥ 0.38.17**.

```sh
NINEP=~/src/plan9port/bin/9p

# Bridge the 9P CDC port (the one whose name ends in 3). Pin it explicitly: do NOT glob
# "*3" -- that also matches a DK J-Link serial like ...707993, which sorts first.
PORT=/dev/cu.usbmodem1203                                  # your node's *3 port
pkill -f 'socat.*usbmodem'; sleep 2
socat UNIX-LISTEN:/tmp/9p.sock,fork "$PORT",rawer &  sleep 3

# 1. Stream the signed image. One ordinary write; the relay paces the inter-chip UART.
$NINEP -a unix!/tmp/9p.sock write /dev/fw9151 < dect_mesh/build_thingy/dect_mesh/zephyr/zephyr.signed.bin

# 2. Swap, then confirm (the relay also auto-confirms if healthy).
echo 1 | $NINEP -a unix!/tmp/9p.sock write /dev/reboot9151   # MCUboot swaps the new image
#   ... wait ~55-60 s for the swap + the relay to re-attach ...
echo 1 | $NINEP -a unix!/tmp/9p.sock write /dev/confirm9151  # make it permanent (no rollback)
```

`/dev/fw5340` (the relay's own firmware) is updated the same way — write it, then
`/dev/reboot` + `/dev/confirm`. Port map (relay CDC): `*01` = 9151 console · `*03` = 9P ·
`*05` = 5340 console.

## How it stays reliable

The host↔relay link is **USB-CDC** (flow-controlled — the host can't outrun the relay).
The relay↔9151 link is a **raw 1 Mbps inter-chip UART with no RTS/CTS**, and the 9151
stalls that UART's RX while it erases/writes each flash page. So a single large `Twrite`
forwarded *whole* to the 9151 used to overrun its RX and drop bytes → `write failed`
(empirically: ~4 KB squeaked through, ~8 KB failed).

The fix (relay **0.38.17**, `fw9151_write`): the relay forwards each host write to the 9151
in **bounded sub-writes** (`CONFIG_NINEP_REMOTE_FS_WRITE_CHUNK`, 2048 B), each its own
round-tripped `Twrite`. The round-trip *is* the flow control — the 9151 never sees an
oversized burst. The chunking lives in the **bridge**, where the two transports meet, not
in the client. (An async/EasyDMA RX path was tried first, but enabling `UART_ASYNC_API` on
the nRF91 grabs a peripheral the DECT modem needs → boot loop; relay-side rate-matching is
the fix that fits this SiP.)

**Older relay (< 0.38.17)** without the fix: stream client-side instead, with
`tools/aether_conv --put dev/fw9151 <…/zephyr.signed.bin> 1024` (paced 1 KB chunks).

## OTA over the mesh — no wires to the target at all

The same "update is a file write" idea, but the file being written lives on a **remote node
reachable only over the DECT NR+ mesh**. The gateway's 9151 runs an in-process 9P *client*
session to a peer's mesh 9P server and streams the signed image straight into the peer's
`dev/firmware` — then commits, reboots, and confirms it, **all over the air**. No USB, no
debugger, no wires to the target. The peer needs **zero** new code: every node already
serves its own `dev/firmware`/`dev/reboot`/`dev/confirm` over the mesh (`aether_9p_mesh`).

```
host ─9P/USB─▶ gateway 5340 ─9P/UART─▶ gateway 9151 ─mesh 9P (reliable unicast)─▶ peer 9151
                                        (in-proc 9P client)      over the air        (dev/firmware)
```

### The recipe

```sh
# One process, one connection drives the whole cycle (target → stream → commit → reboot):
socat UNIX-LISTEN:/tmp/9p.sock,fork /dev/cu.usbmodem1203,rawer &  sleep 2   # gateway *3 port
tools/aether_conv /tmp/9p.sock --mesh-ota <peer_eui12> dect_mesh/build_thingy/.../zephyr.signed.bin 1024
#   ... peer boots the new image ...
tools/p9do /tmp/9p.sock "ws:dev/push_ctl:confirm <peer_eui12>"   # confirm OVER THE MESH (no rollback)
```

`--mesh-ota` is **resumable**: on a transient mesh wedge it backs up, lets the mesh settle,
re-targets, and continues — the peer dedups the overlap (idempotent `dfu_write`). A clean
push completes in **one pass** (~305 KB in ~56 s single-hop). `tools/mesh_ota_verify.sh <N>`
runs a full hands-off cycle and returns PASS/FAIL by reading the peer's boot banner.

### Reliability

Measured **15/15 full-image (305 KB) OTAs over the mesh, 0 failures** on a healthy bench
(2026-07-30): 14 back-to-back version bumps (0.7.70→0.7.83), all single-pass, ~51–108 s,
**1 retry total**; plus one deliberately **interrupted-at-51%-then-resumed** push that still
produced a **MCUboot-validated, bootable** image. Three target-side fixes make it correct:

- **exactly-once `dfu_write`** (idempotent by offset) — a re-sent chunk after a lost ACK is
  skipped, not appended twice, so the image can't shift.
- **ack-gate backpressure** — the peer refuses a 9P T-frame *before* ACKing when its request
  ring is full, so the sender's ARQ retransmits instead of the chunk being silently dropped.
- **finalize decoupled from session-reset** — a resume re-versions the peer (Tversion), which
  clunks the open DFU fid; that clunk no longer *finalizes* the partial image. Only an
  explicit commit finalizes. (This is what makes a resumed image validate instead of corrupt.)

And one gateway-side fix: the DFU-over-mesh path does **not** quiesce the mesh (the mesh *is*
the transport here), unlike the wired path which still quiesces during a flash write.

**Scope of the claim:** single-hop, single RF environment, N=15. Multi-hop OTA *through* a
relay node and thousands-of-cycles endurance are not yet characterized. Sustained heavy load
can still degrade a bench over many pushes — a power-cycle resets it; the resumable path
rides through transient wedges.

### Confirm-over-mesh + reboot notes
- After any swap the new image boots in **test mode** (`confirmed no`) and reverts on the next
  reboot unless confirmed. Confirm over the mesh: `ws:dev/push_ctl:confirm <eui>`.
- Reading the peer's `dev/fw9151` immediately after its reboot can race the relay re-attach
  (empty read); retry a few times with a short settle.

## Gotchas

- **Pin the `*3` port; don't glob `*3`** — a connected Nordic DK is a J-Link with a serial
  like `...707993` (ends in `3`, sorts first), so `ls /dev/cu.usbmodem*3 | head -1` grabs
  the DK, not your node.
- **Hold ONE socat bridge for the whole sequence — do NOT cycle it.** This is the single
  biggest reliability lesson, and it is about the **host / USB-CDC side, not the mesh or 9P**
  (the 9P stack + the 0.38.17 flow-control fix are robust). Bring up one
  `socat UNIX-LISTEN:/tmp/9p.sock,fork <port>,rawer`, run *all* ops against it
  (write → reboot → confirm → any reads), then tear it down **once** at the end. Do **not**
  `pkill socat; socat …` between operations: every socat open/close toggles **DTR**, which
  churns the relay's **size-1, DTR-gated session pool** and progressively wedges the USB-CDC
  peripheral → `operation not permitted` on open, read/write timeouts, `bad rpc tag` desync.
  These *look* like mesh-load or link failures but are pure local-port abuse — they vanish
  with a single held bridge (proven: after dozens of failures from cycling, a node OTA'd
  clean on one held bridge with the mesh fully active). `socat …,fork` keeps the port (and
  DTR) asserted across multiple client connections, so several `9p`/`p9do` invocations over
  the *same* bridge are fine; it is **restarting socat** that hurts. Also avoid gratuitous
  diagnostic reads that add churn around the write. If the relay's session is *already*
  wedged (reads time out even on a fresh bridge), **power-cycle the node** to reset it.
- **A failed write latches the 9151 DFU in `state error`.** Clear it with
  `9p write /dev/reboot9151` (or `kernel reboot cold` on the 9151 console `*01` if the 9P
  link is wedged) before retrying. A partial image in the secondary slot is harmless —
  MCUboot ignores it and keeps the good primary.
- **`,rawer` on the socat address is required** (no tty line discipline mangling 9P bytes).
- **macOS has no `timeout`** (it's `gtimeout`); guard long ops with
  `perl -e 'alarm N; exec @ARGV' <cmd>`.
- **Wait the full ~55–60 s after `reboot9151`** (MCUboot swap + relay re-attach). Don't poll.

## Versioning

`CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` (in `dect_mesh/prj.conf`) and the `dect_mesh/VERSION`
file must move **together** (banner version == OTA image version). For a real change, **bump
the patch level** (e.g. `0.7.21 → 0.7.22`) so the update is a proper upgrade and the boot
banner alone identifies the build. Shipping a changed build under the *same* version only
"works" because downgrade-prevention is off, and it makes the version string ambiguous.

## Verifying the new image is actually running

`dev/fw9151` reports `state / current / pending / confirmed`, but only the (pinned) version
string. The unambiguous check is the **boot banner on the 9151 console (`*01`)**:

```
*** Booting DECTstrous Mesh v0.7.22-<git-hash> ***
```

`<git-hash>` is `git describe` of this repo at build time, so a changed hash proves the new
image swapped in even if the version string didn't change.

## What the files mean

| File (`/dev/…`) | Meaning |
|---|---|
| `fw9151` | the mesh node's running/pending firmware (proxied from its `/dev/firmware`) — **write the signed image here** |
| `reboot9151` / `confirm9151` | reboot the 9151 (→ MCUboot swap) / confirm the running image (no rollback) |
| `fw9151auto` | the relay's auto-confirm policy |
| `link9151` | live relay↔9151 link health (`up/down`, relink attempts, last contact) |
| `fw5340` / `reboot` / `confirm` | the relay's *own* firmware + its reboot/confirm — same file-write OTA, local chip |

See also: [`README.md`](../README.md) (the 9P value-prop) and `doc/NET_AETHER_SPEC.md`.
