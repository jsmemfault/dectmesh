# Demo video script — self-organizing NR+ mesh (~80 seconds)

*Companion asset to the NR+ mesh pitch — [your name]*

**Goal:** in ~80 seconds, make one thing undeniable — *identical firmware on three Nordic radios
forms a multi-hop mesh by itself, and heals itself when you kill the root.* The self-heal is the
money shot; everything before it earns the right to show it.

**Setup on camera:** three boards on the bench, clearly labeled (e.g., **ROOT / N2 / N3**), each with
its own large-font terminal tiled on screen. All three running the self-healing build. Keep it
one-take and unedited where you can — the credibility *is* that it's real.

---

| # | Time | On screen / action | Voiceover |
|---|------|--------------------|-----------|
| 1 | 0:00–0:10 | Pan the three boards + three terminals. Hold. | "Three Nordic nRF9151s. The **same firmware** on every one — no roles, no config. Let's watch them build a network." |
| 2 | 0:10–0:28 | Type `aether tree` in each terminal. Highlight `0000 ROOT`, then `1000` / `2000` / `3000`, all `rank 1`. | "They elected a root and formed a tree on their own. The addresses *encode* the topology — so forwarding is just arithmetic. No tables, nothing to provision." |
| 3 | 0:28–0:44 | On N3 type `aether chat hello from node 3` → it pops up as `<chat …> hello from node 3` on ROOT and N2. | "And it actually carries traffic — a party-line chat across the mesh. This is **multi-hop**, on a radio where the standard link layer only does a star." |
| 4 | 0:44–1:08 | "Now the important part." **Physically pull power on the ROOT board.** Cut to N2/N3 terminals; let the logs scroll: `parent 0000 silent, orphaned — relinquishing to re-elect`. Small caption: *(~15 s, unedited)*. A new `0000 ROOT` appears on a surviving node; the other rejoins. | "Now I kill the root. No intervention from me… they notice it's gone, re-elect a new root, and re-form the tree — by themselves." |
| 5 | 1:08–1:20 | `aether tree` again: new flat tree, different root. Then end card. | "Pull any node — including the root — and the network repairs itself. Same stack runs on my LoRa radios *and* DECT NR+; the routing layer didn't change. This is the open, Zephyr-native mesh NR+ doesn't have yet." |

**End card (hold 3 s):** project name · `github.com/jrsharp/aephyr` (MIT) · "Built on Dean Hall's HeyMac" · your name/contact.

---

## Shooting notes
- **Pre-stage:** big terminal fonts, windows pre-labeled ROOT/N2/N3, `aether tree` typed and ready
  (or use shell history ↑) so you're not hunting for commands on camera.
- **The kill must look real** — yank the USB/power on the actual ROOT board in frame. That physical
  beat is what sells it; don't fake it with a command.
- **The heal takes ~15–20 s** (three announce intervals). Either keep it real-time with the
  `(~15 s, unedited)` caption (best for credibility), or do one honest, visible time-cut — never a
  hidden one.
- **Capture the logs**, not just the result — the `orphaned — relinquishing to re-elect` line is the
  proof the recovery is autonomous, not staged.
- **Let the artifacts talk.** Minimal narration, confident and plain; no hype words. The working
  system is the argument.

## Cut-downs
- **60 s:** drop beat 3 (chat) and go straight from "it self-organized" to "watch it heal."
- **15 s teaser (for chat/Slack/social):** beats 4→5 only — kill the root, show the heal, end card.
  This is the clip that travels.
