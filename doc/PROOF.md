# Proof dossier — the receipts

*Companion to the NR+ mesh pitch — Jon Sharp. Every headline claim, backed by the actual
on-hardware output that produced it. Nothing here is a mockup; these are transcripts from a
3-node bench (two Thingy:91 X + one nRF9151 DK), all running identical `dect_mesh` firmware on
US 915 MHz NR+ (band 4, carrier 538).*

The fleet, by durable cryptographic identity:

| node | CGA address (`node_eui`) | key storage | carrier |
|------|--------------------------|-------------|---------|
| Node A (Thingy:91 X) | `1e:de:2e:99:a4:f6` | external QSPI NVS | 538 (915 MHz) |
| Node B (Thingy:91 X) | `36:0a:45:63:c7:17` | external QSPI NVS | 538 (915 MHz) |
| DK (nRF9151 DK) | `6e:1e:d1:61:8f:6c` | internal flash NVS | 538 (915 MHz) |

---

## 1 · Multi-hop, forced and proven (A — DK — B)

Co-located nodes hear each other directly, so a "mesh" can hide a 1-hop star. This test **removes the
direct link** and shows traffic still crosses — which is only possible via a relay.

**Baseline — A and B are direct neighbours** (a 1-hop path exists):
```
Node A (2000): DK 00:00 rssi -39 | B 10:00 (36:0a:45:63:c7:17) rssi -62
Node B (1000): DK 00:00 rssi -40 | A 20:00 (1e:de:2e:99:a4:f6) rssi -63
```

**Force the line** — deny each node's link address on the other's console:
```
A: Denied 00:00:00:00:10:00 -- evicted; Direct route to denied 00:00:00:00:10:00 invalidated
B: Denied 00:00:00:00:20:00 -- evicted; Direct route to denied 00:00:00:00:20:00 invalidated
```

**Proof #1 — they are now genuinely deaf to each other.** Each node's only live neighbour is the DK,
and every route points through it:
```
A neighbours: 00:00 (DK) rssi -40, 3s ago      route: ...00:00 via 00:00 hops 1   (B is GONE)
B neighbours: 00:00 (DK) rssi -41, 2s ago      route: ...00:00 via 00:00 hops 1   (A is GONE)
```

**Proof #2 — chat still crosses, and only the DK could have carried it:**
```
B's chatlog:  <a4:f6> HOP-from-A      ← A's message reached B
A's chatlog:  <c7:17> HOP-from-B      ← B's message reached A
DK's chatlog: <a4:f6> HOP-from-A
              <c7:17> HOP-from-B      ← the relay saw and forwarded BOTH
```

There are three nodes. A and B provably cannot hear each other (Proof #1), yet each other's messages
arrive (Proof #2), and the DK's log shows it relayed both. **The only possible path is A → DK → B and
B → DK → A.** The deny gates on the *immediate* RF sender, so a denied node's direct frame is dropped
but the DK's re-broadcast (immediate sender = DK) is accepted — that is exactly how a relay is supposed
to work. Repeatable via `tools/multihop_proof.sh` / `tools/run_aether_suite.sh` Phase 5–6.

*(Routed **unicast** multi-hop — reliable, acknowledged datagrams across a 2-hop root path — was
separately validated 10/10 both directions.)*

---

## 2 · Cryptographic identity that persists (CGA)

Each node's address is `node_eui = SHA256(pubkey)[:6]`, bound to a P-256 keypair generated once and
kept in NVS. **The address is the same on every boot** — because the key is.

**Node B across three warm reboots and a full power-cycle:**
```
boot 1: CGA node identity 36:0a:45:63:c7:17 (persistent)   short addr 23:7d
boot 2: CGA node identity 36:0a:45:63:c7:17 (persistent)   short addr 78:cf
boot 3: CGA node identity 36:0a:45:63:c7:17 (persistent)   short addr 39:b5
power-cycle: net/aether/addr => 36:0a:45:63:c7:17
```
The durable identity (`node_eui`) is rock-stable; the throwaway HONR *routing* address churns each boot
as the tree re-forms — exactly the separation of concerns the design intends. The DK proves the same on
a *different* storage backend (internal flash, no QSPI): `6e:1e:d1:61:8f:6c` survives every reset.

Self-certifying: **no PKI, no CA, no key distribution.** To trust that address `X` belongs to key `P`,
you hash `P`. That's it.

---

## 3 · Ownership proof — spoof-resistant, verified independently

A node proves it holds the private key behind its address. The verifier (`tools/aether_verify.c`,
OpenSSL) trusts nothing the node says — it checks the math.

**Live proof (Node B):**
```
node claims address : 36:0a:45:63:c7:17
challenge sent      : 5e1afa7d12ad00aea5f95c99fd778ffc   (fresh random)
  address binding  SHA256(pubkey)[:6]  : MATCH
  key possession   ECDSA-P256 over nonce: VALID
  ✓ PROOF VALID -- node cryptographically owns CGA 36:0a:45:63:c7:17
```

**Attack 1 — impersonation** (present a valid proof, claim someone else's address):
```
  address binding  : MISMATCH (claimed 1e:de:2e:99:a4:f6, computed 36:0a:45:63:c7:17)
  ✗ PROOF FAILED
```

**Attack 2 — replay** (reuse a signature under a new challenge):
```
  key possession   : INVALID
  ✗ PROOF FAILED
```

You cannot find a pubkey that hashes to another node's address without its key, and you cannot reuse a
signature bound to a different challenge. Signing runs on Nordic's on-die **PSA/Oberon** crypto, on the
stock minimal TF-M. And the whole exchange is a **file**: write `net/aether/prove`, read the result.

---

## 4 · One filesystem, every transport

The same node state / control / firmware / identity is reachable as files over **USB-CDC, UART, BLE
L2CAP, and across the mesh itself** — with generic `9p`/`cat` tooling, no per-feature protocol. OTA of
a USB-less radio is a file *write* (`9p write /dev/fw9151`), proxied through its companion nRF5340 and
auto-confirmed. See `doc/AETHER_MOUNT.md`, `doc/9P_BLE_GATEWAY_BRIEF.md`, `doc/OTA.md`.

---

*Reproduce: `tools/run_aether_suite.sh` (full regression), `tools/multihop_proof.sh` (Section 1),
`tools/aether_prove.sh <port>` (Section 3). Firmware `dect_mesh 0.7.38`. Built on Dean Hall's HeyMac.*
