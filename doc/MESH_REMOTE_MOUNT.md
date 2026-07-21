# Modem-side mesh remote-mount (`aether!<addr>` resolved at the relay)

**Who this is for.** A **dumb 9P client** — one that speaks stock 9P over the
modem's L2CAP/USB but can NOT run a custom transport of its own. The main case is
a **macOS host** browsing over `l2cat` + `9pfuse`/plan9port (the majority of this
demo's users), but also any phone/laptop 9P client. For a full **cyberdeck** (its
own 9p4z stack), the deck-side path is better — it dials `aether!<addr>` itself via
`transport_nsfile`; see **[AETHER_MOUNT.md](AETHER_MOUNT.md)**. This doc is the
*complementary* modem-side path for clients that can't do that.

**Goal.** Let such a dumb client mount a *remote* mesh node's filesystem —
`dev`/`net` of a node 2 hops away — with **no client-side mesh code**: it does
`mount l2cap!dect-modem` and remote nodes appear as ordinary directories to `bind`.
All the aether routing happens **inside the relay (5340)**.

This is the on-device home for the one piece the 9P-over-mesh demo ran on the host
(`tools/aether_conv --bridge`), so a dumb client gets remote mounts *seamlessly*
instead of having to run that bridge tool by hand. Everything else already exists.

**Status: BUILT (relay 0.38.20), not yet hardware-tested.** `aether_conv_transport.c`
+ the `/net/mesh` re-export are wired into `main.c` (peer set via `dev/mesh_peer`,
MVP Phase 1). The three client paths: `transport_nsfile` (deck, built) · this
modem-side re-export (dumb clients, **built** — gate on §7 step 2) · `aether_conv
--bridge` (host tool, works today as the manual stopgap). NOTE: Phase 1 shipped as
`dev/mesh_peer` + a fixed `/net/mesh` mount (not `/net/mesh/ctl` + `/net/mesh/node/`
— same behavior, one writable control file instead of a `ctl` node).

---

## 1. What already exists (reuse verbatim)

- **`remote_fs`** (9p4z): transparent proxy `fs_ops` that re-exports an upstream
  9P subtree reached over a `struct ninep_client`, mounted into a `union_fs`.
  Already used by the relay for `/net/aether` (`main.c:1169`, `:1194`).
- **`mesh_client` / `mesh_transport`** (relay): a `ninep_client` to the 9151 over
  the inter-chip UART, with lazy attach + self-heal (`mesh_ensure_attached`,
  `mesh_remote_root`, `mesh_remote_down`, the link monitor). `main.c:285-710`.
- **The 9151's `/net/aether` conversation interface**: `clone` → conv `N` →
  `N/data` + `connect <peer>` on the ctl fid. This is the datagram plane.
- **The remote node's mesh 9P server** (`dect_mesh/src/aether_9p_mesh.c`): a
  `ninep_server` that answers any reliable datagram shaped like a 9P T-message,
  serving that node's own `/net`+`/dev`. **This is the far end — already proven.**
- **`do_bridge`** (`tools/aether_conv.c:607`): the adapter loop — per 9P message,
  write the framed T to `N/data`, read the R with **4× retry + tag-match** to
  discard stale/duplicate replies on the untagged datagram stream.
- **The L2CAP + USB-CDC session pools** already serving the relay's union.

## 2. The one new component: the aether-conversation transport

A second `struct ninep_transport` (`aether_conv_transport.c`) that carries a
**nested** 9P client's messages through a held `/net/aether` conversation on the
9151 — reached via the *existing* `mesh_client` (UART). The 9151 does the radio
and HONR hop-routing; this transport only does message-framing + reliability.

