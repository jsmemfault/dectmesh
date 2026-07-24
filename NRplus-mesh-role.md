# How I'd lead it: an open NR+ mesh effort at Nordic

*Companion to "An open-source mesh stack for Nordic's non-cellular radios" — Jon Sharp · June 2026*

**The role in one line:** Technical lead for an open, Zephyr-native NR+ mesh stack — *and* Nordic's
developer-ecosystem voice for non-cellular mesh. One person who both **builds the thing** and **grows
the community** around it, because for an open project those two jobs reinforce each other.

## Why a dedicated lead, not a side project
Open source that a silicon vendor blesses lives or dies on **sustained ownership**: a coherent
architecture, a public roadmap people can bet on, responsive maintainership, and a credible human the
community can actually talk to. Split that across a rotating team or run it as 20%-time and you get
exactly the half-maintained repos developers learn not to trust. The payoff — more NR+ / 9151 design
wins — only materializes if someone owns it end to end.

## Mandate 1 — Technical lead (own the stack)
- Architecture, roadmap, and code for the open mesh stack: routing, self-organization, link layer,
  and the uniform **9P filesystem control plane** that makes state, firmware, and telemetry one
  interface across transports.
- Upstream it as first-class **Zephyr modules** and keep it building in NCS.
- Drive the roadmap that turns the demo into something real: **link-layer security** (self-certifying
  cryptographic node identity + anti-spoof proof is **already shipped**; on-air confidentiality next),
  reliability, **cloud/fleet telemetry** (nRF Cloud / Memfault), and the **sub-GHz NR+** target.
- Maintain a documented path toward a **certifiable MAC** as Nordic's MAC matures — so the open stack
  grows up instead of dead-ending as "research only."

## Mandate 2 — Developer-ecosystem voice (grow the adoption)
Not just NR+ mesh — a **public proponent for the underlying patterns** (9P as an embedded control
plane, self-organizing mesh, self-certifying identity) across the **Zephyr ecosystem** and the wider
industry. Defined by adoption outcomes, not visibility for its own sake:
- **DevAcademy course / labs** for NR+ mesh, plus reference samples that "just work" on a DK.
- **Talks and writing** — Zephyr Developer Summit, Embedded World, Nordic webinars, DevZone.
- An **active public repo + community**: issues, PRs, a visible roadmap people can contribute to.
- Presence in the relevant **DECT-NR+ / standards community** so Nordic's open posture has a face.
- Measured by real funnel metrics: repo adoption, sample/course usage, NR+ design-win mentions.

## How I'd operate (the posture)
- **Open by default** (MIT), with attribution to Dean Hall's HeyMac and collaboration with him where possible.
- **Complement, never compete** with Wirepas/Lynq — explicitly the *open on-ramp that feeds the partner
  funnel*. I'd keep that line bright in everything public.
- **Earn the title in public.** I'm not asking to be anointed the face on day one — ship the repo, give
  the first talk, land the first upstream, and let the role ratify what the community already sees.

## First 90 days (proof I'll deliver, not just talk)
1. Public repo cleaned up + an NCS-buildable NR+ mesh sample anyone can flash on two DKs.
2. A short architecture/roadmap RFC, circulated internally for the complement-not-compete sign-off.
3. One DevZone post + a recorded demo, and a conference talk proposal submitted.
4. A working **mesh-to-cloud** telemetry sample (nodes → nRF Cloud / Memfault) — the differentiator.

## What I'd need
An exec sponsor spanning **DevRel / ecosystem + connectivity-SW / product**, a home org, a clear
starting time allocation (even part-time to prove it out), and explicit agreement on the open-source +
complement-the-partners posture. Scope and headcount grow with traction.

## Why me
I've shipped the cross-PHY proof (LoRa → DECT NR+, routing unchanged), the multi-hop proof (a
repeatable, automated test that forces two nodes apart and verifies a real chat conversation still
crosses via a relay), *and* — since — **self-certifying cryptographic identity**: each node's address
is the hash of a keypair it owns, persists across power-cycles, and is provable on demand, with
spoofing and replay rejected by an independent verifier. That last one is the tell for the *architect*
half of this job: I recognized that a 2005 IPv6 idea (CGA), our own on-die PSA/Oberon crypto, and
the 9P fabric compose into **self-certifying identity with no PKI** — and that *proving who you are*
becomes just another file (`net/aether/prove`). The bigger version of that move came first: **I built
Aether on my own time, for a separate LoRa project** — and recognized it was exactly the gap our NR+
line has. Bringing my personal work across, and proving it ports essentially unchanged, is the whole
point: seeing where a powerful thing already in hand fits a need the org hasn't filled. Building the multi-hop test also wasn't a demo
exercise — it surfaced and fixed a real admission-control bug (gating on the wrong address across a
relay hop) and a durable-identity / routing-address conflation spanning two protocol layers: the part
of the job that doesn't show up in a demo video — writing the test that finds the bug the demo would
have quietly hidden. I come from **nRF Cloud** with the **Memfault** observability angle few others can
bring; and I communicate it clearly in writing and demos — the same skill the ambassador half of the
role demands. I'd like to point all of that at the part of Nordic where it compounds our RF strength.

---
*Companion to the project one-pager. Built on Dean Hall's HeyMac (github.com/dwhall/HeyMac).*
