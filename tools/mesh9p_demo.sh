#!/usr/bin/env bash
#
# mesh9p_demo.sh -- a CLEAN, disciplined 9P-over-mesh demo.
#
# The discipline (learned the hard way; the CDC is fine, our host handling was not):
#   * ONE socat bridge per node, brought up ONCE and HELD for the whole run.
#   * NEVER pkill/cycle socat mid-run; NEVER `pkill -f cat` (kills soCAT too).
#   * NEVER open a node's console port while its 9P bridge is held (same relay ->
#     DTR churn). This script touches ONLY the *3 (9P) ports.
#   * Batch auxiliary reads; the bridge holds its session, 9p commands ride it.
#   * Single clean teardown via trap.
#
# Usage:  bash tools/mesh9p_demo.sh [n1_9p_port] [n3_9p_port]
#   defaults: /dev/cu.usbmodem1303 (bridge-through node), /dev/cu.usbmodem1203 (target node)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
AC="$HERE/aether_conv"; P9="$HERE/p9do"; NP="$HOME/src/plan9port/bin/9p"
P_SRC="${1:-/dev/cu.usbmodem1303}"   # bridge runs THROUGH this node
P_DST="${2:-/dev/cu.usbmodem1203}"   # 9P server we mount lives on this node
S_SRC=/tmp/m9p_src.sock; S_DST=/tmp/m9p_dst.sock; U=/tmp/mesh9p.sock

cc -O2 -o "$AC" "$HERE/aether_conv.c" 2>/dev/null || { echo "build aether_conv failed"; exit 2; }

# -- bring up the two held bridges ONCE ------------------------------------
pkill -9 -f 'socat.*usbmodem' 2>/dev/null; sleep 2; rm -f "$S_SRC" "$S_DST" "$U"
socat UNIX-LISTEN:"$S_SRC",fork "$P_SRC",rawer & SC1=$!
socat UNIX-LISTEN:"$S_DST",fork "$P_DST",rawer & SC2=$!
BRIDGE=""
cleanup(){ [ -n "$BRIDGE" ] && kill "$BRIDGE" 2>/dev/null; kill "$SC1" "$SC2" 2>/dev/null; rm -f "$S_SRC" "$S_DST" "$U"; }
trap cleanup EXIT
sleep 3

# -- read addresses with MINIMAL connections (the churn that wedges the CDC is
#    the NUMBER of fresh socat connections, each a DEV re-open/DTR toggle -- so
#    NO poll loop: a fixed settle, then ONE batched read per node, 1 retry).
echo "letting the mesh settle (fixed wait, no polling churn)..."
sleep 15
read1(){ local o; o=$("$P9" "$1" $2 2>/dev/null); [ -z "$o" ] && { sleep 3; o=$("$P9" "$1" $2 2>/dev/null); }; echo "$o"; }
SRC_INFO=$(read1 "$S_SRC" "rd:dev/aether/addr")
DST_INFO=$(read1 "$S_DST" "rd:dev/aether/addr rd:net/aether/addr")
A=$(echo "$SRC_INFO" | grep -oE '[0-9a-f]{4} *$' | tr -cd '0-9a-f' | tail -c4)
B=$(echo "$DST_INFO" | sed -n 's#.*dev/aether/addr => \([0-9a-f]*\).*#\1#p' | head -1)
DST_ID=$(echo "$DST_INFO" | sed -n 's#.*net/aether/addr => ##p' | tr -cd '0-9a-f:')
[ -n "$A" ] && [ -n "$B" ] && [ "$A" != "$B" ] || { echo "not converged/read (src=$A dst=$B); nodes still settling -- rerun"; exit 1; }
DST_ADDR="00:00:00:00:${B:0:2}:${B:2:2}"
echo "bridge-through node = $A   target node = $B ($DST_ADDR)   target identity = $DST_ID"

# -- start the ONE bridge (loop-accepts many 9p commands), hold it ---------
"$AC" "$S_SRC" --bridge "$DST_ADDR" "$U" > /tmp/m9p_bridge.log 2>&1 & BRIDGE=$!
sleep 3
grep -q connected /tmp/m9p_bridge.log || { echo "bridge did not connect:"; cat /tmp/m9p_bridge.log; exit 1; }

# -- the demo: mount the target's filesystem across the mesh ---------------
# pace between 9P sessions: each plan9port command is a fresh session over the
# shared mesh conversation; without a gap the previous session's clunk + ARQ ACKs
# are still in flight when the next Tversion arrives, colliding on the target's
# single-in-flight 9P server. A short settle makes sustained reads reliable.
q(){ perl -e 'alarm 22; exec @ARGV' "$NP" -a "unix!$U" "$@" 2>&1; local r=$?; sleep 2; return $r; }
echo
echo "════════ 9P SESSION ACROSS THE MESH: host -> node($A) -> DK relay -> node($B) ════════"
echo; echo "\$ 9p ls /";                    q ls ''
# Front-load the RELIABLE proof-of-identity: the target's own routing tree
# (node NNNN / parent 0000) IS proof it's that node, fetched over the air, and
# it lands reliably in the first ~3 sessions. Sustained reads have a tail, so
# the durable-EUI read comes last as the headline stretch.
echo; echo "\$ 9p read dev/aether/tree";    TREE=$(q read dev/aether/tree); echo "$TREE"
echo; echo "\$ 9p ls dev";                  q ls dev
REMOTE_ID=$(q read net/aether/addr)
echo; echo "\$ 9p read net/aether/addr   ->  $REMOTE_ID"
echo "   (durable EUI identity of the target, fetched across the mesh)"
echo
echo "── proof it is really that node's file, fetched over the air ──"
echo "   routing tree read across the mesh reports: $(echo "$TREE" | tr '\n' ' ')"
echo "   remote EUI identity (read across the mesh): ${REMOTE_ID:-<dropped on sustained-read tail>}"
echo "   direct EUI identity  (read over its USB):   $DST_ID"
if [ -n "$REMOTE_ID" ] && [ "$REMOTE_ID" = "$DST_ID" ]; then
  echo "   MATCH -- the 9P read crossed the mesh and returned the target node's own file."
elif echo "$TREE" | grep -q "node $B"; then
  echo "   PROVEN via routing tree -- the mesh returned node $B's own state (parent 0000)."
else
  echo "   (identity read dropped on the sustained-read tail -- see bridge log)"
fi
echo
echo "════════ bridge relayed $(grep -c 'across the mesh' /tmp/m9p_bridge.log) 9P messages across the mesh ════════"
