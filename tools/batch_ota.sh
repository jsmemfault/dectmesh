#!/usr/bin/env bash
# batch_ota.sh <start_patch> <end_patch>
# Run a sequence of full hands-off mesh-OTA cycles (build-if-missing -> push ->
# verify), logging a one-line verdict per version to /tmp/ota_batch_results.txt.
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
S="$1"; E="$2"
RES=/tmp/ota_batch_results.txt
for N in $(seq "$S" "$E"); do
  IMG="/tmp/ota_images/mesh_0.7.$N.signed.bin"
  if [ ! -f "$IMG" ]; then
    echo ">>> building 0.7.$N"
    bash "$DIR/build_ver.sh" "$N" >/tmp/build_$N.log 2>&1 || { echo "0.7.$N: BUILD-FAIL" | tee -a $RES; continue; }
  fi
  echo ">>> cycle 0.7.$N"
  bash "$DIR/mesh_ota_verify.sh" "$N" >/tmp/cycle_$N.log 2>&1
  RC=$?
  V=$(grep -oE "RESULT 0\.7\.$N: [A-Z]+" /tmp/cycle_$N.log | tail -1)
  STREAM=$(grep -oE "streamed [0-9]+ B in [0-9]+ ms \([0-9]+ retries, [0-9]+ pass[es]*\)" /tmp/cycle_$N.log | tail -1)
  echo "0.7.$N: ${V:-NO-VERDICT} rc=$RC | ${STREAM:-no-stream}" | tee -a $RES
done
echo ">>> batch done"; echo "=== SUMMARY ==="; cat $RES
