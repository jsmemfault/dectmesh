# An open-source mesh stack for Nordic's non-cellular radios

*Internal proposal — [your name], nRF Cloud · [date]*

**One line:** Nordic ships world-class non-cellular PHYs (DECT NR+ today, sub-GHz NR+ next) but no
open, Zephyr-native, multi-hop mesh to run on them. I've built a working one across two radios, and
I'd like to do it for Nordic — as an open reference stack with first-class cloud/fleet observability.

---

## The gap
For NR+, Nordic provides the **PHY**, a **MAC that is star-only and explicitly *not* a router**, and
then routes the full networking stack to **paid, closed partners** (Wirepas, Lynq). There is **no
open-source, Zephyr-native, self-organizing multi-hop mesh** a developer can pick up and build on.
That's friction at the exact moment we're trying to grow NR+ adoption — and it's a category where
Nordic already wins with open stacks everywhere else (OpenThread, Matter, Bluetooth mesh).

## What I've already proven
A complete, self-organizing mesh running on **three nRF9151s** (one DK + two Thingy:91 X) on the
**US DECT band (1920–1930 MHz, §15.323)**, all on **identical firmware**, built directly on the
DECT NR+ **PHY API**:

- **Self-organizing, zero-config:** runtime root election, shallowest-parent join, and **root-loss
  self-healing** — pull any node, including the root, and the network re-forms itself. No roles to
  provision.
- **Stateless tree routing (HONR16):** addresses encode topology; forwarding is arithmetic, no route
  tables to converge. (Dean Hall's HeyMac scheme — I'm implementing his spec, with his involvement.)
- **It works end-to-end:** continuous-RX link layer, party-line chat across all three nodes live.
- **Cross-PHY portability — the key result:** this is a port of my proven **LoRa** mesh; the routing
  and MAC logic moved to DECT NR+ **essentially unchanged.** That's evidence the abstraction sits at
  the right layer — and it pre-fits the **sub-GHz NR+** part on the roadmap.

Code is a clean, MIT-licensed Zephyr module. 3-minute demo video: [link].

## Why this is strategic for Nordic
- **Drives silicon adoption.** Open + easy mesh lowers the barrier to choosing a 9151 / future
  sub-GHz NR+ part. Same "great HW + free SW" flywheel as our other connectivity stacks.
- **One stack, three radios.** A unified open mesh across **LoRa + DECT-NR+ + sub-GHz-NR+** is a
  story no partner is telling.
- **My differentiator — fleet observability built in.** Coming from nRF Cloud, and with **Memfault
  now in-house**, I'd make every mesh node report health, connectivity, and topology to the cloud.
  Mesh + first-class observability is squarely Nordic's post-acquisition strength and a real
  differentiator vs. the partner stacks.

## Honest scope (so this complements, not competes)
This is a **reference / research / maker enabler** that grows the top of the adoption funnel — it is
**not** a certified product stack, and is **not** meant to displace Wirepas/Lynq, who serve
certified, supported, high-end deployments. Custom-MAC-on-PHY can't be DECT-certified today; the
roadmap (link security, reliability, and eventually riding a certifiable MAC as that matures) is how
it grows up. Framing it as the open on-ramp *feeds* the partner ecosystem rather than undercutting it.

## What I'm proposing
A bounded first deliverable: an **officially-sanctioned open-source NR+ PHY mesh *reference* stack in
NCS** — the self-organizing routing layer demonstrated above, plus a cloud-telemetry sample wiring
nodes to nRF Cloud / Memfault. From there: security, the sub-GHz NR+ target, and a documented path
toward a certifiable MAC.

## The ask
30 minutes to show the demo and find where this belongs — and who in the **DECT NR+ product line /
connectivity-software / NCS** orgs would want to own it. I want to apply my mesh + cloud experience
where it compounds Nordic's RF strength, and I think this is that place.

---
*Built on Dean Hall's HeyMac (github.com/dwhall/HeyMac). Repo: github.com/jrsharp/aephyr (MIT).
DECT NR+ modem FW used under Nordic's access-controlled license; kept out of the public repo.*
