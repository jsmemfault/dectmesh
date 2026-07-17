# DECT gateway — fix broadcast RX so the deck's `achat` displays messages

## Symptom observed on the deck
Running `achat` over your mounted `/net/aether`: a broadcast from another node
*arrives* (the deck's e-paper does its RX "border flash"), but **no message text
appears**. The identical deck code displays fine when talking to the **local
sx1262** `/net/aether`. So the deck is fine — the bytes your gateway returns on
the broadcast `data` **read** don't match the framing the local server emits.

## The exact contract (what the deck's lobby reader consumes)
The deck opens the broadcast conversation (`clone` → `ctl: connect
ff:ff:ff:ff:ff:ff`) and loops doing **blocking reads on that conversation's
`data` file**. For each read it expects **one datagram**, framed as:

```
byte 0..5 : src  — the 6-byte ÆTHER address of the SENDER
byte 6..N : payload — the message bytes, verbatim
```
- Return value of the read = `6 + payload_len`.
- **One datagram per read.** The read blocks until a datagram is available.
- **No length header. No trailing NUL. No proto/marker byte.** Just
  `[6 src][payload]`, contiguous, in a single read.

The deck parses it as: `src = buf[0..5]`, `text = buf[6..n]`. It then renders
`< [xx:xx] text`. So any deviation shifts or drops the text.

### Reference (the working sx1262 server), for byte-parity
`frst/src/dev_aether_net.c`, `anet_read` / `ANF_DATA`:
```c
if ((c->announced || c->bcast) && cnt >= 6) {
    memcpy(buf, m->src, 6);   /* 6-byte source Æther addr first */
    n = 6;
}
memcpy(buf + n, m->data, plen);  /* then the raw payload */
n += plen;
return (int)n;                   /* single read = whole datagram */
```
Match this exactly.

## Ranked likely bugs (check in this order)
1. **No source prefix.** You return just the payload. The deck then eats the
   first 6 bytes of *text* as "src" (garbled) or, for messages ≤ 6 bytes, sees
   `n < 6` and **drops them silently**. → Prepend the 6-byte sender Æther addr.
2. **You included a wire marker / length byte** (e.g. the `0x05` app-proto byte,
   or a count). That marker is *internal PHY framing* and must be **stripped
   before the fs read** — the `data` read is ONLY `[src][payload]`.
3. **src and payload split across two reads** (read #1 = 6-byte src, read #2 =
   payload). The deck shows an **empty line** then a garbled one. → Return the
   whole `[src][payload]` in **one** read, atomically.
4. **Empty payload (src only).** You deliver the 6-byte src but lose the body.
   The deck prints an empty message = "border flash, no text." → Include the
   full payload.
5. **Wrong src.** All-zeros or the gateway's own address instead of the
   *originating peer's* Æther address. (Text would still show, so not the
   no-text symptom — but fix it; the deck's peer roster is built from these
   src bytes.)

## Likely root cause given the "border flash, no text" symptom
That symptom (something arrives, deck refreshes, but blank) points hardest at
**#3 or #4** — the read is returning the src but not the payload (split, or
empty). Diff your **broadcast** read path against your **directed/announce**
read path: if directed works, the announce path almost certainly already does
`[6 src][payload]` correctly and the broadcast path forgot to copy the payload.

## Self-check (no deck needed)
On the gateway, when a DECT broadcast arrives and you service the deck's `data`
read, log the exact buffer + return count you hand back. Assert:
- return count ≥ 6 and == `6 + len(message)`
- `buf[0..5]` == the originating peer's 6-byte Æther address (not zeros, not you)
- `buf[6..]` == the message bytes, no extra leading/trailing bytes
- it's produced by a **single** read call, not two

## Also confirm the write side (so the deck can SEND to the lobby)
A broadcast `data` **write** from the deck carries the **whole message as the
payload, no src/dst prefix**. Take those bytes verbatim and broadcast them over
DECT to all peers. (Don't expect an address prefix on a bcast write — that's
only for the *directed/announce* server-reply case.)

## Spec references
`frst/NET_AETHER_SPEC.md` §6a (Broadcast / party-line class) and `aether(3)`.
The roster/discovery needs nothing extra — it's built deck-side purely from the
src addresses heard in the lobby, so once this read framing is correct, both
lobby chat and peer discovery light up.
