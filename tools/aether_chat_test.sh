#!/usr/bin/env bash
#
# aether_chat_test.sh -- e2e chat test over the DECK-SHAPED path: rc achat
# (tools/aether.rc) driving two 9pfuse-mounted nodes' /net/aether. This carries
# the 9P-session suite forward to the chat/datagram layer through the *dumb
# client* mount transport -- the same way the on-device deck achat will run.
#
# Complements tools/run_aether_suite.sh: that suite drives the C `aether_conv`
# straight over socat (the host-tool path); this one proves the identical layer
# reached the way a macOS/plan9port client actually reaches it -- a 9pfuse mount
# + stock file I/O, no custom binary on the client side.
#
#   Broadcast (party line)  : A sends, B listens on ff:ff:ff:ff:ff:ff  -> [src]-tagged
#   Directed (reliable ARQ) : A sends to B's addr, B listens connected to A -> bare
#
# Usage:  bash tools/aether_chat_test.sh
#         PORTS="/dev/cu.A /dev/cu.B" bash tools/aether_chat_test.sh
#
# IMPORTANT: run from a REAL terminal (Terminal.app/iTerm), NOT Claude Code's `!`
# prefix -- there the backgrounded socat/9pfuse are reaped on return and the
# mounts die mid-test (same caveat as tools/mount-term.sh). Needs 9pfuse/macFUSE.
#
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export PATH="$HOME/src/plan9port/bin:$PATH"    # rc, read, 9pfuse from plan9port
RC="$HOME/src/plan9port/bin/rc"
FUSE="$HOME/src/plan9port/bin/9pfuse"
RCS="$HERE/aether.rc"
[ -x "$RC" ]   || { echo "no plan9port rc at $RC"; exit 2; }
[ -x "$FUSE" ] || { echo "no 9pfuse at $FUSE (install plan9port + macFUSE)"; exit 2; }
[ -f "$RCS" ]  || { echo "missing $RCS"; exit 2; }

PASS=0; FAIL=0
ok(){ printf '  [PASS] %s\n' "$1"; PASS=$((PASS+1)); }
no(){ printf '  [FAIL] %s :: %s\n' "$1" "$2"; FAIL=$((FAIL+1)); }
settle(){ sleep 3; }
hex4(){ local a; a=$(echo "$1" | tr -cd '0-9a-f'); echo "${a:0:4}"; }

MNTA=/tmp/aether_cA; MNTB=/tmp/aether_cB
SKA=/tmp/aether_cA.sock; SKB=/tmp/aether_cB.sock

cleanup(){
  umount "$MNTA" 2>/dev/null; umount "$MNTB" 2>/dev/null
  pkill -f "9pfuse.*aether_c" 2>/dev/null
  pkill -f "socat.*aether_c"  2>/dev/null
}
trap cleanup EXIT

# --- ports (same detection as run_aether_suite.sh: the "...03" 9P CDC) --------
PORTS=(${PORTS:-$(ls /dev/cu.usbmodem*03 2>/dev/null)})
[ "${#PORTS[@]}" -ge 2 ] || { echo "need two thingy 9P ports (found ${#PORTS[@]}); set PORTS=..."; exit 2; }

# --- bring up a 9pfuse mount per node (mount-term.sh's proven incantation) ----
mount_node(){  # <port> <sock> <mnt>
  local port="$1" sock="$2" mnt="$3"
  umount "$mnt" 2>/dev/null; pkill -f "socat.*$sock" 2>/dev/null; sleep 1
  rm -f "$sock"; mkdir -p "$mnt"
  socat UNIX-LISTEN:"$sock",fork "$port",rawer & sleep 2
  # -A 60: cache lookups+attrs 60s. Also key for chat: it keeps macOS from
  # re-walking `clone` on every incidental stat -- each walk allocates a
  # conversation (AETHER_MAX_CONNS=4), so uncached stat storms could exhaust slots.
  "$FUSE" -A 60 "unix!$sock" "$mnt" & sleep 2
  mount | grep -q "$mnt" || { echo "mount failed: $mnt"; return 1; }
  mdutil -i off "$mnt" >/dev/null 2>&1 || true   # keep Spotlight off net/
  mdutil -d "$mnt"     >/dev/null 2>&1 || true
  return 0
}

echo "mounting node A ($SKA -> $MNTA) and node B ($SKB -> $MNTB)..."
mount_node "${PORTS[0]}" "$SKA" "$MNTA" || exit 2
mount_node "${PORTS[1]}" "$SKB" "$MNTB" || exit 2
settle

