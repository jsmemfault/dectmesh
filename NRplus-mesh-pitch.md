# An open-source mesh stack for Nordic's non-cellular radios

*Internal proposal — Jon Sharp, nRF Cloud · June 2026 (updated July 2026: multi-hop proven; cryptographic node identity shipped)*

**One line:** We ship world-class non-cellular PHYs (DECT NR+ today, sub-GHz NR+ next) but no open,
Zephyr-native, multi-hop mesh to run on them. I built one — a self-organizing mesh in which **the
entire distributed system is a 9P filesystem** — originally, on my own time, for a LoRa project. It
ported to DECT NR+ essentially unchanged, and I recognized the opportunity: **it's exactly what our
NR+ radios are missing.** I'd like to grow my personal work into an official open reference stack with
fleet observability built in — and be a public proponent for these patterns across the Zephyr ecosystem.

---

## The gap
For NR+, Nordic provides the **PHY**, a **MAC that is star-only and explicitly *not* a router**, and
then routes the full networking stack to **paid, closed partners** (Wirepas, Lynq). There is **no
open-source, Zephyr-native, self-organizing multi-hop mesh** a developer can pick up and build on.
That's friction at the exact moment we're trying to grow NR+ adoption — and it's a category where
Nordic already wins with open stacks everywhere else (OpenThread, Matter, Bluetooth mesh).

**Prior art:** the closest open effort — the Opener Initiative's Apache-2.0 DECT NR+ stack (UPCT/Ostfalia,
EU MERCI project) — nails the **ETSI MAC framing and FT/PT association** but stops at a **single-hop star
with static roles**; its one mesh primitive is declared in the headers and never implemented. The
spec-conformant framing is the published, mechanical part — **the self-organizing multi-hop mesh on top is
the gap, and it's what I've built.**

## What I've built — and proven on hardware
Two pillars, running on **nRF9151s** in US NR+ spectrum — demonstrated on the **1920–1930 MHz UPCS
band (§15.323)** where the work began, and now on the **915 MHz sub-GHz band (R&D evaluation)** — all on **identical
firmware**, built directly on the DECT NR+ **PHY API**. (The same stack running on both bands is
itself the portability point, and a head start on the sub-GHz NR+ roadmap below.)

*Hardware proof, updated: three nodes, self-organization, self-heal by re-election, reliable
acknowledged transport, and the full 9P fabric below — **and multi-hop, proven.** A repeatable,
automated regression test forces two nodes to refuse each other directly (`aether deny`, gated on
the link-layer immediate sender so a relayed frame still gets through even though its original
sender is denied) and then drives a real multi-line chat exchange between them, independently
verifying every line's delivery against the recipient's own scrollback. It runs the same way every
time — not a one-off demo trick — and building it caught (and fixed) two real protocol bugs along
the way: an admission-control check that gated on the wrong address, and a durable-identity /
routing-address conflation across two layers. The multi-hop claim is on solid, tested ground now,
not a promise for the next milestone.*

**Pillar 1 — a self-organizing mesh.**
- **Zero-config:** runtime root election, shallowest-parent join, and **root-loss self-healing** —
  pull any node, including the root, and the network re-forms itself. No roles to provision.
- **Stateless tree routing (HONR16):** addresses encode topology; forwarding is arithmetic — no route
  tables to converge. (Dean Hall's HeyMac scheme — implemented with his involvement.)
- **Reliable transport:** acknowledged, in-order, de-duplicated datagrams over a continuous-RX link
  layer — not a toy; a documented protocol with a green regression suite.
- **PHY-aware, not lifted-and-shifted:** tuned to DECT NR+'s own affordances (per-frame transport-block
  sizing, full-size payloads), so it uses the radio the way the radio wants to be used.

**Pillar 2 — the whole mesh is a filesystem (9P).**  *(the differentiator)*
Every node's live state, control surface, firmware, and telemetry are **files**. One uniform,
transport-agnostic interface — the *same* generic, decades-old `cat`/`9p` tooling — with no
per-feature protocol, no SDK per metric. Demonstrated end-to-end on hardware:
- **Firmware OTA of a radio that has no USB becomes a file *write*** — proxied transparently through a
  companion chip, hands-off, no debugger. The "small idea, outsized leverage" made literal: `cp` is the
  update mechanism.
- **Fleet introspection is `cat`** — a node's live mesh state (neighbors, routes, topology) read as
  files, the same way over **USB, UART, BLE L2CAP, and across the mesh itself** (zero-config service
  discovery on the BLE side).
- It's a 40-year-old idea (Plan 9) applied to embedded fleet management — and it's exactly what keeps
  the observability story below simple as the fleet grows.

