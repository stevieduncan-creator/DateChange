#!/usr/bin/env bash
#
# Build the date-only shim: 32-bit and 64-bit hook DLLs, launchers, and the
# ShowDate test tool. Output goes to build/ and is copied to dist/.
#
# Toolchain (Debian/Ubuntu):
#   sudo apt-get install gcc-mingw-w64
#
set -euo pipefail

CC32=${CC32:-i686-w64-mingw32-gcc}
RC32=${RC32:-i686-w64-mingw32-windres}
CC64=${CC64:-x86_64-w64-mingw32-gcc}
RC64=${RC64:-x86_64-w64-mingw32-windres}

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src"
OUT="$ROOT/build"
DIST="$ROOT/dist"
mkdir -p "$OUT" "$DIST"

# Target Windows XP and up; resolve newer APIs at runtime via GetProcAddress.
# -Wno-cast-function-type: GetProcAddress/CreateRemoteThread casts are the
# documented Win32 idiom and are intentional here.
CFLAGS_COMMON="-O2 -Wall -Wextra -Wno-cast-function-type \
 -D_WIN32_WINNT=0x0501 -DWINVER=0x0501"

echo "== Hook DLL (32-bit) =="
$CC32 $CFLAGS_COMMON -shared -s \
    -o "$OUT/SadieHook32.dll" "$SRC/SadieHook.c" \
    -Wl,--enable-stdcall-fixup

echo "== Hook DLL (64-bit) =="
$CC64 $CFLAGS_COMMON -shared -s \
    -o "$OUT/SadieHook64.dll" "$SRC/SadieHook.c"

echo "== Launcher (32-bit) -> SadieDate.exe =="
$RC32 -I "$SRC" "$SRC/SadieInject.rc" -O coff -o "$OUT/inject32.res.o"
$CC32 -municode $CFLAGS_COMMON -s \
    -o "$OUT/SadieDate.exe" "$SRC/SadieInject.c" "$OUT/inject32.res.o" \
    -lshell32

echo "== Launcher (64-bit) -> SadieDate64.exe =="
$RC64 -I "$SRC" "$SRC/SadieInject.rc" -O coff -o "$OUT/inject64.res.o"
$CC64 -municode $CFLAGS_COMMON -s \
    -o "$OUT/SadieDate64.exe" "$SRC/SadieInject.c" "$OUT/inject64.res.o" \
    -lshell32

echo "== Test tool ShowDate (32- and 64-bit) =="
$CC32 $CFLAGS_COMMON -s -o "$OUT/ShowDate.exe"   "$SRC/ShowDate.c"
$CC64 $CFLAGS_COMMON -s -o "$OUT/ShowDate64.exe" "$SRC/ShowDate.c"

echo "== Copy to dist/ =="
cp "$OUT/SadieHook32.dll" "$OUT/SadieHook64.dll" \
   "$OUT/SadieDate.exe"   "$OUT/SadieDate64.exe" \
   "$OUT/ShowDate.exe"    "$OUT/ShowDate64.exe"  "$DIST/"

echo
echo "Done. Artifacts in dist/:"
for f in SadieDate.exe SadieDate64.exe SadieHook32.dll SadieHook64.dll ShowDate.exe ShowDate64.exe; do
    printf '  %-18s %s\n' "$f" "$(file -b "$DIST/$f" 2>/dev/null | cut -d, -f1-2)"
done
