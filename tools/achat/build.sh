#!/bin/sh
# Build achat-core. serial.c uses system headers (plain cc); achat_core.c uses
# plan9port headers (9c). Link with lib9pclient + libthread + lib9 via 9l.
set -e
: "${PLAN9:=$HOME/src/plan9port}"
export PLAN9
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

cc -c -o serial.o serial.c
"$PLAN9/bin/9c" -o achat_core.o achat_core.c
"$PLAN9/bin/9l" -o achat-core achat_core.o serial.o -l9pclient -lthread -l9

echo "built $here/achat-core"
