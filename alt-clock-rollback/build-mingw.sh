#!/usr/bin/env bash
#
# Build the ALTERNATIVE clock-rollback launcher (no injection).
# See alt-clock-rollback/README.md. Most users want the main shim instead.
#
set -euo pipefail
CC=${CC:-i686-w64-mingw32-gcc}
WINDRES=${WINDRES:-i686-w64-mingw32-windres}
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/build"
mkdir -p "$OUT"
echo "Compiling resources..."
"$WINDRES" -I "$DIR" "$DIR/SadieDate.rc" -O coff -o "$OUT/SadieDate.res.o"
echo "Compiling and linking..."
"$CC" -municode -O2 -s -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
    -o "$OUT/SadieDate-clock.exe" \
    "$DIR/SadieDate.c" "$OUT/SadieDate.res.o"
echo "Built: $OUT/SadieDate-clock.exe"
file "$OUT/SadieDate-clock.exe" 2>/dev/null || true