# --- convergence: read each node's HONR addr from its own mount (dev/ is fast) -
echo "waiting for convergence (distinct HONR addrs)..."
adA=""; adB=""
for t in $(seq 1 10); do
  adA=$(hex4 "$(cat "$MNTA/dev/aether/addr" 2>/dev/null)")
  adB=$(hex4 "$(cat "$MNTB/dev/aether/addr" 2>/dev/null)")
  [ "${#adA}" -ge 3 ] && [ "${#adB}" -ge 3 ] && [ "$adA" != "$adB" ] && break
  sleep 2
done
DA="00:00:00:00:${adA:0:2}:${adA:2:2}"
DB="00:00:00:00:${adB:0:2}:${adB:2:2}"
echo "node A addr=$adA ($DA)   node B addr=$adB ($DB)"
[ -n "$adA" ] && [ -n "$adB" ] && [ "$adA" != "$adB" ] || \
  { no "convergence" "nodes did not reach distinct addrs (A=$adA B=$adB)"; }
settle

# --- the chat round-trip: B listens, A sends (paced), diff recipient stream ----
# recv_mnt listens `mode`; send_mnt sends the lines paced 1/s (avoid LBT bursts);
# assert every line landed in the recipient's stdout. One retry for the missing
# lines (broadcast is best-effort -- same policy as the suite's ann/conn helpers).
chat_round(){  # <recv_mnt> <recv_arg> <send_mnt> <send_arg> <label>
  local rmnt="$1" rarg="$2" smnt="$3" sarg="$4" label="$5"
  local rlog="/tmp/_chat_${label}.txt"; : > "$rlog"
  local tag="c$$_${label}"
  local lines=("chat-one $tag" "chat-two $tag" "chat-three $tag")

  # send the given lines paced 1/s (avoid LBT bursts) through a fresh sender
  # conversation. Nested fn -> dynamic scope sees $smnt/$sarg; args are the lines
  # (bash 3.2: pass by value via "$@", no namerefs).
  send(){ { local x; for x in "$@"; do echo "$x"; sleep 1; done; } \
            | "$RC" "$RCS" -m "$smnt" "$sarg" >/dev/null 2>&1; }

  # pass 1: listener up, then send all lines
  "$RC" "$RCS" -m "$rmnt" -l 10 "$rarg" > "$rlog" 2>/dev/null & local lp=$!
  sleep 2                                   # let the listener's conversation attach
  send "${lines[@]}"
  wait "$lp" 2>/dev/null

  local missing=(); local l
  for l in "${lines[@]}"; do grep -qF "$l" "$rlog" || missing+=("$l"); done

  # pass 2: retry only the missing lines once (broadcast is best-effort, same
  # policy as the suite's ann/conn helpers)
  if [ "${#missing[@]}" -gt 0 ]; then
    echo "  ${#missing[@]}/${#lines[@]} missing after first pass, retrying once..."
    "$RC" "$RCS" -m "$rmnt" -l 10 "$rarg" > "$rlog.2" 2>/dev/null & lp=$!
    sleep 2; send "${missing[@]}"; wait "$lp" 2>/dev/null
    cat "$rlog.2" >> "$rlog" 2>/dev/null
    missing=(); for l in "${lines[@]}"; do grep -qF "$l" "$rlog" || missing+=("$l"); done
  fi

  if [ "${#missing[@]}" = 0 ]; then
    ok "$label: all ${#lines[@]} lines received over the mount path"
  else
    no "$label" "${#missing[@]}/${#lines[@]} not received (e.g. '${missing[0]}')"
  fi
  settle
}

echo; echo "########## chat over the mount path (rc achat + 9pfuse) ##########"
# broadcast party line: A sends bcast, B listens bcast (reads are [src]-prefixed)
chat_round "$MNTB" bcast "$MNTA" bcast "broadcast-A2B"
# directed reliable: B listens connected to A ($DA), A sends to B ($DB) -> bare payload
chat_round "$MNTB" "$DA" "$MNTA" "$DB" "directed-A2B"

echo; echo "########## SUMMARY ##########"
echo "  PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" = 0 ] && echo "  ALL GREEN (chat/datagram layer verified through the dumb-client mount transport)" \
  || echo "  (a FAIL here is a genuine gap in the mount-path chat -- sanity-check with a \
standalone aether.rc against one mount, and confirm run_aether_suite.sh Phase 2 is green)"
exit $([ "$FAIL" = 0 ] && echo 0 || echo 1)
