#!/bin/sh
# Trimpod — Rockbox-based music player for NextUI tg5040 and RG34XXSP/H700.
PAK_DIR="$(CDPATH= cd "$(dirname "$0")" 2>/dev/null && pwd)" || exit 1
RBDIR="$PAK_DIR/trimpod"
RBDIR_BIND="/tmp/trimpod"
cd "$PAK_DIR" || exit 1

# NextUI treats a parenthesized pak suffix as a hidden platform/tag marker, so
# TrimPod(RUS).pak is normally displayed as just "TrimPod".  Register an
# official Tools/map.txt alias for the actual folder name without disturbing
# aliases belonging to other tools.  NextUI picks it up when Tools is reloaded.
PAK_NAME="$(basename "$PAK_DIR")"
TOOLS_DIR="$(dirname "$PAK_DIR")"
TOOLS_MAP="$TOOLS_DIR/map.txt"
TOOLS_MAP_TMP="$TOOLS_DIR/.map.txt.trimpod.$$"

register_tools_name() {
  if [ -f "$TOOLS_MAP" ]; then
    awk -v key="$PAK_NAME" 'index($0, key "\t") != 1' "$TOOLS_MAP" > "$TOOLS_MAP_TMP" || {
      rm -f "$TOOLS_MAP_TMP"
      return
    }
  else
    : > "$TOOLS_MAP_TMP" || return
  fi

  if ! printf '%s\t%s\n' "$PAK_NAME" 'TrimPod(RUS)' >> "$TOOLS_MAP_TMP" ||
     ! mv "$TOOLS_MAP_TMP" "$TOOLS_MAP"; then
    rm -f "$TOOLS_MAP_TMP"
  fi
}

register_tools_name

# Rockbox stores its config/playlists under HOME; keep it on the SD card.
HOME="$USERDATA_PATH"

# Keep the tg5040 runtime in main's original location. Only H700 needs a
# separate data tree because its binary and 360x240 theme are different.
case "$PLATFORM" in
  tg5040)
    case "$DEVICE" in
      brick)    RBDEVICE="TUI-Brick" ;;
      brickpro) RBDEVICE="TUI-BrickPro" ;;
      *)
        echo "Unsupported tg5040 device: ${DEVICE:-unset} (expected brick or brickpro)" >&2
        exit 1
        ;;
    esac
    PROFILE="tui-brick.sys"
    LOGICAL_SIZE="512x384"
    ;;
  h700)
    model="$(printf '%s' "${RGXX_MODEL:-}" | tr '[:upper:]' '[:lower:]')"
    case "$DEVICE:$model" in
      rg34xx:rg34xxsp|rg34xxsp:rg34xxsp) RBDEVICE="RG34XXSP" ;;
      *)
        echo "Unsupported H700 device: DEVICE=${DEVICE:-unset} RGXX_MODEL=${RGXX_MODEL:-unset} (expected RG34XXSP)" >&2
        exit 1
        ;;
    esac
    RBDIR="$PAK_DIR/runtimes/h700/trimpod"
    PROFILE="rg34xxsp.sys"
    LOGICAL_SIZE="360x240"
    ;;
  *)
    echo "Unsupported platform: ${PLATFORM:-unset} (expected tg5040 or h700)" >&2
    exit 1
    ;;
esac

[ -x "$RBDIR/trimpod" ] || {
  echo "Missing $PLATFORM runtime: $RBDIR/trimpod" >&2
  exit 1
}
[ -f "$RBDIR/systems/$PROFILE" ] || {
  echo "Missing hardware profile: $RBDIR/systems/$PROFILE" >&2
  exit 1
}
. "$RBDIR/systems/$PROFILE"

echo "Trimpod device: PLATFORM=$PLATFORM DEVICE=$DEVICE MODEL=${RGXX_MODEL:-${TRIMUI_MODEL:-unknown}} logical=$LOGICAL_SIZE zoom=$ZOOMVAL" >&2

# The app's data dir is built as /tmp/trimpod, so bind our pak data there.
if [ ! -f "$RBDIR_BIND/rockbox" ]; then
  mkdir -p "$RBDIR_BIND"
  mount --bind "$RBDIR" "$RBDIR_BIND"
fi

