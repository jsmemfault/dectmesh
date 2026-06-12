#!/usr/bin/env bash
#
# mount-finder.sh -- bridge a DECTstrous node's 9P port (USB-CDC) and mount it
# with 9pfuse, then pop it open in Finder. Quick-and-dirty demo helper.
#
#   ./mount-finder.sh [/dev/cu.usbmodemXXX3]
#
# With no arg it grabs the first 9P port (the one ending in '3'; x1=9151 console,
# x3=9P, x5=5340 console). Browse dev/ and dev/aether/ -- the live mesh node as a
# folder. Avoid net/ over FUSE (it's the clone/datagram fs; drive it with 9p tools).
#
set -u

PORT="${1:-$(ls /dev/cu.usbmodem*3 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no 9P port (/dev/cu.usbmodem*3) -- plug a node in"; exit 1; }

SOCK=/tmp/aether.sock
MNT=/tmp/aether
FUSE="$HOME/src/plan9port/bin/9pfuse"
[ -x "$FUSE" ] || { echo "9pfuse not found at $FUSE"; exit 1; }

# tear down any previous session (umount cleanly; never just kill the mount)
umount "$MNT" 2>/dev/null
pkill -f "9pfuse.*$MNT" 2>/dev/null
pkill -f "socat.*$SOCK" 2>/dev/null
sleep 1
rm -f "$SOCK"; mkdir -p "$MNT"

echo "bridge : $PORT  ->  $SOCK"
socat UNIX-LISTEN:"$SOCK",fork "$PORT",rawer &
sleep 2

echo "mount  : $MNT"
"$FUSE" "unix!$SOCK" "$MNT" &
sleep 2

if mount | grep -q "$MNT"; then
	echo "opening Finder at $MNT"
	open "$MNT"
	echo
	echo "mounted. try:  ls $MNT/dev/aether   cat $MNT/dev/aether/addr"
	echo "clean up with: umount $MNT ; pkill -f 'socat.*$SOCK'"
else
	echo "mount failed -- check 9pfuse/macFUSE and that the node is responsive"
	pkill -f "socat.*$SOCK" 2>/dev/null
	exit 1
fi
