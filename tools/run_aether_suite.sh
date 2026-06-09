#!/usr/bin/env bash
#
# run_aether_suite.sh -- comprehensive, host-driven /net/aether test suite,
# run against real hardware over the 5340's USB-CDC 9P ports.
#
#   Phase 1  single-node conformance        (tools/aether_test, on node A)
#   Phase 2  two-node datagram delivery      (tools/aether_conv, A <-> B)
#
# Auto-detects two Thingy:91 X 9P ports (/dev/cu.usbmodem*3), brings up socats,
# waits for the mesh to converge to distinct addresses, runs the battery, and
# prints a PASS/FAIL summary. Datagram-delivery checks retry once so a single
# transient drop does not fail them; the reliability check stays honest.
#
# Usage:  bash tools/run_aether_suite.sh
#
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NP="${NP:-$HOME/src/plan9port/bin/9p}"
AC="$HERE/aether_conv"
AT="$HERE/aether_test"
P9="$HERE/p9do"
SA=/tmp/aether_A.sock
SB=/tmp/aether_B.sock
PASS=0; FAIL=0
ok(){ printf '  [PASS] %s\n' "$1"; PASS=$((PASS+1)); }
no(){ printf '  [FAIL] %s :: %s\n' "$1" "$2"; FAIL=$((FAIL+1)); }

# bounded 9p helper (macOS has no `timeout`)
bnd(){ local to=$1; shift; eval "$@" >/tmp/_o 2>&1 & local n=$!
  for i in $(seq 1 "$to"); do kill -0 "$n" 2>/dev/null || break; sleep 1; done
  kill -0 "$n" 2>/dev/null && kill -9 "$n" 2>/dev/null; cat /tmp/_o; }
trun(){ "$@" & local p=$!; for i in $(seq 1 "${TRUN_TO:-10}"); do kill -0 "$p" 2>/dev/null || break; sleep 1; done; kill -9 "$p" 2>/dev/null; }

# --- build the host tools --------------------------------------------------
cc -O2 -o "$AC" "$HERE/aether_conv.c" || { echo "build aether_conv failed"; exit 2; }
cc -O2 -o "$AT" "$HERE/aether_test.c" || { echo "build aether_test failed"; exit 2; }
cc -O2 -o "$P9" "$HERE/p9do.c"        || { echo "build p9do failed"; exit 2; }

# --- ports + socats --------------------------------------------------------
PORTS=($(ls /dev/cu.usbmodem*3 2>/dev/null))
[ "${#PORTS[@]}" -ge 2 ] || { echo "need two thingy 9P ports (found ${#PORTS[@]})"; exit 2; }
pkill -f 'socat.*usbmodem' 2>/dev/null; sleep 2; rm -f "$SA" "$SB"
socat UNIX-LISTEN:"$SA",fork "${PORTS[0]}",rawer & socat UNIX-LISTEN:"$SB",fork "${PORTS[1]}",rawer & sleep 3
trap 'pkill -f "socat.*usbmodem" 2>/dev/null' EXIT

# Persistent-session reads via p9do: ONE held 9P session per call, and BATCHED
# (nodeid + addr together), instead of per-command 9p(1) that opens/closes the
# port every read. The relay's DTR session pool is size 1, so per-command churn
# degrades the USB-CDC -- this convergence loop alone used to fire ~150 open/close
# cycles before Phase 2, wedging the link. Hold + batch + fewer iterations.
conv_read(){ "$P9" "$1" rd:dev/aether/nodeid rd:dev/aether/addr 2>/dev/null; }
hex4(){ local a; a=$(echo "$1" | tr -cd '0-9a-f'); echo "${a:0:4}"; }

A=$SA; B=$SB

# --- wait for the mesh to converge to DISTINCT addresses -------------------
echo "waiting for convergence (distinct HONR addrs)..."
ida=""; idb=""; adA=""; adB=""
for t in $(seq 1 10); do
  oa=$(conv_read "$A"); ob=$(conv_read "$B")
  ida=$(echo "$oa" | sed -n 's#.*nodeid => ##p' | tr -cd '0-9a-f')
  idb=$(echo "$ob" | sed -n 's#.*nodeid => ##p' | tr -cd '0-9a-f')
  adA=$(hex4 "$(echo "$oa" | sed -n 's#.*addr => ##p')")
  adB=$(hex4 "$(echo "$ob" | sed -n 's#.*addr => ##p')")
  [ "${#adA}" -ge 3 ] && [ "${#adB}" -ge 3 ] && [ "$adA" != "$adB" ] && break
  sleep 2
