# TrimPod(RUS) for TrimUI Brick / Brick Pro / Anbernic RG34XXSP

<img width="256" height="192" alt="Main menu" src="screenshots/main-menu.png" /><img width="256" height="192" alt="Now playing" src="screenshots/now-playing.png" /><img width="256" height="192" alt="Synchronized lyrics" src="screenshots/lyrics.png" />
<img width="256" height="192" alt="Context menu" src="screenshots/context-menu.png" /><img width="256" height="192" alt="MilkDrop visualizer" src="screenshots/visualizer.png" />

## Description

**TrimPod(RUS)** is a Russian-language fork of the original
[TrimPod Classic](https://github.com/tyrannotorus/c-trimui-trimpod-classic-pak) by Werewolf Camp.
This fork is maintained by [B3L4CQU4](https://github.com/B3L4CQU4) and supports TrimUI Brick,
Brick Pro and Anbernic RG34XXSP. It builds upon the original project's heavily modified Rockbox base and includes 100
Winamp-inspired MilkDrop visualizations.

## User Disclaimer

TrimPod(RUS) is shared free with the TrimUI community and provided **as-is, without any
warranty**. It is an independent personal-use app; use it at your own risk. The author accepts no
liability for any damage to your device when it melts from awesomeness.

## Dev Disclaimer

This fork retains substantial code from TrimPod Classic and Rockbox. Changes specific to the fork
include Brick Pro/1024×768 and RG34XXSP/H700 adaptations, Russian localization,
Cyrillic-capable fonts and a Russian UTF-8 keyboard.
Note: Like the original project, this one was designed by a human, but slop-coded via codex. I'm a frontend developer. Have mercy on me )

## Supported Platforms

- **tg5040** — TrimUI Brick and TrimUI Brick Pro (`DEVICE=brick` / `brickpro`)
- **h700** — Anbernic RG34XXSP (`DEVICE=rg34xx`, `RGXX_MODEL=RG34xxSP`)

Other H700 models are deliberately rejected: their display geometry and controls have not been
validated by this port.

## Features

| Feature | Notes |
|---|---|
| **Milkdrop / projectM visualizer** | Real-time, audio-reactive presets on a dedicated CPU core. A curated set ships; toggle them from Settings. |
| **1st-gen iPod interface** | Chicago typography, chevron menus, page-slide transitions, and a Now Playing screen with scrolling track info. |
| **Pixel-perfect display** | Brick uses 512×384 → 1024×768; RG34XXSP uses 360×240 → 720×480. Both are exact 2× nearest-neighbour layouts. |
| **Russian interface** | Russian is the default; Settings → Language switches between Russian and English. Russian mode uses the pixel Mulmaru design for Cyrillic, Latin, digits and symbols with slightly wider glyph spacing; English keeps ChicagoFLF. The UTF-8 keyboard has RU/EN layouts. |
| **Audio spectrum** | A live spectrum on the Now Playing screen. |
| **Synchronized lyrics** | Press X in Now Playing to show local `.lrc` lyrics or fetch them from LRCLIB. The current line follows playback and is drawn larger; Up/Down scroll one line. |
| **iPod volume bar** | The volume rocker works from any screen; a momentary iPod-style bar shows the level. |
| **Folder-based music** | Browse your own source folders rather than a fixed library (default `/mnt/SDCARD/Music`; add more in Settings). |
| **Colour themes** | Several iPod colour palettes (Settings → Power → Color). |

## Controls

- `A` — select; play/pause in Now Playing.
- `B` — back/cancel; hold for the main menu.
- `MENU` — open the context menu as a centered popup.
- `X` — show/hide synchronized lyrics in Now Playing.
- D-pad or left stick — navigate; Up/Down scroll lyrics one line while lyrics are shown.
- `L1` / `R1` — seek backward/forward by 10 seconds in Now Playing.

RG34XXSP uses Xbox-style face-button semantics: the physical bottom button confirms as `A`, and
the physical right button goes back as `B`. Closing the lid turns only the display off; playback
continues, volume remains available, and opening the lid restores the previous display state.

## Lyrics

When lyrics mode is opened with `X`, TrimPod(RUS) checks these sources in order:

1. A sidecar file with the same name as the track, for example `Song.flac` → `Song.lrc`.
2. The persistent cache under `$HOME/lyrics/` on the SD card.
3. Synced lyrics from [LRCLIB](https://lrclib.net/), first by exact metadata and then by search.

The supported format is synchronized LRC with timestamps such as `[01:23.45]` or
`[01:23.456]`. Up/Down temporarily scrolls the text; automatic following resumes after five
seconds. Network lookup sends the track title, artist and duration to LRCLIB. It starts only when
lyrics mode is opened and neither a sidecar nor cached file is available.

GNU Wget is bundled in the pak for HTTPS, so nothing needs to be installed on the Brick or Brick
Pro. Its provenance, checksum and GPLv3 license are in `pak/licenses/`.

## Install

1. Download `TrimPod(RUS).pak.zip` from the [latest release](https://github.com/B3L4CQU4/c-trimui-trimpod-classic-pak-RUS/releases).
2. Copy it to `/mnt/SDCARD/Tools/tg5040/` on Brick or `/mnt/SDCARD/Tools/h700/` on RG34XXSP.
3. Extract it so the `TrimPod(RUS).pak` folder lands directly in that platform directory, then
   delete the zip.
4. On device, open **Tools → TrimPod(RUS)**.
5. Put music under `/mnt/SDCARD/Music` (or add folders from Settings), then pick a track.

> **Note:** `launch.sh` must sit directly inside the `.pak` folder. Some unzip tools double-wrap the
> archive — if you see `TrimPod(RUS).pak/TrimPod(RUS).pak/`, move the inner folder up one level.
> When upgrading from the original name, remove the old `Trimpod Classic.pak` directory so NextUI
> does not show both the old and renamed entries.
> NextUI normally treats `(RUS)` as a hidden pak tag and initially shows the folder as `TrimPod`.
> On first launch, TrimPod(RUS) safely adds its exact display-name alias to the shared Tools
> `map.txt`; return to or reopen Tools (restart NextUI if it is still cached) to see `TrimPod(RUS)`.

## Build

Cross-compiled in the official NextUI `tg5040` and `h700` Docker toolchains. Needs Docker (and
ADB to deploy).

```sh
# Required after changing projectM, and always performed by release CI:
bash tools/build_projectm.sh
./build.sh      # build main's build-trimpod plus build-trimpod-h700
# ./build.sh h700 clean   # optional: clean H700-only build
# ./build.sh tg5040      # optional: incremental tg5040-only build
./package.sh    # assemble the pak, release zip and LGPL relinking kit

# Deploy RG34XXSP. Clear the destination first so adb does not nest the pak.
PAK="/mnt/SDCARD/Tools/h700/TrimPod(RUS).pak"
adb shell "rm -rf \"$PAK\"" && adb push 'dist/TrimPod(RUS).pak' "$PAK"
```

`./build.sh` also reproducibly rebuilds the compact 18/20/24px language-aware UI
Russian fonts from the vendored PixelMplus and Mulmaru sources. English loads
the original checked-in ChicagoFLF files unchanged; Russian uses Mulmaru for all available glyphs.
Release CI first rebuilds the statically linked projectM archives from their
checked-in corresponding source.
`./build.sh clean` forces a fresh
reconfigure. A full pak deploy resets on-device settings — the live
config (`trimpod/config.cfg`) is bind-mounted from inside the pak, so replacing it restores defaults.
For code-only changes, push `trimpod/trimpod` on tg5040 or
`runtimes/h700/trimpod/trimpod` on H700 instead of the whole pak.

### Architecture

TrimPod(RUS), like the original TrimPod Classic, is a custom Rockbox **SDL-application target**
(`retro-handheld`) with independent binaries and data trees for each platform. tg5040 renders at
512×384; H700 renders at 360×240. Both are enlarged exactly 2× to their physical displays. The
launcher keeps tg5040 in `trimpod/` as in `main` and selects `runtimes/h700` only on RG34XXSP, then
bind-mounts it to `/tmp/trimpod`, and restores CPU, mixer and NextUI input state on exit. Brick input
uses SDL. RG34XXSP reads its built-in controls and hall sensor directly from evdev while retaining
SDL for external Bluetooth/USB controllers; no gptokeyb2 shim is used.

### Layout

| Path | Role |
|---|---|
| `build.sh`, `package.sh` | build, then assemble the pak |
| `Dockerfile.trimpod` | shared layer over the official NextUI tg5040/H700 toolchains |
| `pak/` | the pak skeleton: `launch.sh`, bundled Wget, licenses, `config.cfg`, `.sys` files (`pak.json` lives at the repo root) |
| `assets/` | theme, language-aware UI font sources/output, icons and Milkdrop presets |
| `third_party/` | complete upstream source archives required by bundled command-line tools |
| `apps/`, `firmware/`, `lib/`, `tools/` | the Rockbox source tree + the Trimpod target |

> clangd flags missing `config.h` / undeclared identifiers in this tree — they resolve only inside
> the Docker build, which is the source of truth.

## License

TrimPod(RUS) is a fork of TrimPod Classic and an independent build of
[Rockbox](https://github.com/Rockbox/rockbox). It is
licensed under the [**GNU General Public License v2.0**](LICENSE).

Third-party license texts and attributions shipped with the pak are indexed in
[`pak/licenses/THIRD_PARTY_NOTICES.txt`](pak/licenses/THIRD_PARTY_NOTICES.txt).
The complete projectM 4.1.6 source, the TrimPod framebuffer patch, provenance,
and instructions for replacing and relinking the statically linked library are
under [`lib/projectm/`](lib/projectm/). Release builds also publish a matching
`TrimPod(RUS)-relink-kit.tar.gz` asset with the machine-readable Rockbox object
files needed for relinking.

### Credits

- **TrimPod Classic** by Werewolf Camp — the original project this fork is based on
  ([tyrannotorus/c-trimui-trimpod-classic-pak](https://github.com/tyrannotorus/c-trimui-trimpod-classic-pak)).
- **B3L4CQU4** — maintainer of the TrimPod(RUS) fork.
- **Rockbox** — the firmware this is built from. GPLv2.
- **projectM 4.1.6** — the Milkdrop-compatible visualizer engine
  ([projectM-visualizer/projectm](https://github.com/projectM-visualizer/projectm)).
  LGPL-2.1-or-later; complete source and the local framebuffer patch are
  included under `lib/projectm/`.
- **projectM-eval 1.0.5** — the expression evaluator bundled with projectM. MIT License.
- **Cream of the Crop** — the bundled Milkdrop preset pack ([presets-cream-of-the-crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)).
- **1ST_GEN_REMIX** theme by Monica G. — [themes.rockbox.org #3958](https://themes.rockbox.org/index.php?themeid=3958), modified for TrimPod and distributed under CC BY-SA 3.0.
- **ChicagoFLF** — public-domain Chicago-style Latin glyphs (bundled, anti-aliased).
- **Mulmaru** by Mushsooni — the complete Russian UI glyph design in
  TrimpodRus. SIL Open Font License 1.1.
- **PixelMplus** by Itou Hiroki — the Japanese glyphs merged into the UI fonts
  ([itouhiro/PixelMplus](https://github.com/itouhiro/PixelMplus)). M+ FONT LICENSE.
- **NextUI** by LoveRetro — the launcher and toolchain this builds against.
- **NextUI Music Player** by Mohammad Syuhada — the lyrics lookup and caching
  flow in TrimPod(RUS) is based in part on its implementation
  ([mohammadsyuhada/nextui-music-player](https://github.com/mohammadsyuhada/nextui-music-player)). MIT License.
- **GNU Wget 1.24.5** — bundled AArch64 HTTPS downloader used for LRCLIB.
  GPLv3-or-later; its official source archive is included under
  `third_party/wget/`.
- Hardware-enablement files adapted from Hairo R. Carela's GPL work in
  [IncognitoMan/rockbox](https://github.com/IncognitoMan/rockbox); original file
  headers are retained.
