# `/net/aether` — Filesystem Interface Specification

**Status:** Draft 1 (2026-06). Describes the existing FRST implementation
(`src/dev_aether_net.c`, LoRa/SX1262-backed) as a PHY-agnostic contract, so a
**second, independent implementation** — an external BLE gateway "modem"
serving a DECT NR+ PHY — can conform to it and be mounted by an unmodified
deck.

---

## 0. Why this spec exists

`/net/aether` is the deck's Plan 9-style network interface to the Æther mesh.
Today it is served *locally* by the deck firmware on top of the on-board LoRa
radio. We are introducing a **separate gateway device** (battery-powered,
multi-MCU, multi-radio) that owns a **DECT NR+ PHY** and **exports `/net/aether`
over 9P-over-L2CAP**. A deck mounts that exported tree at `/net/aether` and is
otherwise unchanged — the radio has been swapped underneath a stable filesystem
boundary.

The interface is therefore the contract that **both** of these must satisfy:

1. **Local server** — deck firmware over its own PHY (LoRa now; could be a
   directly-attached DECT NR+ PHY later — "re-bind the new PHY onto the deck").
2. **Gateway server** — the external modem, exporting the same tree over
   9P-over-L2CAP.

A conforming consumer (the deck's `dial`/`mount`/`/net` users, `achat`, etc.)
**MUST NOT** be able to tell which it's talking to.

> Design lineage: 9front `/net/<proto>` (model on `/net/udp`). See
> `project_net_aether_fs`. This spec formalises the subset already shipping plus
> the reserved surface for the gateway.

---

## 1. Model

- **L3 datagram service.** Connectionless at the wire; a *conversation* is a
  local binding (to one peer, or to "any"), exactly like `/net/udp`.
- **Addresses are 6 bytes** ("mesh MAC"), rendered as colon-separated lowercase
  hex: `aa:bb:cc:dd:ee:ff`. Dial strings are `aether!<addr>` (Plan 9 `!`-form).
- **Reliable delivery** is the default datagram class (acked, retried,
  windowed — the §4.6 reliable engine). Fragmentation/reassembly happen *below*
  this filesystem and are invisible to readers/writers (see §6).
- **PHY-agnostic.** Nothing in the file interface names LoRa or DECT NR+. The
  server maps `data` writes → reliable sends on its PHY, and reliable receives →
  `data` reads.

---

## 2. Namespace

```
/net/aether/
    clone                 ctl-clone: open allocates a conversation; read -> "N"
    status                one-line-per-conversation + neighbour summary (read)
    addr                  this node's 6-byte mesh address (read)
    <N>/                  one conversation (N = 0 .. MAX_CONNS-1)
        ctl               write commands (§5); read -> "N\n"
        data              read/write datagrams (§6)
        local             this node's address (read)
        remote            peer address, or empty if unbound (read)
        status            "connected …" | "announced" | "unconnected" (read)
```

`MAX_CONNS` is implementation-defined; the reference serves 4. A conforming
server MUST expose at least 1 and advertise the count via `status`.

---

## 3. Addressing

| Form | Example | Used by |
|---|---|---|
| bare address | `aa:bb:cc:dd:ee:ff` | `addr`, `local`, `remote` reads; `connect` arg |
| dial string | `aether!aa:bb:cc:dd:ee:ff` | `status` lines; the `dial`/`srv` layer |

- Hex MUST be lowercase, zero-padded, colon-separated, 6 octets.
- An unbound `remote` reads as **empty** (0 bytes), not an error.
- A server MAY accept fewer than 6 octets on `connect` only if it documents a
  canonical expansion; the reference requires all 6.

---

## 4. Per-file operations

### `clone` (file, the open handle becomes a conversation)
- **open** allocates a free conversation slot and rebinds the fid to `<N>/ctl`
  (Plan 9 clone semantics). Fails (`-ENOSPC`/`Ebusy`) if none free.
- **read** returns the decimal slot number `"N"` (so a client learns its
  conversation index). Offset-addressable.

### `<N>/ctl`
- **write** — newline-or-NUL-terminated command, one per write (§5).
- **read** — returns `"N\n"` (the slot number), matching the Plan 9 contract.

### `<N>/data`
- **write** — one datagram, reliable. Payloads larger than the PHY MTU are
  fragmented by the server (§6); the writer just writes its message.
  - On a **connected** conversation, the datagram goes to the bound peer, **no
    prefix** (`[payload]`).
  - On an **announced** conversation, the write is **prefixed with the 6-byte
    destination address** (`[dst][payload]`) — symmetric with the source-prefixed
    read, so a server replies to whoever it just received from. The write `count`
    must be ≥ 6 + payload; a write of < 6 bytes → `-EINVAL`. This is the
    `/net/udp` datagram-server idiom and is what carries a 9P **server**'s
    R-messages back to a mounter (§8).
- **read** — **one datagram per read**, datagram semantics (offset ignored).
  **Blocks** until a datagram arrives or the conversation is hung up (hangup
  wakes the reader with a 0-byte return = EOF).
  - On an **announced** conversation, each datagram is **prefixed with the
    6-byte source address** (so a server reader learns the sender). Requires the
    read `count` ≥ 6 + payload; a conforming reader passes a buffer ≥ `MAX_MSG`+6.
  - On a **connected** conversation, no prefix (peer is fixed).

### `<N>/local`, `<N>/remote`
- **read** — the local / peer 6-byte address (colon-hex). `remote` empty when
  unbound. Read-only.

### `<N>/status`
- **read**, one of:
  - `connected aether!<addr> reliable\n`
  - `announced\n`
  - `unconnected\n`

### top-level `status`, `addr`
- `addr` — this node's address.
- `status` — a human/script-readable summary: active vs max conversations, and
  the neighbour set. Format is advisory; a conforming server SHOULD lead with
  `<active>/<max>` so tooling can parse occupancy.

---

## 5. `ctl` command grammar

| Command | Effect | Conformance |
|---|---|---|
| `connect <addr>` | Bind this conversation to one peer (client side). `data` writes go there; `data` reads receive only from it. | MUST |
| `announce` | Passive open: receive reliable datagrams from **any** peer (server side). `data` reads are source-prefixed; `data` writes are destination-prefixed (§4) — so a server replies to a requester. | MUST |
| `hangup` | Unbind; wake any blocked `data` reader with EOF; free the slot's RX state. | MUST |

- Commands are case-sensitive, space-separated, terminated by `\n` or end of
  write. Trailing CR/LF/space ignored.
- Unknown verbs → `-EINVAL`. `connect`/`announce` on an already-bound
  conversation → `-EINVAL` (hang up first).
- **Reserved (do not reuse):** `bind`, `port`, `unreliable`, `keepalive` — see
  §9.

---

## 6. Datagram & fragmentation semantics

- **Message ceiling:** a reassembled datagram is bounded (reference:
  `MAX_MSG = 512` bytes). A server MUST advertise its ceiling (TBD: via
  `status` or a `0/maxmsg` ctl read) and SHOULD accept ≥ 512.
- **Fragmentation is below the fs.** The reference splits a message into
  `RELIABLE_MTU`-sized fragments, each carrying a 1-byte fragment header:
  - bit `0x40` `START` — first fragment of a message
  - bit `0x80` `MORE` — more fragments follow
  This framing is an implementation detail of a *given PHY's* reliable engine.
  A gateway over DECT NR+ MAY use any framing it likes **so long as `data`
  reads deliver whole, in-order, de-duplicated datagrams** — the fs contract is
  message-in/message-out, not fragment-level.
- **Reliability:** delivery is acked + retried + windowed. Loss after retry
  exhaustion is surfaced by the conversation going to `unconnected`/EOF, not by
  partial datagrams.
- **Ordering:** within one conversation, datagrams are delivered in send order.

---

## 7. Transport binding (the gateway case)

The gateway exports this tree as a **9P server over BLE L2CAP**:

| Parameter | Value | Notes |
|---|---|---|
| Transport | 9P (9P2000) over L2CAP CoC | connection-oriented channel |
| PSM | `0x0081` | `CONFIG_NINEP_L2CAP_PSM` on the deck client |
| L2CAP MTU | 4096 | `CONFIG_NINEP_L2CAP_MTU`; segmented below |
| L2CAP TX MTU | 247 | `CONFIG_BT_L2CAP_TX_MTU` (BLE default) |
| Security | encryption required (BLE bonding) | SC-only, like the keyboard link |

**Deck side (consumer):** mount the gateway's exported root and bind its
`/net/aether` over the local one, e.g.

```
srv l2cap!<gateway-addr>!0x0081 aether     # post the dialed service at /srv/aether
mount /srv/aether /n/gw
bind /n/gw/net/aether /net/aether          # re-bind the gateway's mesh fs
```

After the bind, every `/net/aether` user (dial, `achat`, etc.) transparently
rides the gateway's DECT NR+ PHY. Unbinding restores the local LoRa server.

**Gateway side (producer):** implement a 9P2000 server whose tree is exactly
§2, accept on PSM `0x0081`, require link encryption, and back `data`/`ctl` with
its DECT NR+ reliable stack. The gateway owns the 6-byte address space of its
PHY; `addr` returns the gateway's mesh address (not the deck's BLE address).

