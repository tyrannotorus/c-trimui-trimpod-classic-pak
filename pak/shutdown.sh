#!/bin/sh
# tg5040 power-off via AXP2202 PMIC (same register sequence as NextUI's poweroff_next).
# No process kill: killing init's children panics the kernel and reboots.
# Re-execs from /tmp so the SD card can be unmounted under it.
case "$0" in
  /tmp/*) ;;
  *) cp "$0" /tmp/trimpod-shutdown.sh && exec sh /tmp/trimpod-shutdown.sh ;;
esac
AXP=/sys/class/axp/axp_reg
[ -w "$AXP" ] || exit 1
sync
umount -f -l /mnt/SDCARD
# mask IRQs, clear IRQ status, PWROFF_EN, power off
for REG in 0x40 0x41 0x42 0x43 0x44; do echo "${REG}00" > "$AXP"; done
for REG in 0x48 0x49 0x4A 0x4B 0x4C; do echo "${REG}FF" > "$AXP"; done
echo 0x220A > "$AXP"
sleep 0.05
echo 0x2701 > "$AXP"
sleep 1
