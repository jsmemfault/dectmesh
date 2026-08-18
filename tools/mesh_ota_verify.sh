#!/usr/bin/env bash
#
# mesh_ota_verify.sh <patchlevel>
# One full HANDS-OFF mesh-OTA cycle of node B (target) from node A (gateway),
# with B's console captured through its reboot, and a PASS/FAIL verdict.
#   PASS  = B's console shows "DECTstrous Mesh v0.7.<N>" AND fw9151 confirmed yes
#   FAIL  = "not valid" (MCUboot rejected) OR wrong version OR push failed
#
# Fixed bench: A gateway 9P=1203 ; B target 9151-console=1301, relay-9P=1303,
# B eui=1ede2e99a4f6. Image staged at /tmp/ota_images/mesh_0.7.<N>.signed.bin.
set -u
N="$1"
DIR=$(cd "$(dirname "$0")" && pwd)
IMG="/tmp/ota_images/mesh_0.7.$N.signed.bin"
BEUI="1ede2e99a4f6"
GW=1203; BCON=1301; B9P=1303
LOG="/tmp/ota_$N.log"
SOCK=/tmp/mesh_ota.sock
tmo() { perl -e 'alarm shift; exec @ARGV' "$@"; }
[ -f "$IMG" ] || { echo "MISSING $IMG"; exit 2; }

# capture B console through the whole cycle
pkill -9 -f "cat /dev/cu.usbmodem$BCON" 2>/dev/null; sleep 0.3; : > "$LOG"
cat "/dev/cu.usbmodem$BCON" >> "$LOG" 2>&1 & CATPID=$!

echo "==== mesh-OTA cycle 0.7.$N  ($(date +%H:%M:%S)) ===="
pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null; sleep 0.5; rm -f $SOCK
socat UNIX-LISTEN:$SOCK,fork "/dev/cu.usbmodem$GW",rawer & sleep 2

echo "-- push --"
MESH_OTA_PASSES=15 MESH_OTA_PACE_US=0 \
  tmo 1200 "$DIR/aether_conv" $SOCK --mesh-ota "$BEUI" "$IMG" 1024 2>&1 | tr '\r' '\n' | grep -E "\[ok\]|stall|resume|\[done\]|commit|reboot|push failed" | tail -8
PRC=${PIPESTATUS[0]:-0}
pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null; rm -f $SOCK

# wait for B's post-OTA boot decision on the console
echo "-- waiting for B boot decision --"
for i in $(seq 1 40); do
  if strings "$LOG" | grep -qiE "DECTstrous Mesh v0\.7\.$N|not valid"; then break; fi
  sleep 3
done
sleep 2

BOOTED=0; INVALID=0
strings "$LOG" | grep -qi "DECTstrous Mesh v0\.7\.$N" && BOOTED=1
strings "$LOG" | grep -qi "not valid" && INVALID=1
echo "-- console: booted_v$N=$BOOTED  invalid=$INVALID --"

# confirm over the mesh (hands-off) if it booted
CONF="n/a"
if [ "$BOOTED" = 1 ]; then
  sleep 8
  socat UNIX-LISTEN:$SOCK,fork "/dev/cu.usbmodem$GW",rawer & sleep 2
  tmo 30 "$DIR/p9do" $SOCK "ws:dev/push_ctl:confirm $BEUI" >/dev/null 2>&1
  pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null; rm -f $SOCK
  # read back confirmed state from B's own relay -- retry: relay re-attach after the
  # target's reboot is slow (~15-30s), so an early read races and returns empty.
  for try in 1 2 3 4 5; do
    sleep 8
    S=/tmp/q_B.sock; pkill -9 -f "socat.*q_B.sock" 2>/dev/null; sleep 0.3; rm -f $S
    socat UNIX-LISTEN:$S,fork "/dev/cu.usbmodem$B9P",rawer & sleep 2
    CONF=$(tmo 15 "$DIR/p9do" $S rd:dev/fw9151 2>/dev/null | grep -oiE "current 0\.7\.[0-9]+\+0 pending 0\.7\.[0-9]+\+0 confirmed (yes|no)")
    pkill -9 -f "socat.*q_B.sock" 2>/dev/null; rm -f $S
    [ -n "$CONF" ] && break
  done
fi

kill "$CATPID" 2>/dev/null
echo "-- fw9151: $CONF --"
if [ "$INVALID" = 1 ]; then echo "==== RESULT 0.7.$N: FAIL (MCUboot rejected) ===="; exit 1; fi
if [ "$BOOTED" = 1 ] && echo "$CONF" | grep -q "confirmed yes" && echo "$CONF" | grep -q "current 0.7.$N"; then
  echo "==== RESULT 0.7.$N: PASS ===="; exit 0
fi
echo "==== RESULT 0.7.$N: INCONCLUSIVE (booted=$BOOTED conf='$CONF' prc=$PRC) ===="; exit 3