# Rewrite bundled theme cfgs from the canonical /.rockbox to the live bind path.
for theme in "$RBDIR"/themes/*.cfg; do
  [ -f "$theme" ] && sed -i 's#/\.rockbox#/tmp/trimpod#g' "$theme"
done

# --- CPU frequency (runtime-only; restored on exit) --------------------------
# Trimpod owns the CPU Frequency policy IN THE APP (Settings -> Power -> CPU):
# power-target.c is the single source that defines/persists (trimpod/cpu_freq.txt)
# and applies the choice -- at startup and on a live menu change. Here we only
# snapshot the system's original cpufreq state so we can restore it on exit
# (below), so whatever the app sets is never left on the device permanently.
CPUP=/sys/devices/system/cpu/cpufreq/policy0
if [ -d "$CPUP" ]; then
  TRIMPOD_OLD_GOV=$(cat "$CPUP/scaling_governor" 2>/dev/null)
  TRIMPOD_OLD_MIN=$(cat "$CPUP/scaling_min_freq" 2>/dev/null)
  TRIMPOD_OLD_MAX=$(cat "$CPUP/scaling_max_freq" 2>/dev/null)
fi

unset SDL_HQ_SCALER SDL_ROTATION SDL_BLITTER_DISABLED

# Input is native with no gptokeyb2 shim: tg5040 uses SDL events as in main;
# H700 reads the built-in evdev nodes and keeps SDL for external controllers.

# Side switch = input lock only. Freeze NextUI's keymon (it buzzes/dims/mutes on
# that switch) while we run; thaw on exit. Fallback: blank MutedVolume @ byte 56.
KEYMON_PIDS="$(pidof keymon.elf 2>/dev/null)"
SHM=/dev/shm/SharedSettings
if [ -f "$SHM" ]; then
  dd if="$SHM" bs=1 skip=56 count=4 of=/tmp/trimpod_muted_vol 2>/dev/null
  printf '\273\377\377\377' | dd of="$SHM" bs=1 seek=56 count=4 conv=notrunc 2>/dev/null
fi
[ -n "$KEYMON_PIDS" ] && kill -STOP $KEYMON_PIDS 2>/dev/null

# Audio: on tg5040, 'DAC volume' 160/255 is NextUI's own ceiling (libmsettings SetRawVolume
# never exceeds it), so matching it can't over-drive the little speaker.
# 'digital volume' (0 = loudest) is owned by the app while it runs --
# trimpod-alsa.c writes it on every volume change, so the 0 here is just the
# baseline.  'Soft Volume Master' is the OS softvol stage, which attenuates in
# SOFTWARE and would re-create the quiet-passage gating, so pin it wide open.
# The H700 branch uses its equivalent lineout controls. Everything changed here
# is snapshotted and restored on exit.
TRIMPOD_DV="$(amixer sget 'digital volume' 2>/dev/null | sed -n 's/.*Mono: \([0-9][0-9]*\).*/\1/p')"
[ -n "$TRIMPOD_DV" ] && amixer -q sset 'digital volume' 0 2>/dev/null
if [ "${AUDIO_PROFILE:-tg5040}" = h700 ]; then
  TRIMPOD_LINEOUT="$(amixer sget 'lineout volume' 2>/dev/null | sed -n 's/.*Mono: \([0-9][0-9]*\).*/\1/p')"
  TRIMPOD_SPK_SWITCH="$(amixer sget 'SPK' 2>/dev/null | sed -n 's/.*Playback \[\(on\|off\)\].*/\1/p' | head -1)"
  TRIMPOD_LINEOUT_SWITCH="$(amixer sget 'LINEOUT' 2>/dev/null | sed -n 's/.*Playback \[\(on\|off\)\].*/\1/p' | head -1)"
  TRIMPOD_OUTL_SWITCH="$(amixer sget 'OutputL Mixer DACL' 2>/dev/null | sed -n 's/.*Playback \[\(on\|off\)\].*/\1/p' | head -1)"
  TRIMPOD_OUTR_SWITCH="$(amixer sget 'OutputR Mixer DACR' 2>/dev/null | sed -n 's/.*Playback \[\(on\|off\)\].*/\1/p' | head -1)"
  [ -n "$TRIMPOD_LINEOUT" ] && amixer -q sset 'lineout volume' 31 2>/dev/null
  amixer -q sset 'SPK' on 2>/dev/null
  amixer -q sset 'LINEOUT' on 2>/dev/null
  amixer -q sset 'OutputL Mixer DACL' on 2>/dev/null
  amixer -q sset 'OutputR Mixer DACR' on 2>/dev/null
else
  TRIMPOD_DAC="$(amixer sget 'DAC volume' 2>/dev/null | sed -n 's/.*Front Left: \([0-9][0-9]*\).*/\1/p')"
  TRIMPOD_SV="$(amixer sget 'Soft Volume Master' 2>/dev/null | sed -n 's/.*Front Left: \([0-9][0-9]*\).*/\1/p')"
  [ -n "$TRIMPOD_DAC" ] && amixer -q sset 'DAC volume' 160 2>/dev/null
  [ -n "$TRIMPOD_SV" ]  && amixer -q sset 'Soft Volume Master' 255 2>/dev/null
fi
# Bluetooth needs nothing here: trimpod-alsa.c drives the device's own A2DP
# volume, so the level the user leaves is the level the system keeps.
# NextUI at volume 0 (or side-switch mute) ALSO latches the speaker driver's
# hard mute (/sys/class/speaker/mute), which survives into our session and
# silences Trimpod regardless of its own volume.  Unmute for the session and
# restore the user's NextUI state on exit.
SPK_MUTE=/sys/class/speaker/mute
if [ "${AUDIO_PROFILE:-tg5040}" != h700 ]; then
  TRIMPOD_SPK_MUTE="$(cat "$SPK_MUTE" 2>/dev/null)"
  [ -n "$TRIMPOD_SPK_MUTE" ] && echo 0 > "$SPK_MUTE" 2>/dev/null
