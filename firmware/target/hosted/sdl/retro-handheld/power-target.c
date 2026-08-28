/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 Hairo R. Carela
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "system.h"
#include "power-target.h"
#include "power.h"
#include "powermgmt.h"
#include "panic.h"
#include "sysfs.h"

#include "tick.h"
#include "kernel.h"      /* sleep(), HZ                 */
#include "thread.h"      /* create_thread() for the charge-limit poll */

#ifdef HAVE_TRIMUI_SAFE_POWEROFF
/* The NextUI/MinUI launcher runs poweroff_next (safe AXP2202 PMIC shutdown:
 * kill SD users -> sync -> unmount -> PMIC off) when it finds this flag after
 * a pak exits. So just drop the flag; the real poweroff happens once we quit. */
void trimui_safe_poweroff(void)
{
    int fd = open("/tmp/poweroff", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0)
        close(fd);
}
#endif

#ifdef HAVE_TRIMUI_DEEP_SLEEP
/* Run NextUI's own `suspend` helper ($SYSTEM_PATH/bin/suspend): stops wifi/bt,
 * `echo mem > /sys/power/state`, restores on resume. Blocks until the power
 * button (the only enabled wake source) wakes the device. */
void trimui_deep_sleep(void)
{
    const char *sys = getenv("SYSTEM_PATH");
    char cmd[256];
    if (!sys || !*sys)
#ifdef TRIMPOD_H700
        sys = "/mnt/SDCARD/.system/h700";
#else
        sys = "/mnt/SDCARD/.system/tg5040";   /* Brick default if env is bare */
#endif
    snprintf(cmd, sizeof cmd, "'%s/bin/suspend'", sys);
    system(cmd);
}
#endif

/* We get called multiple times per tick, let's cut that back! */
static long last_tick = 0;
static bool last_power = false;

static char* get_path(const char *env_key)
{
    char *p = getenv(env_key);
    return (p && *p) ? p : NULL;
}

bool charging_state(void)
{
    if ((current_tick - last_tick) > HZ/2 ) {
        char buf[12] = {0};
        
        char *path = get_path("BATTERY_STATUS");

        if (path)
            sysfs_get_string(path, buf, sizeof(buf));

        last_tick = current_tick;
        last_power = (strncmp(buf, "Charging", 8) == 0);
    }
    return last_power;
}

unsigned int power_input_status(void)
{
    int present = 0;
    
    char *path = get_path("POWER_STATUS");

    if (path)
        sysfs_get_int(path, &present);

    return present ? POWER_INPUT_USB_CHARGER : POWER_INPUT_NONE;
}

unsigned int power_get_battery_capacity(void)
{
    int battery_level;

    char *path = get_path("CAPACITY_STATUS");

    if (path)
        sysfs_get_int(path, &battery_level);

    return battery_level;
}

/* Trimpod: platform-specific CPU frequency control.
 * Pins the policy to a single frequency via scaling_min/max_freq. This is a
 * runtime-only change -- launch.sh saves the system cpufreq state and restores
 * it when Trimpod exits, so it never affects the device permanently. */
#define CPUFREQ_POLICY "/sys/devices/system/cpu/cpufreq/policy0"
#define CPUFREQ_INT    "/sys/devices/system/cpu/cpufreq/interactive"
#define CPU_FREQ_FILE  ROCKBOX_DIR "/cpu_freq.txt"
#ifdef TRIMPOD_H700
#define CPU_FREQ_MIN_KHZ  480000
#define CPU_FREQ_MAX_KHZ 1512000
static const int cpu_freqs[] = {
    480000, 720000, 936000, 1008000, 1104000,
    1200000, 1320000, 1416000, 1512000
};
#else
#define CPU_FREQ_MIN_KHZ  408000   /* idle floor for Dynamic */
#define CPU_FREQ_MAX_KHZ  2000000  /* A133 top step */
static const int cpu_freqs[] = {
    408000, 600000, 816000, 1008000, 1200000,
    1416000, 1608000, 1800000, 2000000
};
#endif

#define CPU_FREQ_COUNT ((int)(sizeof(cpu_freqs) / sizeof(cpu_freqs[0])))

int retrohh_cpu_freq_count(void)
{
    return CPU_FREQ_COUNT;
}

int retrohh_cpu_freq_at(int index)
{
    return index >= 0 && index < CPU_FREQ_COUNT ? cpu_freqs[index] : 0;
}

