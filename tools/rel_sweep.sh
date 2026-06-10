#!/usr/bin/env bash
# rel_sweep.sh -- characterize /net/aether reliable-datagram delivery under load.
#
# The suite's reliability check is a 10/10 spot test at a lazy 1200ms gap. This
# sweeps higher N, BOTH directions, and TIGHTENS the inter-send gap to find where
# the stop-and-wait ARQ starts to drop. Receiver [RECV] count is ground truth
# (sender [SENT] only means "handed to the mesh").
#
# Usage: bash tools/rel_sweep.sh            (defaults: N=30, gaps "1200 600 300 150")
#        REL_N=50 GAPS="1000 400 200" bash tools/rel_sweep.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
AC="$HERE/aether_conv"; P9="$HERE/p9do"
SA=/tmp/aether_A.sock; SB=/tmp/aether_B.sock
N=${REL_N:-30}
GAPS="${GAPS:-1200 600 300 150}"

cc -O2 -o "$AC" "$HERE/aether_conv.c" || exit 2
cc -O2 -o "$P9" "$HERE/p9do.c"        || exit 2

PORTS=($(ls /dev/cu.usbmodem*3 2>/dev/null))
[ "${#PORTS[@]}" -ge 2 ] || { echo "need two 9P ports (found ${#PORTS[@]})"; exit 2; }
pkill -f 'socat.*usbmodem' 2>/dev/null; sleep 2; rm -f "$SA" "$SB"
socat UNIX-LISTEN:"$SA",fork "${PORTS[0]}",rawer & socat UNIX-LISTEN:"$SB",fork "${PORTS[1]}",rawer & sleep 3
trap 'pkill -f "socat.*usbmodem" 2>/dev/null' EXIT

conv_read(){ "$P9" "$1" rd:dev/aether/nodeid rd:dev/aether/addr 2>/dev/null; }
hex4(){ local a; a=$(echo "$1" | tr -cd '0-9a-f'); echo "${a:0:4}"; }

echo "converging..."
adA=""; adB=""
for t in $(seq 1 10); do
  oa=$(conv_read "$SA"); ob=$(conv_read "$SB")
  adA=$(hex4 "$(echo "$oa" | sed -n 's#.*addr => ##p')")
  adB=$(hex4 "$(echo "$ob" | sed -n 's#.*addr => ##p')")
  [ "${#adA}" -ge 3 ] && [ "${#adB}" -ge 3 ] && [ "$adA" != "$adB" ] && break
  sleep 2
done
DA="00:00:00:00:${adA:0:2}:${adA:2:2}"
DB="00:00:00:00:${adB:0:2}:${adB:2:2}"
echo "node A addr=$adA ($DA)   node B addr=$adB ($DB)"
[ "$adA" != "$adB" ] || { echo "convergence failed"; exit 2; }
sleep 3

# relrun <recv_sock> <send_sock> <dst-of-recv> <gap_ms> -> prints "got/N"
relrun(){
  local rs=$1 ss=$2 dst=$3 gap=$4
  "$AC" "$rs" --recv "$N" > /tmp/_sweep_r.log 2>&1 & local rp=$!; sleep 5
  # sender held; bound generously: each send may block on ARQ (up to ~1s) + gap
  local budget=$(( N * (gap/1000 + 2) + 20 ))
  "$AC" "$ss" --sendn "$dst" "$N" "$gap" >/tmp/_sweep_s.log 2>&1 & local sp=$!
  for i in $(seq 1 "$budget"); do kill -0 "$sp" 2>/dev/null || break; sleep 1; done
  kill "$sp" 2>/dev/null; sleep 1; kill -9 "$sp" 2>/dev/null   # SIGTERM -> clunk; -9 backstop
  sleep 4   # drain in-flight retransmits
  kill "$rp" 2>/dev/null; wait "$rp" 2>/dev/null   # SIGTERM -> receiver clunks (no conv leak)
  local got; got=$(grep -c '\[RECV\]' /tmp/_sweep_r.log)
  echo "$got"
}

echo
printf "  %-10s %-8s %-8s %s\n" "gap(ms)" "B->A" "A->B" "notes"
printf "  %-10s %-8s %-8s %s\n" "-------" "----" "----" "-----"
totg=0; totn=0
FLOOR=${FLOOR:-300}   # never drive below this gap: <~300ms bursts wedge the uart1 9P link
for g in $GAPS; do
  if [ "$g" -lt "$FLOOR" ]; then
    printf "  %-10s %-8s %-8s %s\n" "$g" "SKIP" "SKIP" "below ${FLOOR}ms floor (burst-wedge guard; set FLOOR= to override)"
    continue
  fi
  ba=$(relrun "$SA" "$SB" "$DA" "$g"); sleep 3
  ab=$(relrun "$SB" "$SA" "$DB" "$g"); sleep 3
  totg=$(( totg + ba + ab )); totn=$(( totn + 2*N ))
  note=""; [ "$ba" -ge "$N" ] && [ "$ab" -ge "$N" ] && note="perfect"
  printf "  %-10s %-8s %-8s %s\n" "$g" "$ba/$N" "$ab/$N" "$note"
done
echo
echo "  TOTAL delivered: $totg/$totn  ($(awk "BEGIN{printf \"%.1f\", 100*$totg/$totn}")%)"
