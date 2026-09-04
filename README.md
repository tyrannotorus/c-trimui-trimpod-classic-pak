# Trimpod Classic For TrimUI Brick

<img width="800" height="800" alt="eXF4hgWWRl-800" src="https://github.com/user-attachments/assets/52b97a3b-38a7-4d4b-8c72-cb310936c3da" />

## Description

Trimpod Classic is an iPod classic-inspired, offline music player built for the TrimUI Brick.
It builds upon a heavily-modified Rockbox base and includes 100 Winamp-inspired milkdrop visualizations.

## User Disclaimer

Trimpod Classic is shared free with the TrimUI community and provided **as-is, without any
warranty**. It is an independent personal-use app; use it at your own risk. The author accepts no
liability for any damage to your device when it melts from awesomeness.

## Dev Disclaimer

While I am a 10+ year industry veteran, note this project has been human-directed as far as architecture,
but 100% slop-coded. I've done my best to ensure professional standards from a high-level, but make no
guarantee of "thoughtfully-engineered work line by line" at a low-level. Thus is the warning, so beware
when forking. Here may be dragons.

## Supported Platforms

- **tg5040** — TrimUI Brick
- I have no other devices to test with. Send me one! :)

## Features

| Feature | Notes |
|---|---|
| **Milkdrop / projectM visualizer** | Real-time, audio-reactive presets on a dedicated CPU core. A curated set ships; toggle them from Settings. |
| **1st-gen iPod interface** | Chicago typography, chevron menus, page-slide transitions, and a Now Playing screen with scrolling track info. |
| **Audio spectrum** | A live spectrum on the Now Playing screen. |
| **iPod volume bar** | The volume rocker works from any screen; a momentary iPod-style bar shows the level. |
| **Folder-based music** | Browse your own source folders rather than a fixed library (default `/mnt/SDCARD/Music`; add more in Settings). |
| **Audiobooks (.m4b)** | Every book resumes exactly where you left off, and L2/R2 skip between its chapters (default `/mnt/SDCARD/Audiobooks`; add folders in Settings). |
| **Colour themes** | Several iPod colour palettes (Settings → Power → Color). |

## Install

1. Download `Trimpod.pak.zip` from the [latest release](https://github.com/tyrannotorus/c-trimui-trimpod-classic-pak/releases).
2. Copy it to `/mnt/SDCARD/Tools/tg5040/` (mount the SD card or `adb push`).
3. Extract it so the `Trimpod Classic.pak` folder lands directly in `/mnt/SDCARD/Tools/tg5040/`, then
   delete the zip.
4. On device, open **Tools → Trimpod Classic**.
5. Put music under `/mnt/SDCARD/Music` (or add folders from Settings), then pick a track.

> **Note:** `launch.sh` must sit directly inside the `.pak` folder. Some unzip tools double-wrap the
> archive — if you see `Trimpod Classic.pak/Trimpod Classic.pak/`, move the inner folder up one level.

## Build

Cross-compiled in the NextUI `tg5040` Docker toolchain. Needs `docker` (and `adb` to deploy).

```sh
./build.sh      # cross-compile Rockbox -> build-trimpod/trimpod (+ the runtime zip)
./package.sh    # assemble dist/Trimpod.pak (+ dist/Trimpod.pak.zip)

# Deploy: clear the destination FIRST. `adb push <dir> <existing-dir>` nests the
# source inside it (you'd get "Trimpod Classic.pak/Trimpod.pak/...") and leaves
# stale files behind; removing it first makes the push land at the pak root.
PAK="/mnt/SDCARD/Tools/tg5040/Trimpod Classic.pak"
adb shell "rm -rf \"$PAK\"" && adb push dist/Trimpod.pak "$PAK"
```

`./build.sh clean` forces a fresh reconfigure. A full pak deploy resets on-device settings — the live
config (`trimpod/config.cfg`) is bind-mounted from inside the pak, so replacing it restores defaults.
For code-only changes, push just the rebuilt binary (`trimpod/trimpod`) instead of the whole pak.

### Architecture

Trimpod Classic is a custom Rockbox **SDL-application target** (`retro-handheld`) that renders at a
logical 320×240 and is hardware-upscaled 3.2× to the Brick's 1024×768 display. It runs hosted under
NextUI rather than on bare metal: `launch.sh` sources per-device sysfs paths, bind-mounts the pak's
data dir to `/tmp/trimpod`, applies the CPU governor, and runs the binary — then tears all of that
down on exit. Input is read natively through SDL like NextUI: the gamepad and volume rocker come in
as SDL joystick events and the power key as an SDL keyboard scancode (no gptokeyb2 shim).

### Layout

| Path | Role |
|---|---|
| `build.sh`, `package.sh` | build, then assemble the pak |
| `Dockerfile.trimpod` | the toolchain image (NextUI tg5040 + `zip` + an `sdl2-config` shim) |
| `pak/` | the pak skeleton: `launch.sh`, `config.cfg`, `.sys` files (`pak.json` lives at the repo root) |
| `assets/` | product assets — the theme, ChicagoFLF fonts, icons, Milkdrop presets, the skin build |
| `apps/`, `firmware/`, `lib/`, `tools/` | the Rockbox source tree + the Trimpod target |

> clangd flags missing `config.h` / undeclared identifiers in this tree — they resolve only inside
> the Docker build, which is the source of truth.

## License

Trimpod Classic is an independent build of [Rockbox](https://github.com/Rockbox/rockbox) and is
licensed under the **GNU General Public License v2.0**.

### Credits

- **Rockbox** — the firmware this is built from. GPLv2.
- **projectM** — the Milkdrop-compatible visualizer engine ([projectM-visualizer/projectm](https://github.com/projectM-visualizer/projectm)). LGPL 2.1.
- **Cream of the Crop** — the bundled Milkdrop preset pack ([presets-cream-of-the-crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)).
- **1ST_GEN_REMIX** theme by Monica G. — [themes.rockbox.org #3958](https://themes.rockbox.org/index.php?themeid=3958).
- **ChicagoFLF** — an openly-licensed Chicago typeface reproduction (bundled, anti-aliased).
- **PixelMplus** by Itou Hiroki — the Japanese glyphs merged into the UI fonts
  ([itouhiro/PixelMplus](https://github.com/itouhiro/PixelMplus)). M+ FONT LICENSE.
- **NextUI** by LoveRetro — the launcher and toolchain this builds against.
- Hardware-enablement files adapted from IncognitoMan's GPL work.
