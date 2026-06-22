#!/usr/bin/env bash
#
# mount-term.sh -- bridge a DECTstrous node's 9P port (USB-CDC) and mount it with
# 9pfuse for hand navigation in the TERMINAL (no Finder). The socat + 9pfuse run
# in the background and OUTLIVE this script, so you keep browsing after it returns.
#
#   ./mount-term.sh [/dev/cu.usbmodemXXX3] [tag]
#
# With no PORT it grabs the first 9P port (ending in '3'; x1=9151 console, x3=9P,
# x5=5340 console). dev/ is the static node tree (fast). net/aether is the clone-
# proxy: `ls`/`cat` a handful of entries is fine; avoid hammering readdirs (the
# bursty 9151 proxy can't keep up and the CDC back-pressures).
#
# TAG (optional) suffixes the socket + mount point so you can mount MORE THAN ONE
# node at once without collision. No tag -> /tmp/aether.sock + /tmp/aether. Tag
# "b" -> /tmp/aetherb.sock + /tmp/aetherb. Each instance tears down only its own
# socket/mount, so run it once per node:
#   ./mount-term.sh /dev/cu.usbmodem1203 a
#   ./mount-term.sh /dev/cu.usbmodem2103 b
#
# IMPORTANT: run from a real Terminal.app/iTerm session. Under Claude Code's `!`
# prefix the backgrounded socat/9pfuse get reaped on return and the mount dies.
#
set -u

PORT="${1:-$(ls /dev/cu.usbmodem*3 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no 9P port (/dev/cu.usbmodem*3) -- plug a node in"; exit 1; }
TAG="${2:-}"

SOCK="/tmp/aether${TAG}.sock"
MNT="/tmp/aether${TAG}"
FUSE="$HOME/src/plan9port/bin/9pfuse"
[ -x "$FUSE" ] || { echo "9pfuse not found at $FUSE"; exit 1; }

# tear down any previous session for THIS instance (umount cleanly; never just
# kill the mount). Scoped by $SOCK/$MNT so other tagged mounts are left alone.
umount "$MNT" 2>/dev/null
pkill -f "9pfuse.*$MNT" 2>/dev/null
pkill -f "socat.*$SOCK" 2>/dev/null
sleep 1
rm -f "$SOCK"; mkdir -p "$MNT"

echo "bridge : $PORT  ->  $SOCK"
socat UNIX-LISTEN:"$SOCK",fork "$PORT",rawer &
sleep 2

echo "mount  : $MNT  (-A 60: cache lookups+attrs 60s to quiet the macOS storm)"
"$FUSE" -A 60 "unix!$SOCK" "$MNT" &
sleep 2

if mount | grep -q "$MNT"; then
	# Keep Spotlight off the mount -- otherwise mds_stores readdir's/stats the
	# whole volume in the background (incl. net/), storming the proxy + CDC.
	mdutil -i off "$MNT"        >/dev/null 2>&1 || true
	mdutil -d "$MNT"            >/dev/null 2>&1 || true   # disable indexing store
	echo
	echo "mounted at $MNT -- navigate in THIS terminal. Mount stays up after exit."
	echo
	echo "  cd $MNT"
	echo "  ls                       # dev  net"
	echo "  ls dev                   # static node tree (fast)"
	echo "  cat dev/aether/addr      # this node's mesh address"
	echo "  ls net/aether            # clone-proxy: addr clone maxmsg stats status"
	echo "  cat net/aether/status    # live conv count; cat maxmsg, stats, addr ..."
	echo
	echo "  tip: use plain ls/cat, not 'ls -lR' or tab-completion sprees on net/ --"
	echo "       each stat is a proxied round-trip to the bursty 9151."
	echo
	echo "clean up: umount $MNT ; pkill -f 'socat.*$SOCK'"
else
	echo "mount failed -- check 9pfuse/macFUSE and that the node is responsive"
	pkill -f "socat.*$SOCK" 2>/dev/null
	exit 1
fi