fi

# ALL teardown lives here so it runs on every catchable exit path -- a clean
# binary return, or INT/TERM/HUP (e.g. NextUI stopping the pak) -- not just the
# fall-through after the binary returns.  Idempotent and ordered: stop the app +
# input, restore NextUI's keymon/audio/CPU, then release the bind mount.  The
# bind mount in particular used to never be unmounted -> it leaked, surviving
# exit and stacking on aborted runs.  (SIGKILL is uncatchable; nothing can help
# that, but it's the only gap now.)
cleanup() {
  kill -9 "$(pidof trimpod)"   2>/dev/null
  [ -n "$KEYMON_PIDS" ] && kill -CONT $KEYMON_PIDS 2>/dev/null
  [ -f /tmp/trimpod_muted_vol ] && dd if=/tmp/trimpod_muted_vol of=/dev/shm/SharedSettings bs=1 seek=56 count=4 conv=notrunc 2>/dev/null
  [ -n "$TRIMPOD_DV" ] && amixer -q sset "digital volume" "$TRIMPOD_DV" 2>/dev/null
  [ -n "$TRIMPOD_DAC" ] && amixer -q sset "DAC volume" "$TRIMPOD_DAC" 2>/dev/null
  [ -n "$TRIMPOD_SV" ] && amixer -q sset "Soft Volume Master" "$TRIMPOD_SV" 2>/dev/null
  [ -n "$TRIMPOD_LINEOUT" ] && amixer -q sset "lineout volume" "$TRIMPOD_LINEOUT" 2>/dev/null
  [ -n "$TRIMPOD_SPK_SWITCH" ] && amixer -q sset 'SPK' "$TRIMPOD_SPK_SWITCH" 2>/dev/null
  [ -n "$TRIMPOD_LINEOUT_SWITCH" ] && amixer -q sset 'LINEOUT' "$TRIMPOD_LINEOUT_SWITCH" 2>/dev/null
  [ -n "$TRIMPOD_OUTL_SWITCH" ] && amixer -q sset 'OutputL Mixer DACL' "$TRIMPOD_OUTL_SWITCH" 2>/dev/null
  [ -n "$TRIMPOD_OUTR_SWITCH" ] && amixer -q sset 'OutputR Mixer DACR' "$TRIMPOD_OUTR_SWITCH" 2>/dev/null
  [ -n "$TRIMPOD_SPK_MUTE" ] && echo "$TRIMPOD_SPK_MUTE" > "$SPK_MUTE" 2>/dev/null
  if [ -d "$CPUP" ] && [ -n "$TRIMPOD_OLD_GOV" ]; then
    safe_min="$(cat "$CPUP/cpuinfo_min_freq" 2>/dev/null)"
    [ -n "$safe_min" ] && echo "$safe_min" > "$CPUP/scaling_min_freq"
    echo "$TRIMPOD_OLD_MAX" > "$CPUP/scaling_max_freq"
    echo "$TRIMPOD_OLD_MIN" > "$CPUP/scaling_min_freq"
    echo "$TRIMPOD_OLD_GOV" > "$CPUP/scaling_governor"
  fi
  # Re-enable the battery charger (AXP2202 reg 0x19 bit 1) on exit, in case the
  # in-app Charge Limit had disabled it -- guaranteed even on SIGKILL of the
  # binary, which the in-app code can't catch.  Skipped when the standalone
  # Battery Care daemon is running, since it owns the bit (the two never fight).
  BC_REGS="${BATTERY_REGS:-/sys/kernel/debug/regmap/6-0034/registers}"
  BC_PID=$(cat /tmp/battery-care-daemon.pid 2>/dev/null)
  if [ -f "$BC_REGS" ] && ! { [ -n "$BC_PID" ] && tr -d '\0' < "/proc/$BC_PID/cmdline" 2>/dev/null | grep -q battery-care-daemon; }; then
    bcval=$(grep '^19:' "$BC_REGS" 2>/dev/null | awk '{print $2}')
    [ -n "$bcval" ] && printf '19 %02x\n' "$(( 0x$bcval | 2 ))" > "$BC_REGS" 2>/dev/null
  fi
  while mount 2>/dev/null | grep -q "$RBDIR_BIND"; do umount -l "$RBDIR_BIND" 2>/dev/null; done
  rmdir "$RBDIR_BIND" 2>/dev/null   # drop the empty mountpoint dir, not just the mount
}
trap cleanup EXIT
trap 'exit' INT TERM HUP

"$RBDIR/trimpod" --zoom "$ZOOMVAL" > "$PAK_DIR/trimpod.log" 2>&1
