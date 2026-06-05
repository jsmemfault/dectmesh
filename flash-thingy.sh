#!/usr/bin/env bash
#
# flash-thingy.sh — bring up a DECTstrous node over J-Link (SWD).
#
# Flashes the APP firmware onto one chip of a Thingy:91 X. After this one-time
# J-Link bring-up, the node self-updates over USB (/dev/fw5340) and is OTA-able
# over 9P (/dev/fw9151) — no debugger needed again.
#
# Usage:
#   ./flash-thingy.sh <jlink-serial> relay [--recover]   # nRF5340  (set SW2 -> nRF53)
#   ./flash-thingy.sh <jlink-serial> mesh                # nRF9151  (set SW2 -> nRF91)
#
# SW2 is under the Thingy's front cover and selects the SWD target. A 10-pin SWD
# cable goes DK "DEBUG OUT" -> Thingy P8 (the DK doubles as the J-Link probe).
#
# !! NEVER recover the nRF9151 — it ERASES the licensed DECT NR+ modem firmware.
#    --recover is ONLY for a factory-fresh / readback-protected nRF5340 (no modem
#    there). This script refuses to recover the 91. The DECT modem FW itself is a
#    separate, deliberate one-time step (see notes in thingy91x-dect-flashing).
#
set -euo pipefail

DECT="$(cd "$(dirname "$0")" && pwd)"
RELAY_APP="$DECT/dect_relay/build/merged.hex"
RELAY_NET="$DECT/dect_relay/build/merged_CPUNET.hex"
MESH_APP="$DECT/dect_mesh/build_thingy/merged.hex"

usage() { echo "usage: $0 <jlink-serial> relay|mesh [--recover]" >&2; exit 1; }
SN="${1:-}"; TGT="${2:-}"; RECOVER="${3:-}"
[ -n "$SN" ] && [ -n "$TGT" ] || usage

np() { nrfutil device "$@" --serial-number "$SN" --traits jlink; }

case "$TGT" in
  relay|53|nrf53)
    [ -f "$RELAY_APP" ] || { echo "missing $RELAY_APP — build the relay first" >&2; exit 1; }
    echo ">>> nRF5340 RELAY  (set SW2 -> nRF53)  J-Link $SN"
    if [ "$RECOVER" = "--recover" ]; then
      echo "    recover (fresh/protected unit — safe, no modem on the 53): both cores"
      np recover --x-family nrf53 --core Network
      np recover --x-family nrf53 --core Application
    fi
    np program --x-family nrf53 --core Network     --firmware "$RELAY_NET"
    np program --x-family nrf53 --core Application  --firmware "$RELAY_APP"
    np reset   --x-family nrf53
    echo ">>> Relay up. From now on it self-DFUs over USB: 9p write /dev/fw5340 < zephyr.signed.bin"
    ;;
  mesh|91|nrf91)
    [ "$RECOVER" = "--recover" ] && { echo "REFUSED: never recover the nRF9151 (wipes the licensed DECT modem FW)." >&2; exit 1; }
    [ -f "$MESH_APP" ] || { echo "missing $MESH_APP — build the mesh first" >&2; exit 1; }
    echo ">>> nRF9151 MESH app  (set SW2 -> nRF91)  J-Link $SN  [program only; modem FW preserved]"
    np program --x-family nrf91 --firmware "$MESH_APP"
    np reset   --x-family nrf91
    echo ">>> Mesh app up. From now on it's OTA-able over 9P: 9p write /dev/fw9151 < zephyr.signed.bin"
    ;;
  *) usage;;
esac
