#!/bin/sh
# mflt_forward.sh -- forward Memfault chunks to the cloud from the gateway's
# composed 9P namespace, over a single connection. Handles both the gateway's
# own two chips AND mobile mesh nodes whose telemetry is relayed over the air.
#
# The relay multiplexes two local chunk streams into one namespace:
#   dev/mflt5340  -- the relay's own chunks (coredumps, reboot reasons, metrics)
#   dev/mflt9151  -- the gateway 9151's chunks, proxied over the inter-chip link
#
# MESH-RELAYED TELEMETRY (the field-test headline): a mobile node has no host of
# its own -- battery, no USB, no LTE. Its telemetry rides the DECT mesh to the
# gateway: aether_conv tunnels a 9P session through the gateway's conversation
# layer to the peer's mesh 9P server (which serves the SAME dev/mflt), so this
# host can read a quarter-mile-away node's dev/mflt as if it were local. List a
# peer's CGA in MESH_PEERS and its chunks are drained + POSTed every cycle too.
#
# Every stream is self-describing: it emits a "DEV:<serial>:" line before its
# "MC:<base64>:" chunks, so one generic loop forwards any number of chips/nodes
# to the right Memfault device without being told who they are. Peers are
# addressed by durable CGA, so the mesh re-resolves routing as a node roams.
#
# Usage:
#   MEMFAULT_PROJECT_KEY=<key> tools/mflt_forward.sh <gateway 9P port> [interval_s]
#   # add mesh-relayed mobile nodes by their CGA (space-separated):
#   MESH_PEERS="36:0a:45:63:c7:17" MEMFAULT_PROJECT_KEY=<key> \
#       tools/mflt_forward.sh 1203 15
set -u
KEY="${MEMFAULT_PROJECT_KEY:?set MEMFAULT_PROJECT_KEY=<your Memfault project key>}"
PORT="${1:?usage: MEMFAULT_PROJECT_KEY=... $0 <gateway 9P port suffix, e.g. 1203> [interval_s]}"
INT="${2:-30}"
PEERS="${MESH_PEERS:-}"          # CGAs of mobile nodes reached over the mesh
FILES="dev/mflt5340 dev/mflt9151"
HERE=$(cd "$(dirname "$0")" && pwd)
P9="$HERE/p9do"; [ -x "$P9" ] || cc -O2 -o "$P9" "$HERE/p9do.c"
AC="$HERE/aether_conv"
DEV="/dev/cu.usbmodem$PORT"
SOCK="/tmp/mflt_$PORT.sock"
API="https://chunks.memfault.com/api/v0/chunks"

post_chunk() {  # <device-serial> <base64-chunk> -> prints HTTP status
	printf '%s' "$2" | openssl base64 -d -A 2>/dev/null | \
		curl -s -o /dev/null -w '%{http_code}' -X POST "$API/$1" \
			-H "Memfault-Project-Key: $KEY" \
			-H "Content-Type: application/octet-stream" --data-binary @-
}

# The gateway's single USB 9P port serves ONE consumer at a time, and reconnecting
# a fresh session right after another wedges the relay's DTR-gated session pool.
# So each read phase gets its OWN short-lived socat with a settle after teardown
# (the discipline that makes OTA reliable). Cheap at a 12-15 s cadence.
bring_socat() {
	pkill -f "socat.*usbmodem$PORT" 2>/dev/null; rm -f "$SOCK"; sleep 1
	socat UNIX-LISTEN:"$SOCK",fork "$DEV",rawer & SP=$!; sleep 2
}
drop_socat() {
	kill $SP 2>/dev/null; pkill -f "socat.*usbmodem$PORT" 2>/dev/null; rm -f "$SOCK"; sleep 2
}

# Read a mesh peer's dev/mflt by tunneling 9P through the gateway to the peer's
# mesh 9P server (which serves the SAME dev/mflt). Prints the peer's DEV:/MC:
# stream, or nothing if the node is unreachable this cycle (out of range -> just
# retried next interval). Brings its own socat so it never races the direct read.
read_peer() {  # <peer-cga> -> peer's DEV:/MC: text on stdout
	[ -x "$AC" ] || return 0
	psock="/tmp/mflt_mesh_$(printf '%s' "$1" | tr -d :).sock"
	rm -f "$psock"
	bring_socat
	"$AC" "$SOCK" --bridge "$1" "$psock" >/dev/null 2>&1 &
	bpid=$!
	# Wait (bounded) for the mesh conversation to come up and the endpoint to appear.
	i=0
	while [ ! -S "$psock" ] && [ $i -lt 12 ]; do sleep 1; i=$((i + 1)); done
	[ -S "$psock" ] && "$P9" "$psock" rd:dev/mflt 2>/dev/null
	kill $bpid 2>/dev/null; wait $bpid 2>/dev/null; rm -f "$psock"
	drop_socat
}

forward_cycle() {
	# Gateway's own two chips: one session, both files (a read drains all pending
	# chunks in a single 8K read).
	bring_socat
	out=$("$P9" "$SOCK" rd:dev/mflt5340 rd:dev/mflt9151 2>/dev/null)
	drop_socat

	# Then each mesh-relayed mobile node, each in its own session.
	for peer in $PEERS; do
		pout=$(read_peer "$peer")
		[ -n "$pout" ] && out="$out
$pout"
	done

	# Walk the DEV:/MC: tokens in order: each DEV: sets the device the following
	# MC: chunks belong to (self-describing streams -> one generic loop). Track a
	# per-device tally so a mesh peer's flow is visible in the summary.
	serial=""; total=0; summary=""
	for tok in $(printf '%s' "$out" | grep -oE 'DEV:[^:]*:|MC:[A-Za-z0-9+/=]+:'); do
		case "$tok" in
		DEV:*) serial=${tok#DEV:}; serial=${serial%:} ;;
		MC:*)
			[ -n "$serial" ] || continue
			b64=${tok#MC:}; b64=${b64%:}
			code=$(post_chunk "$serial" "$b64")
			case "$code" in
				200|202) total=$((total + 1)); summary="$summary $serial" ;;
				*) echo "  [$serial] chunk POST HTTP $code" ;;
			esac
			;;
		esac
	done
	tally=$(printf '%s' "$summary" | tr ' ' '\n' | grep . | sort | uniq -c | \
		awk '{printf " %sx%s", $1, $2}')
	echo "$(date +%T)  forwarded $total chunk(s):${tally:- none}"
}

echo "forwarding $FILES${PEERS:+ + mesh peers [$PEERS]} via $DEV -> Memfault every ${INT}s  (Ctrl-C to stop)"
trap 'pkill -f "socat.*usbmodem'"$PORT"'" 2>/dev/null; pkill -f "aether_conv.*--bridge" 2>/dev/null; rm -f "$SOCK"; exit 0' INT TERM
while :; do forward_cycle; sleep "$INT"; done
