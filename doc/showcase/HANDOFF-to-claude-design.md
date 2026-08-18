# Handoff → Claude Design: the Æther / 9P showcase page

**Deliverable:** `doc/showcase/aether-9p-showcase.html` — a single-file, self-contained,
theme-aware interactive web page. **The ask:** re-skin it into the **Nordic style template**
while preserving the narrative, the interactives, and — critically — the factual wording
(see **§ Accuracy** below; these were verified against Nordic's own docs and are load-bearing).

> **Note on provenance.** This page was built as a Claude Artifact in a design conversation.
> The Artifact's ephemeral source + published URL were garbage-collected over a multi-day gap,
> so this HTML is a **faithful reconstruction** from that conversation — functionally identical,
> not necessarily byte-for-byte. It has never been committed until now; it is not yet re-published.

---

## 1. What it is & who it's for

A Nordic-facing pitch for **Æther** — a general-purpose, self-organizing **DECT NR+** mesh —
positioned as a candidate **first-class Zephyr project**. The spine of the argument is that
**9P** (the Plan 9 protocol) is the connective tissue that makes every mesh capability compose
as "just files." Audience: Nordic engineering stakeholders / leadership.

Æther itself is an independently developed, **radio-agnostic** mesh stack (originally built for
LoRa; the PHY is a swappable bottom layer), here ported to the DECT NR+ PHY on a Thingy:91 X.

## 2. Narrative arc (sections, in order)

1. **Hero** — "The self-organizing mesh DECT NR+ is missing." States the gap + the thesis.
2. **Meet Æther** — the 4-layer stack (L1 DECT NR+ PHY → L2 HeyMac → L3 HONR routing → L4
   services), each with a "what 9P adds here" note. Beside it: an **animated self-organizing
   tree** (SVG) showing stateless multi-hop routing + the CGA/HONR identity-vs-location split.
3. **The connective tissue (9P history)** — Unix → Plan 9 → Inferno → Æther lineage timeline.
4. **Build it up (interactive)** — a 5-stage guided "inception": *file → everything is a file →
   two chips → across the air → all at once*. Nested namespace-shells reveal one per stage; a
   **Trace** button animates one `write` falling host→USB→relay→UART→modem→radio→peer→flash.
5. **Remote presence → two directions** — the astral-projection illustration (you "project" onto
   a remote node), then the two flows the mount enables: **read = observability** (telemetry
   drain → nRF Cloud) and **write = OTA** (firmware push). Framed as read/write twins.
6. **Even authentication is a file** — the CGA self-certifying proof (`net/aether/prove`):
   write a challenge, read `pubkey + signature`; PKI-free (no CA / PSK / key server).
7. **A general-purpose mesh** — application chips (metering, industrial, asset tracking, …) +
   5 capability beats (self-org, routing, telemetry both directions, remote mgmt, OTA cherry)
   + an **honest-scope** paragraph.
8. **Close** — "A candidate for a first-class Zephyr project."

## 3. Interactive/animated pieces (all vanilla JS/CSS/SVG, self-contained — please keep them)

- **Staged interactive** (`#scene` + `<script>`): JS state machine (`STAGES`, `go()`), nested
  `.frame` boxes revealed via `data-stage` on `.scene`; `Trace` (`runTrace()`, `TRACE[]`)
  sequentially lights the path and fills `#pathout`. Prev/Next + clickable step beads + arrow keys.
- **Self-organizing tree** (mesh section): SVG nodes/edges + CSS `@keyframes dataDown/ackUp`
  animating a packet down the tree and an ACK back. HONR labels are **real** (see §Accuracy).
- **Astral projection** (remote-presence section): SVG with an animated `.ac-trail` (spectral
  dash-flow), breathing `.ac-echo-grp`, twinkling `.ac-spark` sparkles.
- **Two-direction flow strips** + **auth challenge/response terminal** — static, styled.
- All honor `prefers-reduced-motion`. No external assets anywhere.

## 4. Visual identity (current — replace with Nordic template, but keep the *intent*)

Current system is a deliberate one; when swapping to Nordic's palette/type, preserve what these
elements *communicate*:
- **Amber (`--air`) is reserved *only* for the over-the-air / radio boundary.** It's semantic:
  it makes "crossing the radio" read instantly (the DECT-air bound, the peer frame, the trail).
  Keep a distinct "this hop is wireless" cue even if the hue changes.
- **Monospace is the data type** — version strings, file paths, addresses, node ids. The system
  lives in serial consoles; that's why data reads as mono. Keep that texture.
- **Signal-cyan (`--accent`)** = the 9P/over-the-air through-line; **teal-green (`--pass`)** is
  semantic *success only* (verdicts, ticks), not a second accent.
- Theme-aware via CSS custom-property **tokens** (`:root` light, `@media (prefers-color-scheme)`
  dark, plus `:root[data-theme=…]` overrides). Keep both themes first-class.

## 5. Accuracy — DO NOT loosen these (verified against Nordic docs)

- **"DECT NR+" — never bare "DECT."** Classic DECT is the cordless-phone standard; DECT NR+ is
  **DECT-2020 New Radio** (ETSI **TS 103 636**), a non-cellular **IMT-2020 (5G)** standard.
- We build on the **DECT NR+ *PHY* firmware**, which (Nordic's words) *"implements only the
  physical layer."* Nordic's MAC+PHY firmware is **star-topology only** → the self-organizing
  multi-hop **mesh** is the *documented* gap Æther fills. (Full standard stack: PHY→MAC→DLC→CVG.)
- Spectrum: **"license-exempt but regulated,"** not "licensed-free." Bands 1/2/3/4(9151-only)/9/22;
  the project runs **band 4 (~915 MHz)**.
- Observability backend in the collateral is **nRF Cloud** (the implementation happens to use
  Memfault — do **not** surface that name in this page).
- **CGA** node identity = `SHA-256(P-256 pubkey)[:6]` — durable, self-certifying. **HONR** =
  16-bit **nibble-tree** routing address, root `0x0000`, each level fills the next nibble (values
  1–14); **stateless** forwarding (up to nearest common ancestor, then down). Valid example
  addresses in the tree: root `00:00`; children `10:00`,`20:00`; grandchildren `11:00`,`12:00`
  under `10:00`, `21:00` under `20:00`. **Identifier/locator split:** CGA = *who*, HONR = *where*.
- **Auth honesty:** the `net/aether/prove` ownership proof is **implemented + validated** (spoofed
  and replayed proofs are both rejected). Gating every mesh session on it via 9P `Tauth` is
  **designed, not yet wired** — the honest-scope paragraph says exactly this; keep it.
- **OTA reliability:** **15/15** full-image (305 KB) OTAs over the mesh, ~56 s each,
  MCUboot-validated; one interrupted-and-resumed run still validated. Scope: **single-hop, one RF
  environment, N=15.** Multi-hop-through-a-relay OTA and long-run endurance are **not claimed.**

## 6. Source-of-truth in this repo (for cross-checking claims)

- `doc/OTA.md` — OTA + **OTA-over-the-mesh** section (recipe, the drain/push twins, reliability).
- `doc/ARCHITECTURE.md`, `doc/NET_AETHER_SPEC*.md`, `doc/RF_CHARACTERIZATION.md`,
  `doc/FIELD_TEST.md`, `doc/OBSERVABILITY.md` — deeper background.
- `NRplus-mesh-pitch.md`, `NRplus-mesh-demo-script.md`, `NRplus-mesh-role.md` (repo root) — the
  existing prose pitch/positioning; **align messaging with these.**
- Code: HONR spec `~/src/aephyr/include/zephyr/net/honr.h`; routing/L2 `~/src/aephyr/subsys/net/`;
  9P lib + auth `~/src/9p4z/src/` (`server.c` auth pool, `client.c` `ninep_client_auth`);
  app `dect_mesh/src/` (`cga.c`/`cga.h`, `aether_net.c` incl. `net/aether/prove`); relay/forwarder
  `dect_relay/src/main.c`.

## 7. Security (if touching the repo)

Never commit secrets or licensed FW: `oat_dont_commit` and `dectfw/` are **gitignored** — keep it
that way, and make sure no keys/tokens leak into the page (it's outward-facing collateral).

## 8. Open threads (design decisions left to make)

1. **Astral SVG flavor** — keep the literal "remote login" person↔echo, or nudge the echo toward
   *reading a node's telemetry/vitals* to match the observability framing?
2. **Terminology** — "forwarder at the mesh edge" vs "gateway" / "border node"?
3. **Optional: a "heal" toggle** on the tree — re-parent a node so its **HONR visibly changes**
   while its **CGA stays constant** — to dramatize *why* you need both (identifier/locator split).
4. **Optional: a failing auth example** — show a spoofed/replayed proof being *rejected*, to make
   the security guarantee visceral.

## 9. If re-publishing as a Claude Artifact

Move the `<style>` block + `<body>` contents into the artifact (the platform injects its own
`<!doctype>/<head>/<body>`), keep a `<title>`, set favicon **🪆**. Strict CSP: no external
CSS/JS/fonts/images — inline everything, embed assets as data URIs. Wide content scrolls in its
own container; page body must never scroll sideways.