done
DA="00:00:00:00:${adA:0:2}:${adA:2:2}"
DB="00:00:00:00:${adB:0:2}:${adB:2:2}"
echo "node A = $ida addr=$adA ($DA)   node B = $idb addr=$adB ($DB)"
if [ -z "$adA" ] || [ -z "$adB" ] || [ "$adA" = "$adB" ]; then
  no "convergence" "nodes did not reach distinct addrs (A=$adA B=$adB) -- Phase 2 unreliable"
fi

# ==========================================================================
echo; echo "########## Phase 1: single-node /net/aether conformance (node A) ##########"
P1=$("$AT" "$A")
echo "$P1" | sed 's/^/  /'
r=$(echo "$P1" | grep -oE '[0-9]+ passed, [0-9]+ failed')
[ -n "$r" ] && { PASS=$((PASS + ${r% passed*})); FAIL=$((FAIL + $(echo "$r" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+'))); }
sleep 1

# ==========================================================================
echo; echo "########## Phase 2: two-node datagram delivery (A <-> B) ##########"

# announced receive: <recv_sock> <send_sock> <dst> <payload> <expect-src> <label>
ann(){
  local got=""
  for attempt in 1 2; do
    "$AC" "$1" --recv > /tmp/_r.log 2>&1 & local rp=$!; sleep 3
    TRUN_TO=10 trun "$AC" "$2" "$3" "$4" >/dev/null 2>&1
    sleep 3; kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null; sleep 1
    got=$(grep '\[RECV\]' /tmp/_r.log | head -1); [ -n "$got" ] && break
  done
  if [ -z "$got" ]; then no "$6" "no delivery (2 attempts)"; return; fi
  local n src pl
  n=$(echo "$got" | grep -oE '\[RECV\] [0-9]+' | grep -oE '[0-9]+')
  src=$(echo "$got" | grep -oE 'from [0-9a-f:]+' | awk '{print $2}')
  pl=$(echo "$got" | sed -E 's/.* : //')
  if [ "$n" = "${#4}" ] && [ "$pl" = "$4" ] && [ "$src" = "$5" ]; then
    ok "$6 (len=$n src=$src payload='$pl')"
  else no "$6" "len=$n/${#4} payload='$pl'/'$4' src=$src/$5"; fi
}

# connected receive: <recv_sock> <send_sock> <recv-peer> <dst> <payload> <label>
conn(){
  local got=""
  for attempt in 1 2; do
    "$AC" "$1" --crecv "$3" > /tmp/_c.log 2>&1 & local rp=$!; sleep 3
    TRUN_TO=10 trun "$AC" "$2" "$4" "$5" >/dev/null 2>&1
    sleep 3; kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null; sleep 1
    got=$(grep '\[CRECV\]' /tmp/_c.log | head -1); [ -n "$got" ] && break
  done
  if [ -z "$got" ]; then no "$6" "no delivery (2 attempts)"; return; fi
  local n pl
  n=$(echo "$got" | grep -oE '\[CRECV\] [0-9]+' | grep -oE '[0-9]+')
  pl=$(echo "$got" | sed -E 's/.* : //')
  if [ "$n" = "${#5}" ] && [ "$pl" = "$5" ]; then ok "$6 (len=$n, no prefix, payload='$pl')"
  else no "$6" "len=$n/${#5} payload='$pl'/'$5'"; fi
}

ann  "$A" "$B" "$DA" "announced-B2A" "$DB" "announced receive B->A: length + payload + src"
conn "$A" "$B" "$DB" "$DA" "connected-B2A" "connected receive B->A: bare payload (no prefix)"
ann  "$B" "$A" "$DB" "reverse-A2B"   "$DA" "reverse direction A->B announced receive"

# reliability: 3 sequential announced sends, honest delivered count
echo "  -- reliability: 3 sequential datagrams B->A --"
got=0
for i in 1 2 3; do
  "$AC" "$A" --recv > /tmp/_rel.log 2>&1 & rp=$!; sleep 3
  TRUN_TO=8 trun "$AC" "$B" "$DA" "rel-$i" >/dev/null 2>&1
  sleep 2; kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null; sleep 1
  grep -q "\[RECV\] 5 bytes .* : rel-$i" /tmp/_rel.log && got=$((got+1))
done
[ "$got" = 3 ] && ok "reliability: 3/3 datagrams delivered" || no "reliability: datagram loss" "$got/3 delivered"

# ==========================================================================
echo; echo "########## SUMMARY ##########"
echo "  PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" = 0 ] && echo "  ALL GREEN" \
  || echo "  (Phase 1 exhaustion checks can trip the known ninep_union_fs multi-conv bug;"$'\n'"   Phase 2 needs a healthy USB-CDC -- re-run on a fresh USB if writes are wedged)"
exit $([ "$FAIL" = 0 ] && echo 0 || echo 1)
