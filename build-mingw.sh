#!/usr/bin/env bash
#
# Cross-compile SadieDate.exe with MinGW-w64 (e.g. on Linux).
# Produces ONE 32-bit executable that runs on Windows XP, 7 and 10,
# on both 32-bit and 64-bit editions.
#
# Install the toolchain on Debian/Ubuntu with:
#   sudo apt-get install gcc-mingw-w64-i686
#
set -euo pipefail

CC=${CC:-i686-w64-mingw32-gcc}
WINDRES=${WINDRES:-i686-w64-mingw32-windres}

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src"
OUT="$ROOT/build"
mkdir -p "$OUT"

echo "Compiling resources..."
"$WINDRES" -I "$SRC" "$SRC/SadieDate.rc" -O coff -o "$OUT/SadieDate.res.o"

echo "Compiling and linking..."
"$CC" -municode -O2 -s \
    -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
    -o "$OUT/SadieDate.exe" \
    "$SRC/SadieDate.c" "$OUT/SadieDate.res.o"

echo "Built: $OUT/SadieDate.exe"
file "$OUT/SadieDate.exe" 2>/dev/null || true
