#!/usr/bin/env bash
#
# interrupt_resume.sh <patchlevel>
# Validate the finalize-decouple fix (9p4z 050a23f) with a REALISTIC interrupted OTA:
#   1. start a mesh-OTA push to B
#   2. KILL it mid-stream (~40-50%) -> peer left with a PARTIAL DFU in RECEIVING state,
#      no Tclunk/commit (exactly what a wedge/crash leaves behind)
#   3. re-run --mesh-ota to completion -> its "target" step re-versions the peer's mesh
#      9P server, which clunks the still-open dev/firmware fid. BEFORE the fix that clunk
#      FINALIZED the partial image -> corrupt -> MCUboot "not valid". The fix skips
#      finalize on a session-reset, so the idempotent dfu_write fills the rest and only
#      the explicit commit finalizes -> a VALID image.
# PASS = B boots "v0.7.<N>" (valid) after the interrupted+resumed transfer.
set -u
N="$1"
DIR=$(cd "$(dirname "$0")" && pwd)
IMG="/tmp/ota_images/mesh_0.7.$N.signed.bin"
BEUI="1ede2e99a4f6"; GW=1203; BCON=1301; B9P=1303
SOCK=/tmp/mesh_ota.sock; LOG="/tmp/resume_$N.log"
tmo() { perl -e 'alarm shift; exec @ARGV' "$@"; }
[ -f "$IMG" ] || { echo "MISSING $IMG"; exit 2; }

pkill -9 -f "cat /dev/cu.usbmodem$BCON" 2>/dev/null; sleep 0.3; : > "$LOG"
cat "/dev/cu.usbmodem$BCON" >> "$LOG" 2>&1 & CATPID=$!

echo "==== interrupt+resume test 0.7.$N ($(date +%H:%M:%S)) ===="
pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null; sleep 0.5; rm -f $SOCK
socat UNIX-LISTEN:$SOCK,fork "/dev/cu.usbmodem$GW",rawer & sleep 2

echo "-- phase 1: start push, then KILL mid-stream --"
tmo 1200 "$DIR/aether_conv" $SOCK --mesh-ota "$BEUI" "$IMG" 1024 > /tmp/resume_push1_$N.log 2>&1 &
PUSHPID=$!
# let it get well into the stream (~30s ~= 40-50%)
sleep 32
kill -9 $PUSHPID 2>/dev/null
# also kill the underlying aether_conv (perl exec replaced the shell, so PUSHPID IS it,
# but be thorough)
pkill -9 -f "aether_conv.*mesh-ota" 2>/dev/null
echo "-- killed push at:"; tr '\r' '\n' < /tmp/resume_push1_$N.log | grep -oE "[0-9]+ / 305[0-9]+ B \([0-9]+%\)" | tail -1
sleep 4

echo "-- phase 2: RE-RUN --mesh-ota to completion (re-targets = Tversion on the partial) --"
pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null; sleep 0.5; rm -f $SOCK
socat UNIX-LISTEN:$SOCK,fork "/dev/cu.usbmodem$GW",rawer & sleep 2
MESH_OTA_PASSES=15 tmo 1200 "$DIR/aether_conv" $SOCK --mesh-ota "$BEUI" "$IMG" 1024 2>&1 | tr '\r' '\n' | grep -E "\[ok\]|stall|resume|\[done\]|commit|reboot|push failed" | tail -8
pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null; rm -f $SOCK

echo "-- waiting for B boot decision --"
for i in $(seq 1 40); do
  strings "$LOG" | grep -qiE "DECTstrous Mesh v0\.7\.$N|not valid" && break; sleep 3
done
sleep 2
BOOTED=0; INVALID=0
strings "$LOG" | grep -qi "DECTstrous Mesh v0\.7\.$N" && BOOTED=1
strings "$LOG" | grep -qi "not valid" && INVALID=1
echo "-- console: booted_v$N=$BOOTED  invalid=$INVALID --"
strings "$LOG" | grep -aiE "DECTstrous Mesh v0\.7\.[0-9]+|not valid" | tail -3

kill "$CATPID" 2>/dev/null
if [ "$INVALID" = 1 ]; then echo "==== RESULT: FAIL -- finalize fix BROKEN (partial finalized -> corrupt) ===="; exit 1; fi
if [ "$BOOTED" = 1 ]; then echo "==== RESULT: PASS -- interrupted+resumed image VALID (finalize-decouple works) ===="; exit 0; fi
echo "==== RESULT: INCONCLUSIVE (no boot decision seen; check $LOG) ===="; exit 3