**Pillar 3 — cryptographic node identity, self-certifying, and *also* a file.**  *(shipped since the last update)*
Every node's durable address is now a **Cryptographically Generated Address**: `node_eui =
SHA256(pubkey)[:6]`, bound to a **P-256 keypair the node owns** and persists across reboots (and a full
power-cycle — verified). Because the address *is* the hash of the key, it's **self-certifying — no PKI,
no CA, no key distribution** — exactly what an infrastructure-less mesh needs. And a node **proves it
owns its address** on demand: `net/aether/prove` — write a challenge, read back a signature. An
impostor claiming another node's address, and a replayed stale proof, **both fail** — verified on
hardware by an *independent* checker (no trust in the node). Two things worth noting: the signing runs
on **Nordic's own PSA/Oberon crypto** (more silicon value showcased, on the stock minimal TF-M), and —
the Plan 9 punchline — *even proving who you are is a file operation.* This is the **"link security"
line from the roadmap below, moved to done** (identity + anti-spoof; on-air confidentiality is still
honestly roadmap). It also quietly hardened the routing: a rebooting node reclaims its tree slot by its
**stable key-bound identity**, so the topology re-forms to the same shape instead of churning.

**The key result — cross-PHY portability.** This is a port of my proven **LoRa** mesh; the routing and
MAC logic moved to DECT NR+ **essentially unchanged.** That's evidence the abstraction sits at the
right layer — and it pre-fits the **sub-GHz NR+** part on the roadmap.

Code is a clean, MIT-licensed Zephyr module. 3-minute demo video (script ready — recording next): [link].

> **See it in a few minutes.** (1) Pull the root node → a surviving node re-elects itself and the
> network re-forms. (2) Force two nodes to refuse each other directly and watch a live chat
> exchange still cross between them — multi-hop, proven on camera, not asserted. (3) `cat` a node's
> live state from a laptop over BLE. (4) Update a USB-less radio's firmware by *writing a file*.
> (5) **Challenge a node to prove it owns its address** — write a nonce, read a signature; then try to
> spoof it and watch the proof fail. Self-healing, multi-hop, everything-is-a-file, OTA-as-`cp`, and
> **cryptographic identity you can verify** — on real hardware, identical firmware on every node.

## Why this is strategic for Nordic
- **Drives silicon adoption.** Open + easy mesh lowers the barrier to choosing a 9151 / future sub-GHz
  NR+ part. Same "great HW + free SW" flywheel as our other connectivity stacks.
- **One stack, three radios.** A unified open mesh across **LoRa + DECT-NR+ + sub-GHz-NR+** is a story
  no partner is telling.
- **My differentiator — fleet observability built in.** Coming from nRF Cloud, and with **Memfault now
  in-house**, I'd make every mesh node report health, connectivity, and topology to the cloud. And
  because each node's entire state is **already a 9P filesystem**, that pipeline is *reading files* —
  not bolting on a new protocol per metric, per transport, or per firmware step. Observability *by
  construction* — squarely Nordic's post-acquisition strength, and a real differentiator vs. the
  partner stacks.

## Honest scope (so this complements, not competes)
This is a **reference / research / maker enabler** that grows the top of the adoption funnel — it is
**not** a certified product stack, and is **not** meant to displace Wirepas/Lynq, who serve certified,
supported, high-end deployments. Custom-MAC-on-PHY can't be DECT-certified today; the roadmap
(**on-air confidentiality** on top of the identity/anti-spoof layer already shipped, reliability, and
eventually riding a certifiable MAC as that matures) is how it grows up. The near-term regulatory rigor
is mine to run now — characterizing the real **on-air behavior (duty cycle, occupied bandwidth,
spectral properties) with my own SDR** — and I'd want to **partner with our RF-certification
specialists** to chart the path from an R&D-band experiment toward something certifiable: I don't come
as an RF-cert expert, I come to learn that process from ours and hand them a concrete stack worth taking
through it. Framing it as the open on-ramp *feeds* the partner ecosystem rather than undercutting it.

## What I'm proposing
A bounded first deliverable: an **officially-sanctioned open-source NR+ PHY mesh *reference* stack in
NCS** — the self-organizing routing layer, the 9P fabric, and the **self-certifying cryptographic
identity** demonstrated above, with a cloud-telemetry sample wiring nodes to nRF Cloud / Memfault. From
there: on-air confidentiality, the sub-GHz NR+ target, and a documented path toward a certifiable MAC.

## The ask
30 minutes to show the demo and find where this belongs — and who in the **DECT NR+ product line /
connectivity-software / NCS** orgs would want to own it. I want to apply my mesh + cloud experience
where it compounds **our** RF strength — building the stack *and* being a public proponent for these
patterns (9P-for-embedded, self-organizing mesh, self-certifying identity) across the Zephyr ecosystem
— and I think this is that place.

---
*Built on Dean Hall's HeyMac (github.com/dwhall/HeyMac). Repo: github.com/jrsharp/aephyr (MIT).
DECT NR+ modem FW used under Nordic's access-controlled license; kept out of the public repo.*