/* This file is the SINGLE owner of the CPU Frequency policy (Settings -> Power
 * -> CPU): the two modes are defined here once, persisted here, and applied here
 * both at startup (retrohh_cpu_apply_saved) and on a live menu change. launch.sh
 * only saves the system's original cpufreq state on entry and restores it on
 * exit -- it sets no governor/frequency of its own. */

/* "Pinned": LOCK the cores at a single fixed frequency by collapsing the scaling
 * window to one value (scaling_min == scaling_max == khz). The `performance`
 * governor then simply holds scaling_max_freq -- which is khz, not 2 GHz -- so the
 * clock is fixed at the chosen step (no scaling up or down). */
void retrohh_cpu_set_freq(int khz)
{
    if (khz <= 0)
        return;
    char gov[] = "performance";          /* holds scaling_max_freq, = khz here */
    sysfs_set_string(CPUFREQ_POLICY "/scaling_governor", gov);
    /* lower the floor first so min<=max always holds, then pin both to khz */
    sysfs_set_int(CPUFREQ_POLICY "/scaling_min_freq", CPU_FREQ_MIN_KHZ);
    sysfs_set_int(CPUFREQ_POLICY "/scaling_max_freq", khz);
    sysfs_set_int(CPUFREQ_POLICY "/scaling_min_freq", khz);
}

/* Trimpod "Dynamic": the tuned `interactive` governor across the full range --
 * idles at 408 MHz (cool) and ramps to 2 GHz only under sustained load. The
 * interactive tunables are global and reset to defaults whenever the governor is
 * (re)selected, so they are re-applied here every time: hispeed_freq caps the
 * load-jump and above_hispeed_delay slows the climb, so 30fps peak-meter bursts
 * don't spike to 2 GHz while the visualizer/decode still ramp. */
void retrohh_cpu_set_dynamic(void)
{
#ifdef TRIMPOD_H700
    const char *sys = getenv("SYSTEM_PATH");
    char cmd[256];
    if (sys && *sys)
    {
        snprintf(cmd, sizeof cmd, "'%s/bin/governor.sh' auto", sys);
        if (system(cmd) == 0)
            return;
    }
    char gov[] = "schedutil";
    sysfs_set_int(CPUFREQ_POLICY "/scaling_min_freq", CPU_FREQ_MIN_KHZ);
    sysfs_set_int(CPUFREQ_POLICY "/scaling_max_freq", CPU_FREQ_MAX_KHZ);
    sysfs_set_string(CPUFREQ_POLICY "/scaling_governor", gov);
#else
    char gov[] = "interactive";
    sysfs_set_int(CPUFREQ_POLICY "/scaling_min_freq", CPU_FREQ_MIN_KHZ);
    sysfs_set_int(CPUFREQ_POLICY "/scaling_max_freq", CPU_FREQ_MAX_KHZ);
    sysfs_set_string(CPUFREQ_POLICY "/scaling_governor", gov);
    /* tunables live under the governor's dir, present only once it is active */
    sysfs_set_int(CPUFREQ_INT "/target_loads", 95);
    sysfs_set_int(CPUFREQ_INT "/hispeed_freq", 1008000);
    sysfs_set_int(CPUFREQ_INT "/above_hispeed_delay", 20000);
    sysfs_set_int(CPUFREQ_INT "/timer_rate", 30000);
    sysfs_set_int(CPUFREQ_INT "/min_sample_time", 100000);
#endif
}

/* Persist the menu choice; read back by retrohh_cpu_apply_saved at next startup.
 * khz <= 0 -> "dynamic"; otherwise the pinned step in kHz. */
