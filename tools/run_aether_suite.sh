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
# Settle between tool-to-tool port handoffs. Each tool (p9do/aether_test/aether_conv)
# opens the port via socat,fork; closing it deasserts DTR and the relay's size-1
# session pool tears the session down, then the next tool's open re-asserts DTR and
# re-establishes. Back-to-back (no settle) hiccups the relay CDC -> the next tool's
# first write gets EPIPE. A beat between handoffs avoids it. (The link is fine -- a
# standalone aether_test is 23/23; only the un-settled handoff trips it.)
settle(){ sleep 3; }

# --- build the host tools --------------------------------------------------
cc -O2 -o "$AC" "$HERE/aether_conv.c" || { echo "build aether_conv failed"; exit 2; }
cc -O2 -o "$AT" "$HERE/aether_test.c" || { echo "build aether_test failed"; exit 2; }
cc -O2 -o "$P9" "$HERE/p9do.c"        || { echo "build p9do failed"; exit 2; }

# --- ports + socats --------------------------------------------------------
# Thingy relay 9P data port is the CDC suffix "...03" (01=9151 console, 03=9P,
# 05=5340 console). Match "*03" specifically so a co-connected nRF9151 DK (whose
# J-Link CDC enumerates as a long serial ...991/...993) is NOT mistaken for a 9P
# port -- the DK is a third mesh node now, not a suite endpoint. Override with
# PORTS="/dev/cu.A /dev/cu.B" to pick explicitly.
PORTS=(${PORTS:-$(ls /dev/cu.usbmodem*03 2>/dev/null)})
[ "${#PORTS[@]}" -ge 2 ] || { echo "need two thingy 9P ports (found ${#PORTS[@]}); set PORTS=... to override"; exit 2; }
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

settle  # let the convergence reads' connection fully release before Phase 1 opens

