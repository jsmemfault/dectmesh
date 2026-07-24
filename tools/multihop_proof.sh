#!/bin/bash
# Multi-hop certainty proof for the 3-node Aether mesh (A -- DK -- B).
# Forces a linear topology (A and B mutually deny each other's direct RF), then
# shows chat still crosses -- which it can ONLY do via the DK's relay.
set -u
HERE=/Users/jrsharp/src/dect/tools
P9="$HERE/p9do"
A_9P=/dev/cu.usbmodem1203; A_CON=/dev/cu.usbmodem1201
B_9P=/dev/cu.usbmodem1303; B_CON=/dev/cu.usbmodem1301
DK_CON=/dev/cu.usbmodem0010512707991
TAG="HOP$$"

clean(){ LC_ALL=C sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -vaE 'DECT RX:|HeyMac RX:|mesh DATA from' | sed '/^[[:space:]]*$/d'; }

# console_cmd <port> <cmd> <secs>  -- send a shell cmd to a 9151 console, capture
cc(){ local p="$1" c="$2" s="${3:-5}"
  exec 9<>"$p"; stty -f "$p" 115200 cs8 -cstopb -parenb -echo -ixon clocal -hupcl 2>/dev/null
  printf '\r%s\r' "$c" >&9
  perl -e "alarm $s; exec @ARGV" cat <&9 2>/dev/null | clean
  exec 9<&-; }

# p9read <dev> <cmds...>  -- one clean 9P session (socat torn down after)
p9read(){ local dev="$1"; shift; local n=$(basename "$dev"); local sk="/tmp/hop_$n.sock"
  pkill -f "socat.*$n" 2>/dev/null; rm -f "$sk"; sleep 1
  socat UNIX-LISTEN:"$sk",fork "$dev",rawer & local sp=$!; sleep 2
  "$P9" "$sk" "$@" 2>/dev/null | LC_ALL=C sed 's/\x1b\[[0-9;]*[mDJ]//g'
  kill $sp 2>/dev/null; pkill -f "socat.*$n" 2>/dev/null; rm -f "$sk"; }

echo "###################################################################"
echo "# STEP 1 -- baseline topology (all 9P reads, consoles closed)"
echo "###################################################################"
echo "--- Node A (1e:de..) neighbors ---"; p9read "$A_9P" rd:net/aether/addr rd:dev/aether/addr rd:dev/aether/neighbors
echo "--- Node B (36:0a..) neighbors ---"; p9read "$B_9P" rd:net/aether/addr rd:dev/aether/addr rd:dev/aether/neighbors
echo
echo "Enter A's HONR + B's HONR from above to build deny addrs..."
AH=$(p9read "$A_9P" rd:dev/aether/addr | sed -n 's#.*=> *##p' | tr -cd '0-9a-f' | head -c4)
BH=$(p9read "$B_9P" rd:dev/aether/addr | sed -n 's#.*=> *##p' | tr -cd '0-9a-f' | head -c4)
DA="00:00:00:00:${AH:0:2}:${AH:2:2}"
DB="00:00:00:00:${BH:0:2}:${BH:2:2}"
echo "A HONR=$AH -> deny-addr $DA   |   B HONR=$BH -> deny-addr $DB"

echo
echo "###################################################################"
echo "# STEP 2 -- force linear topology: deny B on A, deny A on B"
echo "###################################################################"
cc "$A_CON" "aether deny $DB" 4 | grep -aiE "Denied|deny"
cc "$B_CON" "aether deny $DA" 4 | grep -aiE "Denied|deny"
echo "waiting for neighbor eviction + HELLO timeout (25s)..."; sleep 25

echo
echo "###################################################################"
echo "# STEP 3 -- confirm A and B are now DEAF to each other (console)"
echo "###################################################################"
echo "--- A's neighbors now (B=$DB should be GONE; only DK 00:00 remains) ---"
cc "$A_CON" "aether status" 4 | grep -aiE "neighbor|00:00:00:00|rssi"
echo "--- B's neighbors now (A=$DA should be GONE) ---"
cc "$B_CON" "aether status" 4 | grep -aiE "neighbor|00:00:00:00|rssi"

echo
echo "###################################################################"
echo "# STEP 4 -- chat MUST now cross via the DK (A->B, B->A)"
echo "###################################################################"
cc "$A_CON" "aether chat ${TAG}-from-A" 3 >/dev/null; sleep 4
cc "$B_CON" "aether chat ${TAG}-from-B" 3 >/dev/null; sleep 4
echo "--- B's chatlog (expect ${TAG}-from-A, delivered via DK) ---"
cc "$B_CON" "aether chatlog" 4 | grep -aE "$TAG" || echo "  (none seen)"
echo "--- A's chatlog (expect ${TAG}-from-B, delivered via DK) ---"
cc "$A_CON" "aether chatlog" 4 | grep -aE "$TAG" || echo "  (none seen)"
echo "--- DK's chatlog (the relay -- should show BOTH) ---"
cc "$DK_CON" "aether chatlog" 4 | grep -aE "$TAG" || echo "  (none seen)"

echo
echo "###################################################################"
echo "# STEP 5 -- restore (allow), clean up"
echo "###################################################################"
cc "$A_CON" "aether allow $DB" 3 | grep -aiE "allow|un-den" || true
cc "$B_CON" "aether allow $DA" 3 | grep -aiE "allow|un-den" || true
echo "done."
