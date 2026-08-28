#!/usr/bin/env bash
# Package the object files needed to relink both platform binaries with a
# modified statically linked projectM.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/TrimPod(RUS)-relink-kit.tar.gz"
BUILDS="build-trimpod build-trimpod-h700"

for build in $BUILDS; do
    [ -f "$ROOT/$build/Makefile" ] && [ -f "$ROOT/$build/make.dep" ] || {
        echo "Missing $build metadata -- run ./build.sh first." >&2
        exit 1
    }
done

mkdir -p "$ROOT/dist"
list="$(mktemp)"
trap 'rm -f "$list"' EXIT

(
    cd "$ROOT"
    for build in $BUILDS; do
        printf '%s\0' \
            "$build/Makefile" \
            "$build/make.dep" \
            "$build/autoconf.h" \
            "$build/lang_enum.h" \
            "$build/rbversion.h" \
            "$build/sysfont.c" \
            "$build/sysfont.h"
        find "$build" -type f \( -name '*.o' -o -name '*.a' \) -print0
    done
    printf '%s\0' \
        lib/projectm/include \
        lib/projectm/lib \
        lib/projectm/source \
        lib/projectm/patches \
        lib/projectm/toolchains \
        lib/projectm/PROVENANCE.md \
        lib/projectm/RELINKING.md \
        tools/build_projectm.sh
) > "$list"

tar -C "$ROOT" --null -T "$list" -czf "$OUT"
echo ">> Relinking materials: $OUT"
