#!/bin/sh
# mflt_forward.sh -- forward a DECTstrous node's Memfault chunks to the cloud.
#
# The mesh node runs the DECT NR+ PHY, not the LTE modem, so it has no direct
# path to the internet. Memfault packetizes metrics / reboots / coredumps into
# transport-agnostic "chunks"; the node exposes them as a 9P file (`dev/mflt`),
# and this reads that file over 9P and POSTs each chunk to Memfault. On the bench
# the tethered host runs this; in the field a gateway (LTE nRF91) would.
#
# The device serial is the node's CGA (node_eui) -- matching the firmware's
# Memfault device id -- so the cryptographic identity IS the fleet's device name.
#
# Usage:  MEMFAULT_PROJECT_KEY=<key> tools/mflt_forward.sh <9P port suffix> [interval_s]
#         e.g. MEMFAULT_PROJECT_KEY=abc123 tools/mflt_forward.sh 1303 30
set -u
KEY="${MEMFAULT_PROJECT_KEY:?set MEMFAULT_PROJECT_KEY=<your Memfault project key>}"
PORT="${1:?usage: MEMFAULT_PROJECT_KEY=... $0 <9P port suffix, e.g. 1303> [interval_s]}"
INT="${2:-30}"
HERE=$(cd "$(dirname "$0")" && pwd)
P9="$HERE/p9do"; [ -x "$P9" ] || cc -O2 -o "$P9" "$HERE/p9do.c"
DEV="/dev/cu.usbmodem$PORT"
SOCK="/tmp/mflt_$PORT.sock"
API="https://chunks.memfault.com/api/v0/chunks"

forward_cycle() {
	pkill -f "socat.*usbmodem$PORT" 2>/dev/null; rm -f "$SOCK"; sleep 1
	socat UNIX-LISTEN:"$SOCK",fork "$DEV",rawer & SP=$!; sleep 2

	# device serial = CGA (net/aether/addr, colons stripped) -> "dect-<12 hex>"
	addr=$("$P9" "$SOCK" rd:net/aether/addr 2>/dev/null | sed -n 's#.*=> *##p' | tr -cd '0-9a-f')
	serial="dect-${addr:-unknown}"

	n=0
	while :; do
		out=$("$P9" "$SOCK" rd:dev/mflt 2>/dev/null)
		chunks=$(printf '%s' "$out" | grep -oE 'MC:[A-Za-z0-9+/=]*:')
		[ -n "$chunks" ] || break
		while IFS= read -r line; do
			[ -n "$line" ] || continue
			b64=${line#MC:}; b64=${b64%:}
			code=$(printf '%s' "$b64" | openssl base64 -d -A 2>/dev/null | \
				curl -s -o /dev/null -w '%{http_code}' -X POST "$API/$serial" \
					-H "Memfault-Project-Key: $KEY" \
					-H "Content-Type: application/octet-stream" \
					--data-binary @-)
			case "$code" in 200|202) n=$((n + 1)) ;;
				*) echo "  chunk POST -> HTTP $code" ;; esac
		done <<-EOF
		$chunks
		EOF
	done

	kill $SP 2>/dev/null; pkill -f "socat.*usbmodem$PORT" 2>/dev/null; rm -f "$SOCK"
	echo "$(date +%T)  $serial  forwarded $n chunk(s)"
}

echo "forwarding $DEV -> Memfault every ${INT}s  (Ctrl-C to stop)"
trap 'pkill -f "socat.*usbmodem'"$PORT"'" 2>/dev/null; rm -f "$SOCK"; exit 0' INT TERM
while :; do forward_cycle; sleep "$INT"; done
