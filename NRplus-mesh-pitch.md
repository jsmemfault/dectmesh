# An open-source mesh stack for Nordic's non-cellular radios

*Internal proposal — Jon Sharp, nRF Cloud · June 2026*

**One line:** Nordic ships world-class non-cellular PHYs (DECT NR+ today, sub-GHz NR+ next) but no
open, Zephyr-native, multi-hop mesh to run on them. I've built one — a self-organizing mesh in which
**the entire distributed system is a 9P filesystem** — working across two radios, and I'd like to
build it for Nordic as an open reference stack with fleet observability built in.

---

## The gap
For NR+, Nordic provides the **PHY**, a **MAC that is star-only and explicitly *not* a router**, and
then routes the full networking stack to **paid, closed partners** (Wirepas, Lynq). There is **no
open-source, Zephyr-native, self-organizing multi-hop mesh** a developer can pick up and build on.
That's friction at the exact moment we're trying to grow NR+ adoption — and it's a category where
Nordic already wins with open stacks everywhere else (OpenThread, Matter, Bluetooth mesh).

## What I've built — and proven on hardware
Two pillars, running on **nRF9151s** in US NR+ spectrum — demonstrated on the **1920–1930 MHz UPCS
band (§15.323)** where the work began, and now on the **915 MHz sub-GHz band (R&D evaluation)** — all on **identical
firmware**, built directly on the DECT NR+ **PHY API**. (The same stack running on both bands is
itself the portability point, and a head start on the sub-GHz NR+ roadmap below.)

*Honest scope of the hardware proof: demonstrated today on a **two-node bench** — self-organization,
self-heal by re-election, reliable acknowledged transport, and the full 9P fabric below. The routing
layer (HONR16, stateless-tree) is **built for multi-hop**; standing up a three-node relay topology is
the immediate next bench milestone, not a redesign.*

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

**The key result — cross-PHY portability.** This is a port of my proven **LoRa** mesh; the routing and
MAC logic moved to DECT NR+ **essentially unchanged.** That's evidence the abstraction sits at the
right layer — and it pre-fits the **sub-GHz NR+** part on the roadmap.

Code is a clean, MIT-licensed Zephyr module. 3-minute demo video (script ready — recording next): [link].

> **See it in 3 minutes.** (1) Pull the root node → a surviving node re-elects itself and the network
> re-forms. (2) `cat` a node's live
> state from a laptop over BLE. (3) Update a USB-less radio's firmware by *writing a file*. Self-healing,
> everything-is-a-file, and OTA-as-`cp` — on real hardware, identical firmware on every node.

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
supported, high-end deployments. Custom-MAC-on-PHY can't be DECT-certified today; the roadmap (link
security, reliability, and eventually riding a certifiable MAC as that matures) is how it grows up.
Framing it as the open on-ramp *feeds* the partner ecosystem rather than undercutting it.

## What I'm proposing
A bounded first deliverable: an **officially-sanctioned open-source NR+ PHY mesh *reference* stack in
NCS** — the self-organizing routing layer plus the 9P fabric demonstrated above, with a cloud-telemetry
sample wiring nodes to nRF Cloud / Memfault. From there: security, the sub-GHz NR+ target, and a
documented path toward a certifiable MAC.

## The ask
30 minutes to show the demo and find where this belongs — and who in the **DECT NR+ product line /
connectivity-software / NCS** orgs would want to own it. I want to apply my mesh + cloud experience
where it compounds Nordic's RF strength, and I think this is that place.

---
*Built on Dean Hall's HeyMac (github.com/dwhall/HeyMac). Repo: github.com/jrsharp/aephyr (MIT).
DECT NR+ modem FW used under Nordic's access-controlled license; kept out of the public repo.*
