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
# Mobile mesh nodes are AUTO-DISCOVERED from the gateway's neighbor table by
# default, so whichever node you leave tethered becomes the gateway and forwards
# the rest -- it doesn't matter which node you walk. Set MESH_PEERS to pin an
# explicit list instead.
#
# Usage:
#   MEMFAULT_PROJECT_KEY=<key> tools/mflt_forward.sh <gateway 9P port> [interval_s]
#   # explicit peer list (overrides auto-discovery), space-separated CGAs:
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
	pkill -9 -f "socat.*usbmodem$PORT" 2>/dev/null; rm -f "$SOCK"; sleep 1
	socat UNIX-LISTEN:"$SOCK",fork "$DEV",rawer & SP=$!; sleep 2
}
drop_socat() {
	kill -9 $SP 2>/dev/null; pkill -9 -f "socat.*usbmodem$PORT" 2>/dev/null; rm -f "$SOCK"; sleep 2
}

# Read a mesh peer's dev/mflt by tunneling 9P through the gateway to the peer's
# mesh 9P server (which serves the SAME dev/mflt). Prints the peer's DEV:/MC:
# stream, or nothing if the node is unreachable this cycle (out of range -> just
# retried next interval). Brings its own socat so it never races the direct read.
read_peer() {  # <peer-cga> -> peer's DEV:/MC: text on stdout (BOUNDED time)
	[ -x "$AC" ] || return 0
	psock="/tmp/mflt_mesh_$(printf '%s' "$1" | tr -d :).sock"
	rm -f "$psock" "$psock.out"
	bring_socat
	"$AC" "$SOCK" --bridge "$1" "$psock" >/dev/null 2>&1 &
	bpid=$!
	# Bounded wait for the bridge endpoint; bail early if aether_conv already exited
	# (connect failed = peer unreachable / out of range this cycle).
	i=0
	while [ ! -S "$psock" ] && kill -0 $bpid 2>/dev/null && [ $i -lt 8 ]; do
		sleep 1; i=$((i + 1))
	done
	if [ -S "$psock" ]; then
		# The mesh round-trip can STALL if the peer roams out of range mid-read, so
		# cap the read with a watchdog instead of letting it block the whole cycle
		# (and the gateway's own telemetry behind it).
		"$P9" "$psock" rd:dev/mflt >"$psock.out" 2>/dev/null &
		rpid=$!
		j=0
		while kill -0 $rpid 2>/dev/null && [ $j -lt 10 ]; do sleep 1; j=$((j + 1)); done
		kill -9 $rpid 2>/dev/null; wait $rpid 2>/dev/null
		cat "$psock.out" 2>/dev/null
	fi
	kill -9 $bpid 2>/dev/null; wait $bpid 2>/dev/null; rm -f "$psock" "$psock.out"
	drop_socat
}

# POST every DEV:/MC: chunk in a text blob and record a per-device tally in the
# cycle-global $TALLY. Called in the main shell (not a subshell) so $TALLY sticks.
TALLY=""
post_tokens() {  # <text>
	serial=""
	for tok in $(printf '%s' "$1" | grep -oE 'DEV:[^:]*:|MC:[A-Za-z0-9+/=]+:'); do
		case "$tok" in
		DEV:*) serial=${tok#DEV:}; serial=${serial%:} ;;
		MC:*)
			[ -n "$serial" ] || continue
			b64=${tok#MC:}; b64=${b64%:}
			code=$(post_chunk "$serial" "$b64")
			case "$code" in
				200|202) TALLY="$TALLY $serial" ;;
				*) echo "  [$serial] chunk POST HTTP $code" ;;
			esac
			;;
		esac
	done
}

