#!/bin/sh
# mflt_forward.sh -- forward BOTH chips' Memfault chunks to the cloud from the
# relay's composed 9P namespace, over a single connection.
#
# The relay multiplexes two chunk streams into one namespace:
#   dev/mflt5340  -- the relay's own chunks (coredumps, reboot reasons, metrics)
#   dev/mflt9151  -- the mesh node's chunks, proxied over the inter-chip mesh link
# Each stream is self-describing: it emits a "DEV:<serial>:" line before its
# "MC:<base64>:" chunks, so this one generic loop forwards any number of chips
# without being told who they are. THIS is the 9P thesis, standalone: one
# forwarder, one namespace, two devices' telemetry -- entirely apart from the mesh.
#
# The mesh node runs the DECT NR+ PHY (no LTE), so it has no direct cloud path;
# on the bench the tethered host POSTs, in the field an LTE gateway would.
#
# Usage:  MEMFAULT_PROJECT_KEY=<key> tools/mflt_forward.sh <relay 9P port> [interval_s]
#         e.g. MEMFAULT_PROJECT_KEY=abc123 tools/mflt_forward.sh 1303 30
set -u
KEY="${MEMFAULT_PROJECT_KEY:?set MEMFAULT_PROJECT_KEY=<your Memfault project key>}"
PORT="${1:?usage: MEMFAULT_PROJECT_KEY=... $0 <relay 9P port suffix, e.g. 1303> [interval_s]}"
INT="${2:-30}"
FILES="dev/mflt5340 dev/mflt9151"
HERE=$(cd "$(dirname "$0")" && pwd)
P9="$HERE/p9do"; [ -x "$P9" ] || cc -O2 -o "$P9" "$HERE/p9do.c"
DEV="/dev/cu.usbmodem$PORT"
SOCK="/tmp/mflt_$PORT.sock"
API="https://chunks.memfault.com/api/v0/chunks"

post_chunk() {  # <device-serial> <base64-chunk> -> prints HTTP status
	printf '%s' "$2" | openssl base64 -d -A 2>/dev/null | \
		curl -s -o /dev/null -w '%{http_code}' -X POST "$API/$1" \
			-H "Memfault-Project-Key: $KEY" \
			-H "Content-Type: application/octet-stream" --data-binary @-
}

forward_cycle() {
	pkill -f "socat.*usbmodem$PORT" 2>/dev/null; rm -f "$SOCK"; sleep 1
	socat UNIX-LISTEN:"$SOCK",fork "$DEV",rawer & SP=$!; sleep 2

	total=0
	for f in $FILES; do
		while :; do
			out=$("$P9" "$SOCK" "rd:$f" 2>/dev/null)
			serial=$(printf '%s' "$out" | sed -n 's/.*DEV:\([^:]*\):.*/\1/p' | head -1)
			chunks=$(printf '%s' "$out" | grep -oE 'MC:[A-Za-z0-9+/=]*:')
			[ -n "$chunks" ] && [ -n "$serial" ] || break
			while IFS= read -r line; do
				[ -n "$line" ] || continue
				b64=${line#MC:}; b64=${b64%:}
				code=$(post_chunk "$serial" "$b64")
				case "$code" in
					200|202) total=$((total + 1)) ;;
					*) echo "  [$f -> $serial] chunk POST HTTP $code" ;;
				esac
			done <<-EOF
			$chunks
			EOF
		done
	done

	kill $SP 2>/dev/null; pkill -f "socat.*usbmodem$PORT" 2>/dev/null; rm -f "$SOCK"
	echo "$(date +%T)  forwarded $total chunk(s) from [$FILES]"
}

echo "forwarding both chips ($FILES) via $DEV -> Memfault every ${INT}s  (Ctrl-C to stop)"
trap 'pkill -f "socat.*usbmodem'"$PORT"'" 2>/dev/null; rm -f "$SOCK"; exit 0' INT TERM
while :; do forward_cycle; sleep "$INT"; done
