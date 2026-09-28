#!/bin/sh
# In-app Power Down on tg5040.  MinUI's safe shutdown (skeleton/SYSTEM/tg5040/bin/
# shutdown, from Helaas/nextui-brick-poweroff-hook): sync, lazy-unmount the SD
# card, then the AXP2202 sequence through the kernel's /sys/class/axp/axp_reg --
# mask IRQs (0x40-0x44), clear IRQ status (0x48-0x4C, write-1-to-clear),
# PWROFF_EN 0x22=0x0A, software power-off 0x27=0x01.  No process sweep: NextUI's
# poweroff_next kills init's children first, init exits, the kernel panics
# ("Attempted to kill init") and kernel.panic=3 REBOOTS the Brick before its own
# PMIC writes ever run.  Runs from a /tmp copy so the SD card can go away under it.
case "$0" in
  /tmp/*) ;;
  *) cp "$0" /tmp/trimpod-shutdown.sh && exec sh /tmp/trimpod-shutdown.sh ;;
esac
AXP=/sys/class/axp/axp_reg
[ -w "$AXP" ] || exit 1
sync
umount -f -l /mnt/SDCARD
for REG in 0x40 0x41 0x42 0x43 0x44; do echo "${REG}00" > "$AXP"; done
for REG in 0x48 0x49 0x4A 0x4B 0x4C; do echo "${REG}FF" > "$AXP"; done
echo 0x220A > "$AXP"
sleep 0.05
echo 0x2701 > "$AXP"
sleep 1