> The same tree served locally needs no transport — it's reached by
> longest-prefix `/net` server lookup, alongside `/net/tcp`, `/net/wifi`.

---

## 8. 9P over `/net/aether` (the IL pattern)

`/net/aether` is a **reliable datagram** service — reliable, in-order,
message-framed, connection-oriented (conversations), and deliberately *without*
flow/congestion control. That is exactly **IL** (Winterbottom's *Internet Link*),
which was designed at Bell Labs to carry 9P precisely because TCP is the wrong
shape for it: 9P is request/response RPC over messages, so it wants reliable,
ordered, framed *messages*, not TCP's byte stream + congestion machinery. (IL
was later dropped on Plan 9 only because the missing congestion control hurt over
the wide internet — a constraint that does **not** apply to a controlled LoRa /
DECT link.)

So a 9P **session** rides `/net/aether` directly, one **9P message per datagram**
(one `data` write = one `T`/`R` message; the engine fragments/reassembles and
preserves boundaries — §6). The "connection" lives at the 9P layer
(`Tversion`/…), not duplicated here.

| 9P role | open | `data` write | `data` read |
|---|---|---|---|
| **client / mounter** (active) | `connect <peer>` | `T`-message, no prefix → peer | `R`-message, no prefix |
| **server / exporter** (passive) | `announce` | `[src-as-dst][R]` (reply to requester) | `[src][T]` (learn the requester) |

