#!/usr/bin/env bash
# Cross-compiles pfind for Windows with MinGW and runs the unit tests under Wine.
# Catches Windows-only compile/link errors and path/Unicode problems without a Windows machine.
# It does NOT replace the MSVC build in CI (MSVC has its own warnings).
#
# Needs (Debian/Ubuntu): sudo apt install g++-mingw-w64-x86-64-posix wine64
# Usage: tools/cross-test-windows.sh
set -euo pipefail

cd "$(dirname "$0")/.."

CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}
OUT=${OUT:-build-windows}
VERSION=$(sed -n 's/^project(pfind VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)

mkdir -p "$OUT"
FLAGS=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -DNOMINMAX -DUNICODE -D_UNICODE
       "-DPFIND_VERSION=\"${VERSION}\"" -Iinclude -Isrc)

objects=()
for source in src/*.cpp tests/tests.cpp; do
    object="$OUT/$(basename "${source%.cpp}").o"
    "$CXX" "${FLAGS[@]}" -c "$source" -o "$object"
    objects+=("$object")
done

core=(); for o in "${objects[@]}"; do case "$o" in */main.o|*/tests.o) ;; *) core+=("$o") ;; esac; done
"$CXX" -static -o "$OUT/pfind.exe" "$OUT/main.o" "${core[@]}" -lshell32 -pthread
"$CXX" -static -o "$OUT/pfind_tests.exe" "$OUT/tests.o" "${core[@]}" -pthread
echo "built $OUT/pfind.exe and $OUT/pfind_tests.exe"

# Wine converts Unicode file names through the Unix locale: it must be UTF-8.
export WINEDEBUG=-all LANG=C.UTF-8 LC_ALL=C.UTF-8
"$WINE" "$OUT/pfind_tests.exe" | tr -d '\r'
"$WINE" "$OUT/pfind.exe" --version | tr -d '\r'
