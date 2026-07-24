#!/bin/sh
# aether_prove.sh -- challenge a DECTstrous node to prove it owns its CGA.
#
# Reads the node's claimed address, sends a fresh random challenge to
# net/aether/prove, reads back {pubkey, signature}, and verifies -- with no
# trust in the node -- that SHA256(pubkey)[:6] == the address AND the signature
# over our challenge is valid (aether_verify, OpenSSL). See dect_mesh/src/cga.c.
#
# Usage: aether_prove.sh <usb-cdc 9P port suffix, e.g. 1303>
set -e
PORT=${1:?usage: aether_prove.sh <9P port suffix, e.g. 1303 for Node B, 1203 for Node A>}
HERE=$(cd "$(dirname "$0")" && pwd)
DEV=/dev/cu.usbmodem$PORT
SOCK=/tmp/prove_$PORT.sock

pkill -f "socat.*usbmodem$PORT" 2>/dev/null || true
rm -f "$SOCK"; sleep 1
socat UNIX-LISTEN:"$SOCK",fork "$DEV",rawer & SP=$!
sleep 3

# fresh, caller-chosen challenge (ASCII hex) so the proof can't be a replay
CHAL=$(openssl rand -hex 16)

# one 9P session (no DTR churn): read addr, write challenge, read the proof
OUT=$("$HERE/p9do" "$SOCK" rd:net/aether/addr "ws:net/aether/prove:$CHAL" rd:net/aether/prove 2>/dev/null || true)

kill $SP 2>/dev/null || true
pkill -f "socat.*usbmodem$PORT" 2>/dev/null || true
rm -f "$SOCK"

ADDR=$(printf '%s\n' "$OUT" | sed -n 's#.*net/aether/addr => *\([0-9a-fA-F:]*\).*#\1#p' | head -1)
PROOF=$(printf '%s\n' "$OUT" | sed -n 's#.*net/aether/prove => *\(.*\)#\1#p' | tail -1)
PUB=$(printf '%s\n' "$PROOF" | awk '{print $1}')
SIG=$(printf '%s\n' "$PROOF" | awk '{print $2}')

echo "node ($DEV) claims address : $ADDR"
echo "challenge sent             : $CHAL"
echo "pubkey  (read from node)   : $PUB"
echo "signature (over challenge) : $SIG"
echo "------------------------------------------------------------"
exec "$HERE/aether_verify" "$ADDR" "$CHAL" "$PUB" "$SIG"
