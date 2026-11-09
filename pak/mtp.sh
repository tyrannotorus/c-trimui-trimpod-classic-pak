#!/bin/sh
# USB file transfer: add umtprd (MTP: the SD card) beside adb on the stock
# gadget, or take it away again.  Trimpod runs "start"/"stop" on the USB power
# edge (power-target.c), so umtprd never runs on battery; launch.sh runs "stop"
# on exit.  Overlapping edges queue on the lock.
G="$MTP_GADGET"
BIN="$(dirname "$0")/trimpod"
FFS=/dev/usb-ffs/mtp
PIDF=/tmp/trimpod-umtprd.pid
[ -n "$G" ] && [ -x "$BIN/umtprd" ] || exit 0
exec 9> /tmp/trimpod-mtp.lock
flock 9

stop() {
  [ -f "$PIDF" ] || return 0
  /bin/setusbconfig adb > /dev/null 2>&1
  pid=$(cat "$PIDF")
  kill "$pid" 2>/dev/null
  i=0
  while kill -0 "$pid" 2>/dev/null && [ $i -lt 20 ]; do sleep 0.1; i=$((i + 1)); done
  umount "$FFS" 2>/dev/null
  rmdir "$FFS" "$G/functions/ffs.mtp" 2>/dev/null
  rm -f "$PIDF"
}

start() {
  [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null && return 0
  stop                                    # clear any leftovers of a dead run
  [ -e /dev/usb-ffs/adb/ep1 ] || return 0 # adbd not up: leave USB alone
  # Unlinking a function is what unbinds the UDC on this kernel (an empty UDC
  # write is ignored), so relink both, then bind once umtprd has written its
  # descriptors.
  for l in "$G"/configs/c.1/*; do [ -L "$l" ] && rm -f "$l"; done
  echo "Trimpod Classic" > "$G/strings/0x409/product"
  mkdir -p "$G/functions/ffs.mtp" "$FFS"
  mount -t functionfs mtp "$FFS"
  ln -s "$G/functions/ffs.mtp" "$G/configs/c.1/ffs.mtp"
  ln -s "$G/functions/ffs.adb" "$G/configs/c.1/ffs.adb"
  "$BIN/umtprd" -conf "$BIN/umtprd.conf" > /tmp/umtprd.log 2>&1 9>&- &
  echo $! > "$PIDF"
  i=0
  while [ ! -e "$FFS/ep1" ] && [ $i -lt 20 ]; do sleep 0.1; i=$((i + 1)); done
  if [ -e "$FFS/ep1" ]; then
    ls /sys/class/udc | head -n 1 > "$G/UDC"
  else
    stop                                  # umtprd never came up: keep adb
  fi
}

case "$1" in
  start) start ;;
  stop) stop ;;
esac
