#!/bin/sh
# field_monitor.sh -- unattended field-test telemetry monitor.
#
# Runs the one-shot forwarder every INTERVAL seconds, each invocation a fresh
# clean process (no accumulating socat/bridge state), logging a timestamped
# per-device tally. This is the robust way to run hands-off: a bad cycle fails
# fast (the forwarder's watchdogs bound every read) and the next run starts
# pristine. Whichever mobile nodes are in mesh range of the gateway that cycle
# get their dev/mflt drained over the air and POSTed; out-of-range nodes simply
# drop from the tally and resume when they're back.
#
#   MEMFAULT_PROJECT_KEY=<key> tools/field_monitor.sh <gateway 9P port> [interval_s]
set -u
KEY="${MEMFAULT_PROJECT_KEY:?set MEMFAULT_PROJECT_KEY}"
PORT="${1:?usage: MEMFAULT_PROJECT_KEY=... $0 <gateway port, e.g. 1203> [interval_s]}"
INT="${2:-45}"
HERE=$(cd "$(dirname "$0")" && pwd)
LOG="${MONITOR_LOG:-/tmp/field_monitor.log}"
BOOT_WAIT="${BOOT_WAIT:-45}"

echo "$(date '+%F %T')  field monitor up: gateway $PORT, one-shot every ${INT}s" | tee -a "$LOG"
sleep "$BOOT_WAIT"   # let a freshly power-cycled gateway boot + settle its uart link

while :; do
	out=$(MEMFAULT_PROJECT_KEY="$KEY" "$HERE/mflt_forward.sh" "$PORT" once 2>/dev/null)
	peers=$(printf '%s' "$out" | sed -n 's/.*discovered mesh peers: */peers=/p')
	tally=$(printf '%s' "$out" | sed -n 's/^[0-9:]* *forwarded:/forwarded:/p')
	echo "$(date '+%T')  ${tally:-forwarded: (no read)}   [${peers:-peers=?}]" >> "$LOG"
	sleep "$INT"
done