forward_cycle() {
	TALLY=""
	# Gateway's own two chips + its live neighbor table, one session (a read drains
	# all pending chunks in a single 8K read; the neighbor read is harmless). POST
	# the gateway's chunks IMMEDIATELY -- its telemetry (the self-heal/reform data)
	# must never wait behind a roaming peer's mesh round-trip.
	#
	# HELD-socat mode ($HELD=1, used when relay is off): reuse one long-lived socat
	# for the whole run instead of cycling it every cycle -- repeated open/close of
	# the CDC port is exactly what wedges it. Bridges still need their own socat.
	[ "$HELD" = 1 ] || bring_socat
	own=$("$P9" "$SOCK" rd:dev/mflt5340 rd:dev/mflt9151 rd:dev/aether/neighbors 2>/dev/null)
	[ "$HELD" = 1 ] || drop_socat
	post_tokens "$own"

	# Which mobile nodes to relay: an explicit MESH_PEERS list, or -- by default --
	# AUTO-DISCOVERED from the gateway's own neighbor table (every node it can hear,
	# by durable CGA, minus the null self-entry). So whichever node you leave
	# tethered becomes the gateway and forwards the rest -- it genuinely doesn't
	# matter which node you walk. (1-hop discovery: a peer that roams multi-hop is
	# relayed only while it's also a direct neighbor.)
	if [ "$PEERS" = "off" ] || [ "$PEERS" = "none" ]; then
		peers=""                       # relay disabled: gateway's own telemetry only
	elif [ -n "$PEERS" ]; then
		peers="$PEERS"                 # explicit pinned list
	else
		peers=$(printf '%s' "$own" | grep -oE 'identity [0-9a-fA-F:]{17}' | \
			awk '{print $2}' | grep -viE '^00:00:00:00:00:00$' | sort -u)
	fi

	echo "  discovered mesh peers: ${peers:-none}" | tr '\n' ' '; echo
	# Each mesh-relayed mobile node, bounded time, POSTed as it arrives. read_peer
	# runs in the MAIN shell (output to a file, NOT $(...)) so its background-process
	# cleanup -- killing the bridge + socat -- actually takes effect; inside a
	# command-substitution subshell those kills were lost and processes piled up on
	# the port.
	for peer in $peers; do
		read_peer "$peer" >"$SOCK.peer" 2>/dev/null
		post_tokens "$(cat "$SOCK.peer" 2>/dev/null)"
	done
	rm -f "$SOCK.peer"

	tally=$(printf '%s' "$TALLY" | tr ' ' '\n' | grep . | sort | uniq -c | \
		awk '{printf " %sx%s", $1, $2}')
	echo "$(date +%T)  forwarded:${tally:- nothing}"
}

if [ "$INT" = "once" ]; then
	echo "one-shot: $FILES via $DEV -> Memfault (peers: ${PEERS:-auto-discovered})"
else
	echo "forwarding $FILES via $DEV -> Memfault every ${INT}s"
	echo "mesh peers: ${PEERS:-auto-discovered from the gateway neighbor table each cycle}  (Ctrl-C to stop)"
fi
trap 'pkill -f "socat.*usbmodem'"$PORT"'" 2>/dev/null; pkill -f "aether_conv.*--bridge" 2>/dev/null; rm -f "$SOCK"; exit 0' INT TERM

HELD=0   # held-socat mode (loop + relay-off only); one-shot never holds

# ONE-SHOT ($2 = "once"): run a single collection cycle and exit. This is the
# robust way to run periodically -- let a scheduler (launchd/cron) invoke it, so
# every run starts from a clean process with no long-lived state to accumulate
# socat/bridge leaks. The continuous loop below is a convenience for interactive
# watching only.
if [ "$INT" = "once" ]; then
	forward_cycle
	pkill -9 -f "socat.*usbmodem$PORT" 2>/dev/null; pkill -9 -f "aether_conv.*--bridge" 2>/dev/null
	rm -f "$SOCK" "$SOCK.peer"
	exit 0
fi

# Relay off -> no per-cycle bridge contention, so hold ONE socat for the whole run
# (gentle on the CDC port). With peers, each phase needs its own socat (bridges).
if [ "$PEERS" = "off" ] || [ "$PEERS" = "none" ]; then HELD=1; bring_socat; fi

while :; do forward_cycle; sleep "$INT"; done
