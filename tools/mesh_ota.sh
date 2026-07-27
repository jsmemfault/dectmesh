#!/bin/sh
# mesh_ota.sh <gateway-9p-port> <peer_eui12> <signed.bin> [chunk]
#
# Push a signed firmware image to a REMOTE mesh peer, over the air, through the
# gateway -- no wires to the target node. The gateway's 9151 runs the actual mesh
# 9P write session to the peer (aether_net_push_*); the host just drives it:
#   target -> stream image -> commit -> reboot -> confirm.
#
# Requires: dect_mesh >= 0.7.55 on the gateway 9151, dect_relay >= 0.38.31.
set -e
PORT="$1"; EUI="$2"; IMG="$3"; CHUNK="${4:-1024}"
DIR=$(cd "$(dirname "$0")" && pwd)
SOCK=/tmp/mesh_ota.sock

[ -n "$PORT" ] && [ -n "$EUI" ] && [ -f "$IMG" ] || {
	echo "usage: $0 <gateway-port> <peer_eui12> <signed.bin> [chunk]"; exit 2; }

up()   { pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null || true; sleep 1; rm -f "$SOCK"
	 socat UNIX-LISTEN:"$SOCK",fork "$PORT",rawer & sleep 2; }
down() { pkill -9 -f "socat.*mesh_ota.sock" 2>/dev/null || true; rm -f "$SOCK"; }

echo "== mesh-OTA $IMG ($(wc -c <"$IMG") B) -> peer $EUI via $PORT =="
up
echo "[1/6] target $EUI (open mesh DFU session to peer's dev/firmware)"
"$DIR/p9do" "$SOCK" "ws:dev/push_ctl:target $EUI"
echo "[2/6] stream image (each chunk -> mesh Twrite to the peer)..."
"$DIR/aether_conv" "$SOCK" --put dev/push "$IMG" "$CHUNK" | tail -1
echo "[3/6] commit (clunk peer fid -> peer validates + requests upgrade)"
"$DIR/p9do" "$SOCK" ws:dev/push_ctl:commit
echo "[4/6] reboot $EUI (peer swaps to the new image)"
"$DIR/p9do" "$SOCK" "ws:dev/push_ctl:reboot $EUI"
down
echo "[5/6] wait 70s for the peer to boot the new image..."
sleep 70
up
echo "[6/6] confirm $EUI (mark the new image good so it persists)"
"$DIR/p9do" "$SOCK" "ws:dev/push_ctl:confirm $EUI"
down
echo "== done: peer $EUI OTA'd over the mesh =="