void retrohh_cpu_save_choice(int khz)
{
    int fd = open(CPU_FREQ_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    char buf[16];
    int n = (khz <= 0) ? snprintf(buf, sizeof buf, "dynamic\n")
                       : snprintf(buf, sizeof buf, "%d\n", khz);
    if (n > 0)
        write(fd, buf, n);
    close(fd);
}

/* Apply the persisted CPU Frequency choice. Called once at app startup; the menu
 * applies live on change. Default (no saved file) is Dynamic. */
void retrohh_cpu_apply_saved(void)
{
    char buf[16] = {0};
    int khz = 0;                         /* 0 / "dynamic" -> Dynamic */
    int fd = open(CPU_FREQ_FILE, O_RDONLY);
    if (fd >= 0)
    {
        int r = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (r > 0)
            khz = atoi(buf);             /* "dynamic" -> 0 */
    }
    if (khz > 0)
        retrohh_cpu_set_freq(khz);
    else
        retrohh_cpu_set_dynamic();
}

/* True when the CPU is in Dynamic mode (a scaling governor); pinned mode uses
 * "performance". */
bool retrohh_cpu_is_dynamic(void)
{
    char gov[24] = {0};
    if (!sysfs_get_string(CPUFREQ_POLICY "/scaling_governor", gov, sizeof gov))
        return false;
    return gov[0] && strncmp(gov, "performance", 11) != 0;
}

int retrohh_cpu_get_freq(void)
{
    int khz = 0;
    sysfs_get_int(CPUFREQ_POLICY "/scaling_cur_freq", &khz);
    return khz;
}

/* ---- Trimpod: Charge Limit (Settings -> Power) --------------------------- *
 * Caps charging at a target % by toggling the AXP2202 charger-enable bit
 * (regmap reg 0x19 bit 1) -- the same mechanism as the standalone Battery Care
 * app, but in-process (no daemon) and only while Trimpod runs. A 60s poll
 * thread does the read-modify-write with 5% hysteresis. launch.sh re-enables
 * the charger on exit (unless the daemon is running) so quitting never leaves
 * charging capped -- even on SIGKILL, which the in-app path can't catch.
 *
 * If the standalone Battery Care daemon is already running we DEFER entirely:
 * the poll thread is never started and the menu row is read-only (showing the
 * daemon's target, else 100%).  Only one of the two ever writes the bit.
 *
 * Every fs access is checked and the register is only ever written from a value
 * derived from a *successful* read, so any failure is a true no-op. */
#ifdef TRIMPOD_H700
#define BATT_REGS         "/sys/kernel/debug/regmap/5-0034/registers"
#else
#define BATT_REGS         "/sys/kernel/debug/regmap/6-0034/registers"
#endif
#define BATT_CAP_FILE     "/sys/class/power_supply/axp2202-battery/capacity"
#define BATT_USB_FILE     "/sys/class/power_supply/axp2202-usb/online"
#define BATT_TARGET_FILE  ROCKBOX_DIR "/charge_limit.txt"
#define BATT_DAEMON_PID   "/tmp/battery-care-daemon.pid"
#define BATT_CHARGER_BIT  0x02   /* reg 0x19 bit 1 = charger enable */
#define BATT_HYST         5      /* re-enable when cap <= target - HYST */
#define BATT_OFF          100    /* target == 100 -> no cap (shown "100%") */
#define BATT_POLL_SECONDS 60
/* Step/max/default/Off-label/wrap mirror the standalone Battery Care app
 * (pak/src/settings.c); the MINIMUM is 75 here (Trimpod-specific) vs the app's
 * 50, so the in-app range is 75..100 in steps of 5, default 100 = Off. */
#define BATT_MIN          75
#define BATT_STEP         5

/* Clamp to [BATT_MIN, BATT_OFF] and snap to the step grid (mirrors the Battery
 * Care app's clamp_step). */
static int batt_clamp_step(int v)
{
    if (v < BATT_MIN) v = BATT_MIN;
    if (v > BATT_OFF) v = BATT_OFF;
    return ((v + BATT_STEP / 2) / BATT_STEP) * BATT_STEP;
}

static bool batt_available;          /* probe ok and not deferred to the daemon */
static bool batt_locked;             /* daemon present -> hide the Charge Limit row */
static int  batt_target = BATT_OFF;

/* Detect the standalone Battery Care daemon by name: its pidfile points at a
 * live process whose cmdline contains "battery-care-daemon" (mirrors the app's
 * own is_our_daemon check; a stale pidfile fails the cmdline test). */
static bool batt_daemon_running(void)
{
    FILE *f = fopen(BATT_DAEMON_PID, "re");
    if (!f)
        return false;
    int pid = -1;
    if (fscanf(f, "%d", &pid) != 1)
        pid = -1;
    fclose(f);
    if (pid <= 0)
        return false;

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    f = fopen(path, "re");
    if (!f)
        return false;                /* pid not alive -> stale pidfile */
    char buf[256];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; i++)
        if (buf[i] == '\0') buf[i] = ' ';
    buf[n] = '\0';
    return strstr(buf, "battery-care-daemon") != NULL;
}

/* reg 0x19 as an int, or -1 on any failure (so callers never write a value
 * derived from a bad read). */
static int batt_read_reg19(void)
{
    FILE *f = fopen(BATT_REGS, "re");
    if (!f)
        return -1;
    char line[64];
    int v = -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "19:", 3) == 0) { v = (int)strtol(line + 3, NULL, 16); break; }
    fclose(f);
    return v;
}

