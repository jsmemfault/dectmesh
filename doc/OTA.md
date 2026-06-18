# Firmware OTA over 9P — *the update mechanism is a file write*

This is one of the project's headline capabilities, and a deliberate showcase of what
9P buys you: **full firmware update of a chip that has no USB, over USB, with no DFU
protocol at all.** You don't speak mcumgr/SMP, you don't ship a transfer tool, you don't
write host code. You **write a file**, and the same `write` updates the local chip
(`/dev/fw5340`) and the remote, USB-less chip (`/dev/fw9151`) *identically* — because to
9P they're just two files in one namespace.

```
host  ──9P/USB──▶  nRF5340 relay  ──9P/UART──▶  nRF9151 mesh node
                   (9P server)      (9P client)   (/dev/firmware = its MCUboot DFU)
                        └── re-exports the 9151's DFU as /dev/fw9151 ──┘
```

Write the image to `/dev/fw9151`, write `/dev/reboot9151`, and the relay watches the new
version boot, verifies it's healthy, and **auto-confirms** it. If it didn't come up,
MCUboot rolls back. No debugger, no mcumgr, no bespoke transfer code. (This is the same
"everything is a file" model that will carry **observability** next — health, metrics,
and topology read as files the same way firmware is written as one.)

---

## TL;DR — the reliable recipe

The image is streamed with **`tools/aether_conv --put` in 1 KB paced chunks** — *not*
`9p write`. See [Why `--put` and not `9p write`](#why---put-and-not-9p-write) below; this
is the single most important thing to get right.

```sh
cc -O2 -o tools/aether_conv tools/aether_conv.c          # once

# 1. Bridge the 9P port (the CDC port whose name ends in 3). Pin it explicitly —
#    do NOT glob "*3": that also matches a DK J-Link serial like ...707993.
PORT=/dev/cu.usbmodem1203                                 # match your node's *3 port
pkill -f 'socat.*usbmodem'; sleep 2
socat UNIX-LISTEN:/tmp/9p.sock,fork "$PORT",rawer &  sleep 3

# 2. Stream the signed image as the FIRST AND ONLY op on this fresh session.
#    (Do NOT run a 9p/p9do read first — see "Gotchas".)
tools/aether_conv /tmp/9p.sock --put dev/fw9151 \
    dect_mesh/build_thingy/dect_mesh/zephyr/zephyr.signed.bin 1024
#    → "[done] wrote dev/fw9151"  (~9.5 s for a 276 KB image)

# 3. Trigger the MCUboot swap (the reply hangs as the node resets — that's benign).
tools/p9do /tmp/9p.sock ws:dev/reboot9151:1

# 4. Wait ~55–60 s for the swap + the relay to re-attach. Just wait; don't poll.

# 5. Confirm so it can't roll back (the relay also auto-confirms if healthy).
tools/p9do /tmp/9p.sock ws:dev/confirm9151:1 rd:dev/fw9151
#    → state idle  current <new>+0  confirmed yes
```

For a node attached directly over its own USB the `*3` port is the 9P server. Port map
(relay CDC): `*01` = 9151 console · `*03` = 9P · `*05` = 5340 console.

---

## Why `--put` and not `9p write`

Conceptually the OTA *is* `9p write /dev/fw9151 < image` — that's the whole value-prop and
it's true. But the inter-chip link between the relay (nRF5340) and the mesh node (nRF9151)
is a **1 Mbps UART**, and the 9151 stalls that UART's RX while it erases/writes each flash
page. A client that streams the image in large bursts **overruns** that UART mid-write and
the relay returns `Rerror: write failed` partway through (and a partial image latches the
DFU in `error`). This is exactly the overrun `dect_mesh/app.overlay` warns about.

`tools/aether_conv --put <path> <file> 1024` avoids it: it holds **one** 9P session and
sends the image in **1 KB chunks, each a Twrite→Rwrite round-trip**. The round-trip is
natural flow control — the next chunk doesn't go out until the 9151 has acked the last
flash write — so the UART never sees a sustained burst. `1024` is the proven chunk size;
it is *not* a knob to tune up. `9p write` (≈8 KB Twrites) and `p9do wf:` (4 KB) both burst
too hard and truncate the image → MCUboot rejects it → the node silently boots the old
version. Use `--put`.

## Gotchas (each of these has bitten us)

- **Never do a `9p`/`p9do` *read* immediately before the `--put`.** Each tool invocation
  opens/closes the CDC port, toggling DTR; the relay's 9P **session pool is size 1 and
  DTR-gated**, so a back-to-back tool handoff churns the session and **EPIPEs the write**
  (`write failed` partway, or at offset 0 if badly churned). Run `--put` as the *first and
  only* op on a freshly-bridged socket. If you want to check the version/link first, do it,
  then **re-bridge** (`pkill socat; socat …; sleep 3`) before the `--put`.
- **Pin the `*3` port; don't glob `*3`.** A connected Nordic DK shows up as a J-Link with a
  serial like `...707993`, which also ends in `3` and sorts *first* — `ls /dev/cu.usbmodem*3
  | head -1` will grab the DK, not your node.
- **A failed write latches the 9151 DFU in `state error`.** Clear it before retrying with
  `ws:dev/reboot9151:1`, or — if the relay↔9151 link is wedged so 9P can't reach it —
  `kernel reboot cold` on the **9151 console** (`*01`). A partial image in the secondary
  slot is harmless: MCUboot ignores it and keeps running the good primary.
- **`,rawer` on the socat address is required** (no tty line discipline mangling 9P bytes).
- **macOS has no `timeout`** (it's `gtimeout`); guard long ops with
  `perl -e 'alarm N; exec @ARGV' <cmd>` instead.
- **Wait the full ~55–60 s after `reboot9151`** (MCUboot swap + relay re-attach). Don't poll
  the link in a tight loop — give it the settle time.

## Versioning

`CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` (in `dect_mesh/prj.conf`) and the `dect_mesh/VERSION`
file must move **together** (banner version == OTA image version). For a real change, **bump
the patch level** (e.g. `0.7.20 → 0.7.21`) so the update is a proper upgrade and the boot
banner alone identifies the build. Shipping a changed build under the *same* version only
"works" because downgrade-prevention is off, and it makes the version string ambiguous.

## Verifying the new image is actually running

`dev/fw9151` reports `state / current / pending / confirmed`, but only the (pinned) version
string. The unambiguous check is the **boot banner on the 9151 console (`*01`)**:

```
*** Booting DECTstrous Mesh v0.7.21-<git-hash> ***
```

`<git-hash>` is `git describe` of this repo at build time, so a changed hash proves the new
image swapped in even if the version string didn't change.

## What the files mean

| File (`/dev/…`) | Meaning |
|---|---|
| `fw9151` | the mesh node's running/pending firmware (proxied from its `/dev/firmware`) — **write the signed image here** |
| `reboot9151` | reboot the 9151 → MCUboot evaluates the secondary slot and swaps |
| `confirm9151` | confirm the running image (no rollback on next boot) |
| `fw9151auto` | the relay's auto-confirm policy |
| `link9151` | live relay↔9151 link health (`up/down`, relink attempts, last contact) |
| `fw5340` | the relay's *own* firmware — the **same** `--put`/write OTA, local chip |

See also: [`README.md`](../README.md) (the 9P value-prop), `doc/NET_AETHER_SPEC.md`
(the `/net/aether` datagram service), and `flash-thingy.sh` (one-time J-Link bring-up).
