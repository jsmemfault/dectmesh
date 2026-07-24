#!/bin/sh
# mflt_upload_symbols.sh -- upload an ELF symbol file to Memfault so coredumps
# and traces symbolicate (real function names + line numbers, not raw addresses).
#
# Needs a Memfault Organization Auth Token (OAT) -- a powerful org-wide token, so
# pass it via the environment and NEVER commit it. Run once per software
# type+version whenever you ship a new build.
#
# Usage:
#   MEMFAULT_ORG_TOKEN=<oat> tools/mflt_upload_symbols.sh \
#       <org-slug> <project-slug> <software-type> <version> <path/to/zephyr.elf>
set -u
OAT="${MEMFAULT_ORG_TOKEN:?set MEMFAULT_ORG_TOKEN=<Memfault organization auth token>}"
ORG="${1:?org slug}"; PROJ="${2:?project slug}"; TYPE="${3:?software_type}"
VER="${4:?version}"; ELF="${5:?path to .elf}"
[ -f "$ELF" ] || { echo "no such ELF: $ELF"; exit 1; }
API="https://api.memfault.com/api/v0/organizations/$ORG/projects/$PROJ"

echo "uploading $(basename "$ELF")  ->  type=$TYPE  version=$VER"

# 1. ask Memfault for a presigned upload slot
resp=$(curl -s -u ":$OAT" -X POST "$API/upload")
url=$(printf '%s' "$resp" | sed -n 's/.*"upload_url":"\([^"]*\)".*/\1/p')
tok=$(printf '%s' "$resp" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
[ -n "$url" ] && [ -n "$tok" ] || { echo "  prepare-upload failed: $resp"; exit 1; }

# 2. PUT the ELF to the presigned URL (no auth -- the URL is signed)
put=$(curl -s -X PUT "$url" -H "Content-Type: application/octet-stream" \
	--data-binary @"$ELF" -o /dev/null -w '%{http_code}')
echo "  upload PUT -> HTTP $put"
[ "$put" = 200 ] || { echo "  upload failed"; exit 1; }

# 3. associate the upload as the symbol file for this software type+version
assoc=$(curl -s -u ":$OAT" -X POST "$API/symbols" \
	-H "Content-Type: application/json" \
	-d "{\"file\":{\"token\":\"$tok\"},\"software_version\":{\"version\":\"$VER\",\"software_type\":\"$TYPE\"}}")
case "$assoc" in
	*'"error"'*) echo "  associate FAILED: $assoc"; exit 1 ;;
	*) echo "  symbols associated OK" ;;
esac
