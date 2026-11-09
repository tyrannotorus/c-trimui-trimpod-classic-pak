#!/usr/bin/env bash
# Assemble dist/Trimpod.pak from the build (build-trimpod), the bundled theme
# (assets/theme), and the static pak files (pak/ -- launch.sh, licenses,
# config.cfg and the .sys device files).
# Run ./build.sh first (it produces the runtime zip via 'make fullzip').
# The app's data dir is /tmp/trimpod (NOT /tmp/rockbox) to avoid clashing with
# other Rockbox installs; inside the pak it lives in trimpod/ and the binary is
# named "trimpod". "Rockbox" only remains in About/Credits.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ZIP="$ROOT/build-trimpod/trimpod-full.zip"
PAK="$ROOT/dist/Trimpod.pak"
THEME="$ROOT/assets/theme/1ST_GEN_REMIX/.rockbox"

[ -n "$ZIP" ] && [ -f "$ZIP" ] || { echo "Missing build-trimpod/*-full.zip — run ./build.sh first."; exit 1; }

echo ">> Staging runtime"
rm -rf "$ROOT/dist"
mkdir -p "$PAK"
tmp="$(mktemp -d)"
unzip -q "$ZIP" -d "$tmp"
# The zip stores the runtime under tmp/trimpod/ (data dir = /tmp/trimpod).
cp -a "$tmp/tmp/trimpod" "$PAK/trimpod"
rm -rf "$tmp"

echo ">> Pruning stock Rockbox themes (1ST_GEN_REMIX is the only theme)"
# The build zip bundles the stock themes (cabbiev2, classic_statusbar, ...).
# Drop them; keep only 1ST_GEN_REMIX + the failsafe the skin engine falls back to.
find "$PAK/trimpod/themes" -type f \
     ! -name 'rockbox_failsafe.cfg' ! -name 'rockbox_default_icons.cfg' -delete 2>/dev/null
find "$PAK/trimpod/wps" -mindepth 1 -maxdepth 1 \
     ! -name 'rockbox_failsafe*' -exec rm -rf {} + 2>/dev/null
rm -rf "$PAK/trimpod/backdrops" 2>/dev/null

echo ">> Injecting 1ST_GEN_REMIX theme"
cp -a "$THEME/wps/." "$PAK/trimpod/wps/"
cp -a "$THEME/icons/." "$PAK/trimpod/icons/"
cp -a "$THEME/themes/." "$PAK/trimpod/themes/"

echo ">> Pruning stock Rockbox fonts (only ChicagoFLF ships; Font menu removed)"
# The build zip bundles the whole upstream font set (Terminus, Adobe-Helvetica, ...).
# Nothing references them (themes are hardcoded to ChicagoFLF), so drop them all.
rm -f "$PAK/trimpod/fonts/"*.fnt "$PAK/trimpod/fonts/COPYING-fonts.txt" 2>/dev/null || true

echo ">> Injecting ChicagoFLF fonts (see assets/fonts)"
cp -a "$ROOT"/assets/fonts/*.fnt "$PAK/trimpod/fonts/"
cp "$ROOT/assets/fonts/COPYING" "$PAK/trimpod/fonts/COPYING-fonts.txt"

echo ">> Injecting Milkdrop visualizer presets (see assets/presets)"
mkdir -p "$PAK/trimpod/presets"
# Ship the curated flat preset set (Settings -> Visualizers toggles them on/off).
cp -r "$ROOT/assets/presets/." "$PAK/trimpod/presets/"

echo ">> Overlaying the static pak files (pak/: launch.sh, licenses, config, .sys)"
# pak/ mirrors the deployed pak skeleton; its trimpod/ merges onto the built one.
# pak.json lives at the repo root (the Pak Store reads it there); copy it in too.
cp -a "$ROOT/pak/." "$PAK/"
cp -a "$ROOT/pak.json" "$PAK/pak.json"

echo ">> Injecting umtprd (USB file transfer, GPL-3.0; source in lib/umtprd)"
cp "$ROOT/build-trimpod/umtprd/umtprd" "$PAK/trimpod/umtprd"
cp "$ROOT/lib/umtprd/LICENSE" "$PAK/licenses/umtprd_gplv3.txt"
chmod +x "$PAK/launch.sh" "$PAK/trimpod/trimpod" "$PAK/trimpod/umtprd"

echo ">> Injecting stock video player plugins (AC-3, DivX; source in lib/tplayer_plugins)"
mkdir -p "$PAK/trimpod/video"
cp "$ROOT"/build-trimpod/tplayer_plugins/*.so "$PAK/trimpod/video/"

echo ">> Packaging the Pak Store release asset (dist/Trimpod.pak.zip)"
# NextUI Pak Store: the zip's ROOT must be the contents of the .pak directory
# (launch.sh, pak.json, trimpod/, ...) -- the store names the installed folder
# from pak.json "name".  The filename must match release_filename in pak.json,
# and the GitHub release tag must match the pak.json "version".
rm -f "$ROOT/dist/Trimpod.pak.zip"
if command -v zip >/dev/null 2>&1; then
    ( cd "$PAK" && zip -qr "$ROOT/dist/Trimpod.pak.zip" . )
else   # no 'zip' binary -- fall back to python3 (contents at archive root)
    ( cd "$PAK" && python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1],'zip','.')" \
          "$ROOT/dist/Trimpod.pak" )
fi

echo ">> Done: $PAK"
du -sh "$PAK"
du -h "$ROOT/dist/Trimpod.pak.zip"