# ==========================================================================
echo; echo "########## Phase 1: single-node /net/aether conformance (node A) ##########"
P1=$("$AT" "$A")
echo "$P1" | sed 's/^/  /'
r=$(echo "$P1" | grep -oE '[0-9]+ passed, [0-9]+ failed')
[ -n "$r" ] && { PASS=$((PASS + ${r% passed*})); FAIL=$((FAIL + $(echo "$r" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+'))); }
settle  # Phase 1 (aether_test) closed node A; let it release before Phase 2 opens

# ==========================================================================
echo; echo "########## Phase 2: two-node datagram delivery (A <-> B) ##########"

# announced receive: <recv_sock> <send_sock> <dst> <payload> <expect-src> <label>
ann(){
  local got=""
  for attempt in 1 2; do
    "$AC" "$1" --recv > /tmp/_r.log 2>&1 & local rp=$!; sleep 5
    TRUN_TO=10 trun "$AC" "$2" "$3" "$4" >/dev/null 2>&1
    sleep 3; kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null; settle
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
    "$AC" "$1" --crecv "$3" > /tmp/_c.log 2>&1 & local rp=$!; sleep 5
    TRUN_TO=10 trun "$AC" "$2" "$4" "$5" >/dev/null 2>&1
    sleep 3; kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null; settle
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

# reliability: N sequential datagrams to ONE held receiver from ONE held sender.
# Both sides hold a single session (no per-send respawn) -> zero harness churn,
# so the delivered count reflects GENUINE mesh delivery, not test artifacts.
REL_N=${REL_N:-10}
echo "  -- reliability: $REL_N sequential datagrams B->A (both sides held) --"
"$AC" "$A" --recv "$REL_N" > /tmp/_rel.log 2>&1 & rp=$!; sleep 5                       # held receiver
TRUN_TO=$((REL_N * 2 + 15)) trun "$AC" "$B" --sendn "$DA" "$REL_N" 1200 >/dev/null 2>&1 # held sender
sleep 3; kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null; settle
rgot=$(grep -c '\[RECV\]' /tmp/_rel.log)
[ "$rgot" -ge "$REL_N" ] && ok "reliability: $rgot/$REL_N datagrams delivered (both sides held)" \
  || no "reliability: datagram loss" "$rgot/$REL_N delivered (both sides held)"

# --- Phase 2b: concurrent multi-conversation isolation ---------------------
# One held session on A holds 3 convs: c1 connect(B), c2 connect(bogus peer),
# c3 announce. B sends ONE datagram -> c1 (peer match) and c3 (announced) must
# receive it; c2 (connected to a different peer) must NOT. Proves peer-filtering
# + announced-receives-all + per-conversation isolation under concurrent traffic.
echo "  -- isolation: 3 concurrent convs on A (connect B / connect other / announce) --"
"$AC" "$A" --iso "$DB" "00:00:00:00:00:99" > /tmp/_iso.log 2>&1 & rp=$!; sleep 4
TRUN_TO=8 trun "$AC" "$B" "$DA" "iso-probe" >/dev/null 2>&1
sleep 3; kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null; settle
i1=$(grep -c '\[ISO c1\]' /tmp/_iso.log); i2=$(grep -c '\[ISO c2\]' /tmp/_iso.log); i3=$(grep -c '\[ISO c3\]' /tmp/_iso.log)
if [ "$i1" -ge 1 ] && [ "$i3" -ge 1 ] && [ "$i2" = 0 ]; then
  ok "isolation: connected(B) + announced received, connected(other) filtered (c1=$i1 c2=$i2 c3=$i3)"
else
  no "isolation: peer-filtering / concurrent delivery" "c1=$i1 (want>=1)  c2=$i2 (want 0)  c3=$i3 (want>=1)"
fi

# ==========================================================================
# Soak: repeated full conformance, asserting status returns to 0/4 every cycle.
# This is the regression net for the conversation leak -- if convs ever stop
# freeing (on clunk or disconnect), status climbs and a cycle fails. Exercises
# both in-session teardown (aether_test's own clone/clunk) and cross-session
# teardown (status read after each run is a fresh session). SOAK_N overrides count.
echo; echo "########## Phase 3: soak -- ${SOAK_N:-5}x conformance, no conversation leak over time ##########"
SOAK_N=${SOAK_N:-5}
soak_fail=0
for c in $(seq 1 "$SOAK_N"); do
  rr=$("$AT" "$A" 2>&1 | grep -oE '[0-9]+ passed, [0-9]+ failed')
  settle
  st=$("$P9" "$A" rd:net/aether/status 2>/dev/null | sed -n 's#.*=> ##p' | tr -cd '0-9/')
  printf '  cycle %d/%d: %s ; status=%s\n' "$c" "$SOAK_N" "${rr:-NO RESULT}" "${st:-?}"
  echo "$rr" | grep -q '^23 passed, 0 failed' || soak_fail=$((soak_fail+1))
  [ "${st%%/*}" = "0" ] || soak_fail=$((soak_fail+1))
  settle
done
[ "$soak_fail" = 0 ] && ok "soak: $SOAK_N cycles all 23/23 + status back to 0/4 (no conversation leak over time)" \
  || no "soak: leak/regression over $SOAK_N cycles" "$soak_fail failed check(s)"

# ==========================================================================
# Phase 4: the new realities (firmware >= 0.7.23). Durable 6-byte identity
# DECOUPLED from the HONR routing address, and the §6a broadcast party line
# carrying that identity as src. On older firmware these FAIL by design (the
# address was the HONR-derived 00:00:00:00:<honr>, mutable and not an identity).
echo; echo "########## Phase 4: durable identity + §6a broadcast party-line ##########"

# this node's durable identity (net/aether/addr) vs its HONR addr (dev/aether/addr)
ident(){ "$P9" "$1" rd:net/aether/addr 2>/dev/null | sed -n 's#.*=> ##p' | tr -cd '0-9a-f:'; }

# 4a: identity is a 6-byte locally-administered unicast id, decoupled from HONR
id_check(){  # <sock> <label>
  local id b0 hi hn
  id=$(ident "$1"); settle
  hn=$(hex4 "$("$P9" "$1" rd:dev/aether/addr 2>/dev/null | sed -n 's#.*=> ##p')"); settle
  if ! echo "$id" | grep -qE '^[0-9a-f]{2}(:[0-9a-f]{2}){5}$'; then
    no "$2 identity is 6-byte colon-hex" "got '$id'"; return; fi
  b0=$((16#${id%%:*}))
  # locally-administered (bit1=1) + unicast (bit0=0)  ->  byte0 & 0x03 == 0x02
  if [ $(( b0 & 3 )) -ne 2 ]; then
    no "$2 identity is LAA-unicast" "byte0=$(printf %02x "$b0")"; return; fi
  # decoupled from HONR: NOT the old 00:00:00:00:<honr> shape (high 4 bytes != 0)
  hi=$(echo "$id" | cut -d: -f1-4 | tr -cd '0-9a-f')
  if [ "$hi" = "00000000" ]; then
    no "$2 identity decoupled from HONR" "HONR-derived shape: $id"; return; fi
  ok "$2 durable identity $id (LAA-unicast, decoupled from HONR=$hn)"
}
settle
id_check "$A" "node A"
id_check "$B" "node B"

# 4a-ii: §6a status surface -- a connect-ff:ff conversation MUST report
#        "broadcast best-effort" (distinct from connected/announced/unconnected),
#        so a client can tell it has joined the party line (spec §6a).
bstat_check(){  # <sock> <label>
  local st
  st=$("$AC" "$1" --bstatus 2>&1 | sed -n 's/.*status: //p'); settle
  if [ "$st" = "broadcast best-effort" ]; then ok "$2 §6a status: '$st'"
  else no "$2 §6a status" "got '$st' (expected 'broadcast best-effort')"; fi
}
bstat_check "$A" "node A"

# 4b: a connect-ff:ff (§6a) receiver sees the SENDER'S DURABLE IDENTITY as src,
#     not 00:00:00:00:<honr>. B broadcasts; A receives. Best-effort -> retry once.
bsrc(){  # <recv_sock> <send_sock> <expect-identity> <label>
  local got="" src
  for attempt in 1 2; do
    "$AC" "$1" --brecv 1 > /tmp/_b.log 2>&1 & local rp=$!; sleep 5
    TRUN_TO=10 trun "$AC" "$2" ff:ff:ff:ff:ff:ff >/dev/null 2>&1
    sleep 3; kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null; settle
    got=$(grep '\[BRECV\]' /tmp/_b.log | head -1); [ -n "$got" ] && break
  done
  if [ -z "$got" ]; then no "$4" "no broadcast received (2 attempts)"; return; fi
  # match only a full 6-octet address -- the payload ("...from aether_conv") also
  # contains the word "from", so a loose 'from [0-9a-f:]+' would mis-capture "ae".
  src=$(echo "$got" | grep -oE '([0-9a-f]{2}:){5}[0-9a-f]{2}' | head -1)
  if [ "$src" = "$3" ]; then ok "$4 (src=$src == sender identity)"
  else no "$4" "src=$src expected sender identity $3"; fi
}
idB=$(ident "$B"); settle
bsrc "$A" "$B" "$idB" "§6a broadcast B->A: src is B's durable identity"

# 4c: own broadcast is NOT echoed back to the originator (identity-keyed own-echo
#     guard). A holds a §6a receiver AND originates -- it must not see itself.
idA=$(ident "$A"); settle
"$AC" "$A" --brecv 1 > /tmp/_e.log 2>&1 & ep=$!; sleep 5
TRUN_TO=10 trun "$AC" "$A" ff:ff:ff:ff:ff:ff >/dev/null 2>&1
sleep 3; kill "$ep" 2>/dev/null; wait "$ep" 2>/dev/null; settle
if grep -q "from $idA" /tmp/_e.log 2>/dev/null; then
  no "own-echo suppression" "node A received its own broadcast (src=$idA)"
else ok "own-echo suppression (A did not receive its own broadcast)"; fi

# NOTE -- deliberately NOT asserted device-in-the-loop (belongs in the native_sim
# multinode harness, where topology + counters are deterministic):
#   * Tree-aware relay (leaf stays silent / interior re-broadcasts): packets_-
#     forwarded is shell-only, and a co-located bench is a flat 1-hop star, so
#     "leaf vs interior" can't be distinguished here. native_sim builds a known
#     tree and asserts the forwarded-count delta per node.
#   * Identity<->link coherence (heymac short_addr == identity[4:5]): the link
#     short address isn't exposed over 9P; a native_sim unit test on the HeyMac
#     identity generation asserts the slice + the LAA-unicast bits directly.
# Also note: Phase 2's announced/connected datagrams intentionally still carry a
# HONR-derived src (00:00:00:00:<honr>) -- unicast keeps the routing address as
# src so stop-and-wait ARQ ACKs can route back. Only the §6a BROADCAST src is the
# durable identity (above). That split is by design until unicast is decoupled.

# ==========================================================================
echo; echo "########## SUMMARY ##########"
echo "  PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" = 0 ] && echo "  ALL GREEN" \
  || echo "  (STRICT: a FAIL is a genuine gap, not flaky-by-design --"$'\n'"   - Phase 1 / soak fail WITH status 0/4: timing/handoff hiccup (bump settle) or a real conformance regression."$'\n'"   - reliability X/N (both sides held, zero harness churn): genuine mesh datagram loss"$'\n'"     -> investigate aether_mesh_send_reliable ack/retry budget + the DECT PHY (known ~90%)."$'\n'"   - soak fail WITH status climbing: the conversation leak is back (must stay 0/4)."$'\n'"   Sanity: a standalone 'tools/aether_test <sock>' should be 23/23.)"
exit $([ "$FAIL" = 0 ] && echo 0 || echo 1)