static void batt_write_reg19(int v)
{
    FILE *f = fopen(BATT_REGS, "we");
    if (!f)
        return;
    fprintf(f, "19 %02x\n", v & 0xff);
    fclose(f);
}

/* Always safe to call: ensure the charger bit is set (charging enabled). */
static void batt_restore_charger(void)
{
    int cur = batt_read_reg19();
    if (cur >= 0 && !(cur & BATT_CHARGER_BIT))
        batt_write_reg19(cur | BATT_CHARGER_BIT);
}

static void batt_save_target(void)
{
    FILE *f = fopen(BATT_TARGET_FILE, "we");
    if (!f)
        return;
    fprintf(f, "%d\n", batt_target);
    fclose(f);
}

static void batt_load_target(void)
{
    batt_target = BATT_OFF;          /* default: no cap */
    FILE *f = fopen(BATT_TARGET_FILE, "re");
    if (!f)
        return;
    int t = 0;
    if (fscanf(f, "%d", &t) == 1)
        batt_target = batt_clamp_step(t);
    fclose(f);
}

/* One poll tick: cap/uncap with hysteresis. No-op unless actively capping and
 * on USB power; each read is checked so a failure writes nothing. */
static void batt_care_tick(void)
{
    if (!batt_available || batt_target >= BATT_OFF)
        return;
    int usb = 0;
    if (!sysfs_get_int(BATT_USB_FILE, &usb) || usb != 1)
        return;                      /* only manage while on USB power */
    int cap = 0;
    if (!sysfs_get_int(BATT_CAP_FILE, &cap))
        return;
    int cur = batt_read_reg19();
    if (cur < 0)
        return;
    bool on = (cur & BATT_CHARGER_BIT) != 0;
    if (cap >= batt_target && on)
        batt_write_reg19(cur & ~BATT_CHARGER_BIT);   /* reached cap: stop charging */
    else if (cap <= batt_target - BATT_HYST && !on)
        batt_write_reg19(cur | BATT_CHARGER_BIT);    /* dropped below: resume      */
}

static long batt_care_stack[DEFAULT_STACK_SIZE / sizeof(long)];
static const char batt_care_thread_name[] = "battery care";

static void batt_care_thread(void)
{
    while (1)
    {
        sleep(HZ * BATT_POLL_SECONDS);
        batt_care_tick();
    }
}

/* Called once at startup (apps/main.c, RETRO_HANDHELD). Decides ownership for
 * the whole session: defer to the daemon if present, else probe the hardware
 * and -- only if usable -- start the poll thread. */
void retrohh_charge_limit_init(void)
{
    batt_load_target();

    if (batt_daemon_running())
    {
        batt_locked = true;          /* daemon owns charging -> hide our row */
        batt_available = false;
        return;                      /* never start the loop -- daemon owns it */
    }

    /* Need a root-writable regmap + readable capacity, or the feature can't run. */
    if (access(BATT_REGS, W_OK) != 0 || access(BATT_CAP_FILE, R_OK) != 0)
    {
        batt_available = false;
        return;
    }
    batt_available = true;

    /* Self-heal: start from charging-enabled (clears any "off" left stranded by a
     * prior hard force-off / crash that skipped launch.sh), then re-apply the
     * saved cap on top.  So simply relaunching Trimpod fixes a stranded charger. */
    batt_restore_charger();
    if (batt_target < BATT_OFF)
        batt_care_tick();            /* re-apply the saved cap immediately */

    create_thread(batt_care_thread, batt_care_stack, sizeof batt_care_stack,
                  0, batt_care_thread_name
                  IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
}

/* Menu hooks (power_menu.c). The row is hidden entirely when the daemon owns
 * charging or the hardware isn't usable. */
bool retrohh_charge_limit_hidden(void)
{
    return batt_locked || !batt_available;
}

int retrohh_charge_limit_get_target(void)
{
    return batt_target;
}

void retrohh_charge_limit_cycle(int dir)
{
    if (retrohh_charge_limit_hidden())
        return;                      /* defensive: row isn't shown when hidden */
    /* Step by BATT_STEP and wrap 100<->75, matching the Battery Care app's cycle. */
    int v = batt_target + ((dir < 0) ? -BATT_STEP : BATT_STEP);
    if (v < BATT_MIN) v = BATT_OFF;
    if (v > BATT_OFF) v = BATT_MIN;
    batt_target = v;
    batt_save_target();
    if (batt_target >= BATT_OFF)
        batt_restore_charger();      /* Off -> resume charging now */
    else
        batt_care_tick();            /* apply the new cap immediately */
}
