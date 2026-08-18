#!/usr/bin/env bash
#
# usb_ota.sh <9p_port> <console_port> <signed.bin> [want_hash]
#
# Reliable LOCAL (wired, over USB) OTA of a node's own 9151 -- the baseline path.
# Follows the proven recipe: single --put (sole op) -> reboot9151 -> settle ->
# confirm9151, then verifies the new git hash on the 9151 console banner.
set -u
P9="$1"; CON="$2"; IMG="$3"; WANT="${4:-}"
DIR=$(cd "$(dirname "$0")" && pwd)
SOCK=/tmp/usb_ota_$P9.sock
CLOG=/tmp/usb_ota_con_$P9.log
g() { perl -e 'alarm shift; exec @ARGV' "$@"; }   # timeout guard

[ -f "$IMG" ] || { echo "no image $IMG"; exit 2; }

# capture console through the reboot
pkill -9 -f "cat /dev/cu.usbmodem$CON" 2>/dev/null; sleep 0.3; : > "$CLOG"
cat "/dev/cu.usbmodem$CON" >> "$CLOG" 2>&1 & CATPID=$!

pkill -9 -f "socat.*usb_ota_$P9.sock" 2>/dev/null; sleep 0.5; rm -f "$SOCK"
socat UNIX-LISTEN:"$SOCK",fork "/dev/cu.usbmodem$P9",rawer & sleep 1.5

echo "== USB OTA $IMG ($(wc -c <"$IMG") B) -> 9151 on $P9 =="
echo "-- put dev/fw9151 (sole op) --"
g 90 "$DIR/aether_conv" "$SOCK" --put dev/fw9151 "$IMG" 1024 | tail -2
echo "-- reboot9151 (ws reply hangs = benign) --"
g 12 "$DIR/p9do" "$SOCK" ws:dev/reboot9151:1 2>&1 | head -1
pkill -9 -f "socat.*usb_ota_$P9.sock" 2>/dev/null; rm -f "$SOCK"
echo "-- settle 60s (MCUboot swap + relay re-attach) --"
sleep 60

socat UNIX-LISTEN:"$SOCK",fork "/dev/cu.usbmodem$P9",rawer & sleep 1.5
echo "-- confirm9151 --"
g 15 "$DIR/p9do" "$SOCK" ws:dev/confirm9151:1 2>&1 | head -1
pkill -9 -f "socat.*usb_ota_$P9.sock" 2>/dev/null; rm -f "$SOCK"
kill "$CATPID" 2>/dev/null

echo "== console banner evidence =="
grep -aiE "Booting DECTstrous|Image version|swap|not valid|v0\.7" "$CLOG" | tail -15
if [ -n "$WANT" ]; then
  if grep -aq "$WANT" "$CLOG"; then echo "== PASS: git $WANT booted =="; exit 0
  else echo "== NOT CONFIRMED: git $WANT not seen in banner (check $CLOG) =="; exit 1; fi
fi
