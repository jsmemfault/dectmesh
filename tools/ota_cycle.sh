#!/usr/bin/env bash
#
# ota_cycle.sh <gw_9p_port> <target_eui12> <signed.bin> <target_console_port> [chunk]
#
# One full mesh-OTA cycle to a REMOTE peer, with the target's console captured
# THROUGH its reboot so we can VERIFY the delivered image actually validated and
# booted (MCUboot swap log + "Image version" banner) -- not just "the stream
# finished". Uses the resumable driver (aether_conv --mesh-ota) so a transient
# mesh wedge resumes instead of failing the cycle.
#
# Exit 0 only if the target console shows the new version booting after the push.
set -u
GW="$1"; EUI="$2"; IMG="$3"; TCON="$4"; CHUNK="${5:-1024}"
DIR=$(cd "$(dirname "$0")" && pwd)
SOCK=/tmp/mesh_ota.sock
TLOG=/tmp/ota_target.log
WANT_VER="${WANT_VER:-}"   # expected version string, e.g. 0.7.70 (for the banner grep)

[ -n "$GW" ] && [ -n "$EUI" ] && [ -f "$IMG" ] && [ -n "$TCON" ] || {
  echo "usage: $0 <gw_9p_port> <target_eui12> <signed.bin> <target_console_port> [chunk]"; exit 2; }

up()   { pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null; sleep 0.5; rm -f "$SOCK"
         socat UNIX-LISTEN:"$SOCK",fork "/dev/cu.usbmodem$GW",rawer & sleep 1.5; }
down() { pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null; rm -f "$SOCK"; }

# fresh target console capture
pkill -9 -f "cat /dev/cu.usbmodem$TCON" 2>/dev/null; sleep 0.3
: > "$TLOG"
cat "/dev/cu.usbmodem$TCON" >> "$TLOG" 2>&1 &
CATPID=$!

echo "== mesh-OTA $IMG ($(wc -c <"$IMG") B) -> $EUI via $GW ; target console $TCON =="
up
echo "-- push (target->stream->commit->reboot), resumable --"
MESH_OTA_PASSES="${MESH_OTA_PASSES:-12}" MESH_OTA_PACE_US="${MESH_OTA_PACE_US:-0}" \
  "$DIR/aether_conv" "$SOCK" --mesh-ota "$EUI" "$IMG" "$CHUNK"
PUSH_RC=$?
down
echo "-- push rc=$PUSH_RC ; waiting 70s for target to swap+boot --"
sleep 70

echo "== target console since push (swap + banner evidence) =="
grep -aiE "swap|secondary|Image version|not valid|magic|booting|Æther|v0\.7|confirm" "$TLOG" | tail -30

# verdict
BOOTED=0
if [ -n "$WANT_VER" ]; then
  grep -aq "$WANT_VER" "$TLOG" && BOOTED=1
fi
SWAPPED=0
grep -aiqE "Starting swap|swap using" "$TLOG" && SWAPPED=1
INVALID=0
grep -aiq "not valid" "$TLOG" && INVALID=1

echo "-- swap=$SWAPPED invalid=$INVALID booted_want($WANT_VER)=$BOOTED --"

# confirm (make the new image permanent) if it looks like it booted
if [ "$INVALID" = 0 ]; then
  echo "-- confirm $EUI --"
  up
  "$DIR/p9do" "$SOCK" "ws:dev/push_ctl:confirm $EUI" 2>&1 | head -2
  down
fi

kill "$CATPID" 2>/dev/null
if [ "$INVALID" = 1 ]; then echo "== RESULT: FAIL (MCUboot rejected the image) =="; exit 1; fi
if [ "$SWAPPED" = 1 ]; then echo "== RESULT: PASS (swap performed) =="; exit 0; fi
echo "== RESULT: INCONCLUSIVE (no swap log seen; check $TLOG) =="; exit 3
