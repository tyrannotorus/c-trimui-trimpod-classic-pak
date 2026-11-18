# Trimpod Classic For TrimUI Brick
Trimpod Classic is an iPod classic-inspired, offline music player built for the TrimUI Brick.

<img width="800" height="800" alt="eXF4hgWWRl-800" src="https://github.com/user-attachments/assets/52b97a3b-38a7-4d4b-8c72-cb310936c3da" />

# Features
- Classic iPod styling
- Plays offline music
- Integrated milkdrop visualizations
- Dynamic playlist and track queuing support
- USBC headphone and bluetooth audio support
- Battery charge protection setting
- Auto usb-mounting when plugged in
- Video support

# Supported Devices
## Trimui Brick/Hammer (easy install)
1. Install via the [NextUI's](https://github.com/LoveRetro/NextUI) [Pak Store](https://github.com/LoveRetro/nextui-pak-store).
## Trimui Brick/Hammer (manual install)
1. Download `Trimpod.pak.zip` from the [latest release](https://github.com/tyrannotorus/c-trimui-trimpod-classic-pak/releases).
2. Copy it to `/mnt/SDCARD/Tools/tg5040/` (mount the SD card or `adb push`).
3. Extract it so the `Trimpod Classic.pak` folder lands directly in `/mnt/SDCARD/Tools/tg5040/`, then
   delete the zip.
4. On device, open **Tools → Trimpod Classic**.
5. Put music under `/mnt/SDCARD/Music` (or add folders from Settings), then pick a track.

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

## License and Credits

Trimpod Classic is an independent build of [Rockbox](https://github.com/Rockbox/rockbox) and is
licensed under the **GNU General Public License v2.0**.

- **Rockbox** — the firmware this is built from. GPLv2.
- **projectM** — the Milkdrop-compatible visualizer engine ([projectM-visualizer/projectm](https://github.com/projectM-visualizer/projectm)). LGPL 2.1.
- **Cream of the Crop** — the bundled Milkdrop preset pack ([presets-cream-of-the-crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)).
- **1ST_GEN_REMIX** theme by Monica G. — [themes.rockbox.org #3958](https://themes.rockbox.org/index.php?themeid=3958).
- **ChicagoFLF** — an openly-licensed Chicago typeface reproduction (bundled, anti-aliased).
- **PixelMplus** by Itou Hiroki — the Japanese glyphs merged into the UI fonts
  ([itouhiro/PixelMplus](https://github.com/itouhiro/PixelMplus)). M+ FONT LICENSE.
- **NextUI** by LoveRetro — the launcher and toolchain this builds against.
- Hardware-enablement files adapted from IncognitoMan's GPL work.