```
   deck ── L2CAP ──► relay union ──► [NEW] mesh_rfs (remote_fs)
                                        │  proxies walk/open/read
                                        ▼
                                     [NEW] mesh9p_client (ninep_client)
                                        │  Tversion/attach/walk/... to the REMOTE node
                                        ▼
                                     [NEW] aether_conv_transport (ninep_transport)
                                        │  send(T): write framed T to N/data (via mesh_client)
                                        │  pump:    read R from N/data, retry+tag-match, recv_cb(R)
                                        ▼
                                     mesh_client (UART 9P) ──► 9151 /net/aether conv N
                                        │  connect <peer>
                                        ▼
                                     DECT mesh (HONR) ──► remote node's aether_9p_mesh server
```

Note the layering: **the conversation `connect` is a transport-level step**; the
**9P `version`/`attach` is the nested client's job** (they become tunneled 9P
messages to the remote node's server). Clean separation, no protocol confusion.

### 2.1 Transport ops (mirrors the relay's `mesh_transport` threading model)

`ninep_client` calls `transport->ops->send(T)` then blocks on a condvar until the
transport calls `transport->recv_cb(R)` in **thread context** (never ISR). So:

- **`aether_tx_start`**: no-op (conversation opened lazily in `root_fn`, §3).
- **`aether_tx_send(buf, len)`**: copy `buf` into a holding buffer, post a
  semaphore to the pump thread, return 0. (Do **not** call `recv_cb` inline — the
  nested client isn't waiting yet; same reason the UART transport defers to a WQ.)
- **`aether_pump` thread** (one; the mesh 9P server is one-T-in-flight anyway):
  wait on the semaphore, then run **`do_bridge`'s inner loop verbatim** against
  the 9151 via the *outer* `mesh_client`:
  ```
  for (attempt = 0; attempt < 8 && rn <= 0; attempt++) {
      ninep_client_write(&mesh_client, data_fid, buf, len);      // Twrite N/data
      for (reads = 0; reads < 6; reads++) {
          rn = ninep_client_read(&mesh_client, data_fid, 0, rb, sizeof rb);
          if (rn >= 7 && tag16(rb) == tag16(buf)) break;         // tag-match
          /* else discard stale R, read again */
      }
  }
  transport->recv_cb(transport, rb, rn, ...);                    // deliver to nested client
  ```
  Retry budget **8** (the mesh 9P server memory note; do_bridge used 4 on USB).

`mesh_client` is per-tag concurrency-safe, so these ops coexist with the existing
`/net/aether` proxy and the OTA proxy. Each nested 9P message costs one UART
`Twrite` + one-or-more UART `Tread` to the 9151 — acceptable amplification.

## 3. Session lifecycle (nested `root_fn`/`down_fn`)

The nested layer needs its own attach state, wrapping the transport's connect:

- **`mesh9p_remote_root(root)`**: (a) ensure the conversation is open+connected to
  the target peer — `clone`/read `N`/walk `N/data`/open/`connect <addr>` via
  `mesh_client` (lift from `do_bridge` setup, `aether_conv.c:611-620`); (b) if not
  yet attached, `ninep_client_version(&mesh9p_client)` + `attach` (these tunnel to
  the remote node); (c) return the remote root fid. Lazy, like `mesh_ensure_attached`.
- **`mesh9p_remote_down`**: drop the conversation (clunk `N/data` + ctl) and mark
  unattached, so the next `root_fn` reconnects. Fires when a proxied walk hits a
  stale-fid error (remote node rebooted / mesh dropped the conversation).

Reuse the two-mutex discipline (`mesh_sess`-style) so a blocking remote read holds
no lock. The existing link monitor already keeps the *outer* 9151 link healthy;
the nested layer self-heals on demand.

## 4. Namespace UX — two phases

### Phase 1 (MVP, ~matches the demo): one configurable peer
Add to the union a small control + a single re-export:
- `/net/mesh/ctl` — writable; deck writes an addr (`"00:00:00:00:30:00"` or `"30:00"`).
  Writing it sets the target peer and drops any current conversation.
- `/net/mesh/node/` — a `remote_fs` mount (base `""` = the remote root) backed by
  `mesh9p_client`. `root_fn` connects to the addr from `ctl`.

Deck flow:
```
mount l2cap!dect-modem /n/modem
echo 30:00 > /n/modem/net/mesh/ctl
bind /n/modem/net/mesh/node /n/remote
ls /n/remote            # -> dev net   (node 3000, across the mesh)
cat /n/remote/dev/aether/tree
```
This is the smallest change that gives a working deck demo, and it reproduces the
proven `mesh9p_demo.sh` transcript with the adapter now **on-device**.

### Phase 2 (full): dynamic `/net/mesh/<addr>/`
A synthetic directory where walking to a new `<addr>` lazily spins up a
conversation + `mesh9p_client` + `remote_fs` for that addr — the true `aether!<addr>`
UX (`mount`/`bind` `/net/mesh/30:00` directly, no `ctl`). Budget: the 9151 caps at
`AETHER_MAX_CONNS` (4) conversations, so cap concurrent remote mounts at ~3 and LRU
-evict. Needs one `ninep_remote_fs` + node-pool + client per live addr (small pools).

## 5. Reliability & sizing (inherited, with one upside)

- **One datagram == one 9P message**, `msize` clamped to `AETHER_MAX_PAYLOAD`
  (~448 B) via the nested client config. No reassembly. Fine for state files;
  bulk transfer wants a windowing layer (future, shared with the demo's tail).
- **Retry + tag-match** carried from `do_bridge` (budget 8).
- **Upside vs the host demo:** the modem funnels every deck op through *one*
  `mesh9p_client` (request-response), so inner requests are **naturally
  serialized** — no back-to-back-session collisions. The demo's sustained-read
  tail came from the host firing fresh sessions without pacing; the modem-side
  path removes that failure mode structurally.

## 6. Scope / files

New:
- `dect_relay/src/aether_conv_transport.c` (+ `.h`) — the transport + pump (~200 LoC,
  most lifted from `do_bridge`).
- In `main.c`: a second `ninep_client mesh9p_client`, a second `ninep_remote_fs
  mesh_rfs` + node pool, the `ctl` writable file, and two `union_fs_mount` calls
  for `/net/mesh/...`.

Unchanged: `remote_fs`, `union_fs`, `mesh_client`/UART, both session pools, the
9151, and the remote node's `aether_9p_mesh` server.

## 7. Test plan (each step gated on the previous)

1. **Relay console:** set target, log that `mesh9p_remote_root` returns a remote
   root fid (proves conversation + tunneled version/attach reach the peer's server).
2. **Host over USB/L2CAP (no deck yet):** `9p ... write .../net/mesh/ctl 30:00`
   then `9p ls /net/mesh/node` → expect `dev`/`net`; `read .../dev/aether/tree` →
   `node 3000 ... parent 0000`. **This is the key gate** — it reproduces the
   known-good `mesh9p_demo.sh` output with routing now in the modem.
3. **Deck over L2CAP:** the flow in §4.1; `bind` into the deck namespace, `ls`/`cat`.
4. **Identity match** as before (remote `dev/aether/tree` reports node 3000).

## 8. Risks / open questions

- **Conversation `connect` → mesh 9P server delivery**: proven by the demo (same
  path aether_conv used) — low risk.
- **Slot pressure**: MVP uses 1 of 4 conversations; Phase 2 must budget/LRU.
- **Buffer sizing**: nested transport buffer ~512 B; the 9151 `N/data` read must
  return whole datagrams (net_buf sizing already fixed — see frame-sizing work).
- **Nested tag/fid space**: independent of the outer `mesh_client` — no collision.
- **The reverse direction (`aether!<addr>` terminating at *another deck*)** is still
  out of scope: it needs the *far* relay to proxy inbound mesh 9P to its L2CAP-
  attached deck (an inbound mirror of this). Separate spec.
