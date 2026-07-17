# Demo video script — self-organizing, multi-hop NR+ mesh (~2 minutes)

*Companion asset to the NR+ mesh pitch — Jon Sharp*

**Goal:** in ~2 minutes, make two things undeniable — *identical firmware on Nordic radios forms a
network by itself, proves it can relay multi-hop live on camera, and heals itself when you kill the
root.* The multi-hop proof and the self-heal are the two money shots; everything before them earns
the right to show it.

> **Hardware-honesty note, updated.** Three nodes are proven on the bench: self-organization,
> self-heal by re-election, reliable chat, **and multi-hop** — a repeatable automated test
> (`tools/run_aether_suite.sh` Phase 5/6) forces two nodes to refuse each other directly and drives
> a real chat exchange between them, independently verifying every line's delivery against the
> recipient's own log. The multi-hop proof is bench-forced (`aether deny`), not yet an
> over-real-distance outdoor run — say that plainly if asked; it's still a stronger claim than an
> unrepeatable field anecdote, because it's mechanical and runs the same way every time.

**Setup on camera:** three boards on the bench, clearly labeled (e.g., **ROOT / N2 / N3**), each with
its own large-font terminal tiled on screen. All three running the self-healing build. Keep it
one-take and unedited where you can — the credibility *is* that it's real.

---

| # | Time | On screen / action | Voiceover |
|---|------|--------------------|-----------|
| 1 | 0:00–0:10 | Pan the three boards + three terminals. Hold. | "Three Nordic nRF9151s. The **same firmware** on every one — no roles, no config. Let's watch them build a network." |
| 2 | 0:10–0:28 | Type `aether tree` in each terminal. Highlight `0000 ROOT`, then `1000` / `2000` / `3000`, all `rank 1`. | "They elected a root and formed a tree on their own. The addresses *encode* the topology — so forwarding is just arithmetic. No tables, nothing to provision." |
| 3 | 0:28–0:44 | On N3 type `aether chat hello from node 3` → it pops up as `<chat …> hello from node 3` on ROOT and N2. | "And it actually carries traffic — a party-line chat across the mesh." |
| 4 | 0:44–1:20 | **The multi-hop proof.** On ROOT type `aether deny <N3's addr>`; on N3 type `aether deny <ROOT's addr>`. Show `aether status` on ROOT — N3 is gone from its neighbor table. Then: `aether chat` from ROOT → it still shows up on N3. | "Now watch closely. I just told the root and node 3 to *ignore each other directly* — they're no longer neighbors, you can see that right there. And the message still gets through. The only way that's possible is node 2 relayed it. **That's multi-hop, proven live** — not asserted, not edited." |
| 5 | 1:20–1:44 | "Now the important part." **Physically pull power on the ROOT board.** Cut to N2/N3 terminals; let the logs scroll: `parent 0000 silent, orphaned — relinquishing to re-elect`. Small caption: *(~15 s, unedited)*. A new `0000 ROOT` appears on a surviving node; the other rejoins. | "Now I kill the root. No intervention from me… they notice it's gone, re-elect a new root, and re-form the tree — by themselves." |
| 6 | 1:44–1:56 | `aether tree` again: new flat tree, different root. Then end card. | "Pull any node — including the root — and the network repairs itself. Same stack runs on my LoRa radios *and* DECT NR+; the routing layer didn't change. This is the open, Zephyr-native, provably multi-hop mesh NR+ doesn't have yet." |

**End card (hold 3 s):** `github.com/jrsharp/aephyr` (MIT) · "Built on Dean Hall's HeyMac" · Jon Sharp · jon.sharp@memfault.com.

---

## Shooting notes
- **Pre-stage:** big terminal fonts, windows pre-labeled ROOT/N2/N3, `aether tree` typed and ready
  (or use shell history ↑) so you're not hunting for commands on camera. For beat 4, know each
  node's HONR address ahead of time (`aether info`) so you're not fumbling to type `aether deny`.
- **Beat 4 is the technical heart — don't rush it.** Let `aether status` sit on screen long enough
  for a viewer to actually read "N3 is not in this list" before sending the chat. The proof only
  lands if the audience can verify the setup themselves, not just take the punchline on faith.
- **The kill must look real** — yank the USB/power on the actual ROOT board in frame. That physical
  beat is what sells it; don't fake it with a command.
- **The heal takes ~15–20 s** (three announce intervals). Either keep it real-time with the
  `(~15 s, unedited)` caption (best for credibility), or do one honest, visible time-cut — never a
  hidden one.
- **Capture the logs**, not just the result — the `orphaned — relinquishing to re-elect` line is the
  proof the recovery is autonomous, not staged.
- **Let the artifacts talk.** Minimal narration, confident and plain; no hype words. The working
  system is the argument.
- **Backup plan:** `tools/run_aether_suite.sh` Phase 5/6 does the deny-and-chat proof
  automatically and prints a transcript — if beat 4 flubs on a take, that script's output is a
  ready-made B-roll / appendix slide that proves the same thing without a re-shoot.

## Cut-downs
- **Two-node version:** ROOT + N2 only, drop beat 4 (multi-hop needs a third node to relay through)
  and go straight from chat to the kill/re-elect beats. Real self-heal, just not a multi-hop proof —
  say so if asked.
- **90 s:** drop beat 3 (chat) and go straight from "it self-organized" to the multi-hop deny proof
  to "watch it heal."
- **20 s teaser (for chat/Slack/social):** beat 4 alone — deny, chat, prove it relayed. This is the
  clip that makes people stop scrolling.

---

## Companion clip — "the mesh is a filesystem" (~30 s, separate asset)

A second, standalone clip for the developer audience — *don't* bolt it onto the self-heal video; the
self-heal sells "it's real," this sells "and it's a joy to work with." One terminal, host-side.

| # | On screen / action | Voiceover |
|---|--------------------|-----------|
| 1 | `9p ls /dev` over USB → the node tree (`fw9151`, `link9151`, `reboot9151`, …). | "The whole node is a filesystem. Live state, firmware, control — files." |
| 2 | `9p read /dev/link9151` and `9p read /dev/fw9151` → human-readable status. | "Health and version: just read them. No SDK, no custom app." |
| 3 | `9p write /dev/fw9151 < image` then `9p write /dev/reboot9151`; the log shows the relay **auto-confirm** the new version. | "Firmware update on a radio that has *no USB* — a file write, proxied through its companion chip. It boots, verifies, and confirms itself." |
| 4 | (optional) the *same* `9p ls /dev` from the cyberdeck over **BLE L2CAP**. End card. | "Same filesystem, different transport — USB or Bluetooth, no code changes. That's 9P." |

**Why it lands:** every beat is a generic, decades-old command doing something a microcontroller radio
mesh has no business making this easy. No narration hype — the `ls`/`read`/`write` *is* the argument.
