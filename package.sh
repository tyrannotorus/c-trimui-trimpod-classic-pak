#!/usr/bin/env bash
# Assemble one universal NextUI pak from the independent tg5040 and H700 builds.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PAK="$ROOT/dist/TrimPod(RUS).pak"
PYTHON="${PYTHON:-python3}"
THEME_TG="$ROOT/assets/theme/1ST_GEN_REMIX/.rockbox"
THEME_H700="$ROOT/assets/theme/1ST_GEN_REMIX-H700/.rockbox"
STATIC_RUNTIME="$ROOT/pak/trimpod"

for platform in tg5040 h700; do
    if [ "$platform" = tg5040 ]; then
        zip="$ROOT/build-trimpod/trimpod-full.zip"
    else
        zip="$ROOT/build-trimpod-h700/trimpod-full.zip"
    fi
    [ -f "$zip" ] || {
        echo "Missing $zip -- run ./build.sh first." >&2
        exit 1
    }
done

rm -rf "$ROOT/dist"
mkdir -p "$PAK"

# Start with main's pak skeleton. The tg5040 runtime stays at pak/trimpod;
# H700 alone is staged separately because it needs a different binary/theme.
cp -a "$ROOT/pak/." "$PAK/"
cp -a "$ROOT/pak.json" "$PAK/pak.json"

stage_runtime() {
    platform="$1"
    if [ "$platform" = tg5040 ]; then
        zip="$ROOT/build-trimpod/trimpod-full.zip"
        runtime="$PAK/trimpod"
    else
        zip="$ROOT/build-trimpod-h700/trimpod-full.zip"
        runtime="$PAK/runtimes/h700/trimpod"
    fi
    tmp="$(mktemp -d)"

    echo ">> Staging $platform runtime"
    unzip -q "$zip" -d "$tmp"
    rm -rf "$runtime"
    mkdir -p "$(dirname "$runtime")"
    cp -a "$tmp/tmp/trimpod" "$runtime"
    rm -rf "$tmp"

    for language in english russian; do
        [ -f "$runtime/langs/$language.lng" ] || {
            echo "$platform build is missing $language.lng -- rerun ./build.sh $platform clean." >&2
            exit 1
        }
    done

    # Keep the fork's only supported theme plus Rockbox failsafes.
    find "$runtime/themes" -type f \
        ! -name 'rockbox_failsafe.cfg' ! -name 'rockbox_default_icons.cfg' \
        -delete 2>/dev/null
    find "$runtime/wps" -mindepth 1 -maxdepth 1 \
        ! -name 'rockbox_failsafe*' -exec rm -rf {} + 2>/dev/null
    rm -rf "$runtime/backdrops" 2>/dev/null

    cp -a "$THEME_TG/wps/." "$runtime/wps/"
    cp -a "$THEME_TG/icons/." "$runtime/icons/"
    cp -a "$THEME_TG/themes/." "$runtime/themes/"
    if [ "$platform" = h700 ]; then
        # Text layouts are native 360x240. Raster sprites are derived from the
        # checked-in 512x384 theme in staging, never by mutating source assets.
        cp -a "$THEME_H700/wps/." "$runtime/wps/"
        cp -a "$THEME_H700/themes/." "$runtime/themes/"
        "$PYTHON" "$ROOT/tools/prepare_h700_theme.py" \
            "$runtime/wps/1ST_GEN_REMIX"
    fi

    rm -f "$runtime/fonts/"*.fnt "$runtime/fonts/COPYING-fonts.txt" 2>/dev/null || true
    for family in ChicagoFLF TrimpodRus; do
        for size in 18 20 24; do
            font="$ROOT/assets/fonts/${size}-${family}.fnt"
            [ -f "$font" ] || {
                echo "Missing $font -- run ./build.sh first." >&2
                exit 1
            }
            cp "$font" "$runtime/fonts/"
        done
    done
    cp "$ROOT/assets/fonts/COPYING" "$runtime/fonts/COPYING-fonts.txt"
    cp "$ROOT/assets/fonts/sources/Mulmaru-OFL.txt" \
       "$runtime/fonts/OFL-Mulmaru.txt"

    mkdir -p "$runtime/presets"
    cp -r "$ROOT/assets/presets/." "$runtime/presets/"

    # config.cfg and systems/*.sys are shared source files. Platform-specific
    # paths are selected by launch.sh at runtime.
    cp -a "$STATIC_RUNTIME/." "$runtime/"
    chmod +x "$runtime/trimpod"
}

stage_runtime tg5040
stage_runtime h700

# CR in a sourced .sys file becomes part of its sysfs path under BusyBox sh.
find "$PAK" -type f \( -name '*.sh' -o -name '*.sys' \) \
    -exec sed -i 's/\r$//' {} +
chmod +x "$PAK/launch.sh" "$PAK/bin/wget"

echo ">> Packaging dist/TrimPod(RUS).pak.zip"
if command -v zip >/dev/null 2>&1; then
    ( cd "$PAK" && zip -qr "$ROOT/dist/TrimPod(RUS).pak.zip" . )
else
    ( cd "$PAK" && "$PYTHON" -c "import shutil,sys; shutil.make_archive(sys.argv[1],'zip','.')" \
          "$ROOT/dist/TrimPod(RUS).pak" )
fi

echo ">> Done: $PAK"
du -sh "$PAK"
du -h "$ROOT/dist/TrimPod(RUS).pak.zip"

# projectM is LGPL and linked statically. Include relinking materials for both
# independently built platform binaries.
bash "$ROOT/tools/package_relink_kit.sh"
du -h "$ROOT/dist/TrimPod(RUS)-relink-kit.tar.gz"
