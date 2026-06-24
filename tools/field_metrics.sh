#!/usr/bin/env bash
#
# field_metrics.sh -- sample a tethered field node's mesh + PHY state on a timer
# and append timestamped rows to a CSV, for real-world field-test analysis.
#
# Drives the node's 9151 CONSOLE/shell (the *01 USB-CDC port) over serial: each
# tick it issues `aether status` + `dect stats`, parses the key numbers, and logs
# one CSV row. Run it on the tethered desk node (node 1) during a field test; pair
# the CSV's timestamps with your position / WiFi-AP logs to correlate link quality
# and delivery with real geographic distance.
#
#   ./field_metrics.sh [console-port] [interval_s] [out.csv]
#
# Defaults: first /dev/cu.usbmodem*1 (4-digit, the 9151 console), 10 s, field_metrics.csv
#
# Captured per row:
#   ts_iso, uptime_ms, addr, rank, neighbors, nbr_rssi_min, nbr_rssi_max,
#   routes, max_hops, hello_sent, hello_recv, fwd, drop, rx_pkts, tx_pkts,
#   dect_tx_ok, dect_tx_busy, dect_tx_err, dect_last_rssi, dect_last_snr
#
# Note: macOS has no `timeout`; this uses a perl alarm guard per sample so a
# wedged read can't hang the logger. Ctrl-C to stop.
set -u

PORT="${1:-$(ls /dev/cu.usbmodem*1 2>/dev/null | grep -E 'usbmodem[0-9]{4}$' | head -1)}"
[ -n "$PORT" ] || { echo "no 9151 console port (/dev/cu.usbmodem*1) -- plug the node in"; exit 1; }
INTERVAL="${2:-10}"
OUT="${3:-field_metrics.csv}"

HDR="ts_iso,uptime_ms,addr,rank,neighbors,nbr_rssi_min,nbr_rssi_max,routes,max_hops,hello_sent,hello_recv,fwd,drop,rx_pkts,tx_pkts,dect_tx_ok,dect_tx_busy,dect_tx_err,dect_last_rssi,dect_last_snr"
[ -f "$OUT" ] || echo "$HDR" > "$OUT"

echo "logging $PORT every ${INTERVAL}s -> $OUT  (Ctrl-C to stop)"
echo "$HDR"

# one sample: open console on its own fd, query, parse, echo a CSV row
sample() {
	local raw clean
	exec 3<>"$PORT" 2>/dev/null || { echo "open failed"; return 1; }
	stty 115200 cs8 -cstopb -parenb -echo -ixon clocal -hupcl <&3 2>/dev/null
	: > /tmp/.fm_raw
	perl -e 'alarm 8; exec @ARGV' cat <&3 > /tmp/.fm_raw 2>/dev/null &
	local rp=$!
	sleep 0.4
	printf '\r' >&3; sleep 0.3
	printf 'aether status\r' >&3; sleep 1.6
	printf 'dect stats\r' >&3; sleep 1.4
	kill $rp 2>/dev/null
	exec 3>&- 2>/dev/null

	clean=$(LC_ALL=C tr -cd '[:print:]\n\r\t' < /tmp/.fm_raw | tr -s '\r' '\n' \
		| sed -E 's/\x1b\[[0-9;]*m//g; s/\[[0-9]+D//g; s/\[J//g')

	# scalars (grep the labelled lines; default to empty on miss)
	local addr rank nbrs routes maxhops hs hr fwd drop rxp txp tok tbz ter lrssi lsnr
	addr=$(echo "$clean"   | sed -nE 's/.*addr +([0-9a-f:]+).*/\1/p' | head -1)
	# rank comes from `aether tree`, not status; approximate from neighbor/route topology instead
	nbrs=$(echo "$clean"   | sed -nE 's/.*neighbors +([0-9]+).*/\1/p' | head -1)
	routes=$(echo "$clean" | sed -nE 's/.*routes +([0-9]+).*/\1/p' | head -1)
	hs=$(echo "$clean"     | sed -nE 's/.*hello +sent +([0-9]+).*/\1/p' | head -1)
	hr=$(echo "$clean"     | sed -nE 's/.*hello .*recv +([0-9]+).*/\1/p' | head -1)
	fwd=$(echo "$clean"    | sed -nE 's/.*fwd +([0-9]+).*/\1/p' | head -1)
	drop=$(echo "$clean"   | sed -nE 's/.*fwd +[0-9]+,? *drop +([0-9]+).*/\1/p' | head -1)
	rxp=$(echo "$clean"    | sed -nE 's#.*rx/tx +([0-9]+)/[0-9]+ pkts.*#\1#p' | head -1)
	txp=$(echo "$clean"    | sed -nE 's#.*rx/tx +[0-9]+/([0-9]+) pkts.*#\1#p' | head -1)
	# neighbor RSSI range (lines like "  00:00:00:00:20:00  rssi -50  4s ago")
	local rssis
	rssis=$(echo "$clean"  | sed -nE 's/.*rssi +(-?[0-9]+).*/\1/p')
	local rmin rmax
	rmin=$(echo "$rssis" | sort -n | head -1)
	rmax=$(echo "$rssis" | sort -n | tail -1)
	maxhops=$(echo "$clean" | sed -nE 's/.*hops +([0-9]+).*/\1/p' | sort -n | tail -1)
	# dect stats
	tok=$(echo "$clean"  | sed -nE 's/.*tx +([0-9]+) ok.*/\1/p' | head -1)
	tbz=$(echo "$clean"  | sed -nE 's/.*tx +[0-9]+ ok, +([0-9]+) busy.*/\1/p' | head -1)
	ter=$(echo "$clean"  | sed -nE 's/.*busy \(LBT\), +([0-9]+) err.*/\1/p' | head -1)
	lrssi=$(echo "$clean"| sed -nE 's/.*last rssi +(-?[0-9]+) dBm.*/\1/p' | head -1)
	lsnr=$(echo "$clean" | sed -nE 's/.*snr +(-?[0-9]+) dB.*/\1/p' | head -1)

	local ts; ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
	echo "${ts},,${addr},,${nbrs},${rmin},${rmax},${routes},${maxhops},${hs},${hr},${fwd},${drop},${rxp},${txp},${tok},${tbz},${ter},${lrssi},${lsnr}"
}

trap 'echo; echo "stopped -> $OUT"; exit 0' INT
while :; do
	row=$(sample) || { sleep "$INTERVAL"; continue; }
	echo "$row" | tee -a "$OUT"
	sleep "$INTERVAL"
done