`msize` is negotiated **≤ `MAX_MSG`** (§6 reassembly ceiling) — small is fine,
LoRa is slow. The deck wires this via the `aether!<addr>` dialstring
(`srv aether!<peer>; mount /srv/<n> /n/<n>`), whose transport maps
`ninep_transport.send` → a `data` write and a reader thread on `data` → the
transport recv callback. This **replaces** the older implicit-ACK 9P-over-LoRa
transport, which bolted its own retransmit on top of best-effort sends.

---

## 9. Conformance checklist

A conforming `/net/aether` server MUST:

1. Present the §2 tree; `clone` open→slot, read→`"N"`; rebind to `<N>/ctl`.
2. Support `ctl`: `connect <addr>`, `announce`, `hangup` (§5).
3. `data`: one whole datagram per read/write; block reads until datagram/EOF;
   source-prefix announced **reads** and destination-prefix announced **writes**
   (§4); reliable, in-order, de-duplicated delivery.
4. Render addresses as 6-octet lowercase colon-hex; dial form `aether!<addr>`.
5. `status` reflect `connected`/`announced`/`unconnected`.
6. Free conversation state and wake blocked readers on `hangup` and on clunk of
   the last fid.

SHOULD: advertise `MAX_CONNS` and the message ceiling; expose a neighbour
summary via top-level `status`.

MUST NOT: require the consumer to know the PHY; leak fragment framing through
`data`.

---

## 10. Reserved / future surface

These are anticipated and **reserved now** so the two implementations don't
diverge:

- **Ports / demux.** The reference notes the announce/port demux is the next
  increment. Reserved `ctl` verb `port <n>` and an address suffix
  `aether!<addr>!<port>` for multiplexing conversations to one peer.
- **Unreliable class.** `ctl unreliable` to opt a conversation into best-effort
  datagrams (lower latency for telemetry); default stays reliable.
- **Neighbour table as files.** A `/net/aether/neighbours` (or per-neighbour
  dirs with RSSI/last-seen) — mirrors `stats`/`/proc` everything-is-a-file.
- **Keepalive / link params.** `ctl keepalive <ms>`; PHY-tuning lives behind
  the fs, not in it.

---

## 11. Reference implementation

- Server: `src/dev_aether_net.c` (prefix `/net/aether`, gated on
  `CONFIG_AETHER_MESH`), over `aether_mesh_reliable_send()` / the reliable
  engine (`zephyr/net/aether_reliable.h`).
- Mesh/PHY: `aether_mesh.h` (LoRa SX1262 today).
- Adjacent: `/dev/aether` (`src/dev_aether.c`) exposes node-level
  `addr`/`status`/`peers`/`routes` — *node* state, distinct from this *network*
  interface (cf. `ps`-over-`/proc` vs `stats`-over-`/dev`).
- 9P-over-L2CAP transport + the `srv`/`mount`/`bind` namespace plumbing the
  gateway rides: `src/conn.c`, `src/dev_srv.c`, `src/ns.c`.
