#!/usr/bin/env bash
#
# build_ver.sh <patchlevel>
# Bump dect_mesh to 0.7.<patchlevel> (VERSION + MCUboot SIGN_VERSION), incremental
# rebuild the thingy image, and stage it at /tmp/ota_images/mesh_0.7.<patch>.signed.bin.
set -eu
P="$1"
cd /Users/jrsharp/src/dect
# VERSION file
perl -0pi -e "s/^PATCHLEVEL = .*/PATCHLEVEL = $P/m" dect_mesh/VERSION
# MCUboot sign version (what MCUboot verifies on swap)
perl -0pi -e "s/CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION=\"0\.7\.[0-9]+\+0\"/CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION=\"0.7.$P+0\"/" dect_mesh/prj.conf
echo "== building 0.7.$P =="
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- bash -c '
  export ZEPHYR_BASE=/opt/nordic/ncs/v3.3.0/zephyr
  cd /Users/jrsharp/src/dect/dect_mesh
  west build -b thingy91x/nrf9151/ns --build-dir build_thingy --sysbuild -- \
    -DEXTRA_CONF_FILE=overlay-915.conf \
    -DZEPHYR_EXTRA_MODULES="/Users/jrsharp/src/aephyr;/Users/jrsharp/src/9p4z"
' 2>&1 | tail -4
BIN=dect_mesh/build_thingy/dect_mesh/zephyr/zephyr.signed.bin
mkdir -p /tmp/ota_images
cp "$BIN" "/tmp/ota_images/mesh_0.7.$P.signed.bin"
echo "== staged /tmp/ota_images/mesh_0.7.$P.signed.bin ($(wc -c <"$BIN") B) =="
