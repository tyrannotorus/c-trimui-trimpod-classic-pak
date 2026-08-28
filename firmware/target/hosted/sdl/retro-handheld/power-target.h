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
#ifndef _POWER_TARGET_H_
#define _POWER_TARGET_H_

#include <stdbool.h>
#include "config.h"

unsigned int power_get_battery_capacity(void);
/* Trimpod CPU Frequency (Settings -> Power -> CPU). power-target.c is the single
 * owner: defines, persists (cpu_freq.txt) and applies the choice. launch.sh only
 * saves/restores the system's original cpufreq state. */
void retrohh_cpu_set_freq(int khz);   /* pin CPU to khz (performance) */
void retrohh_cpu_set_dynamic(void);   /* Dynamic: tuned interactive, full range */
int  retrohh_cpu_get_freq(void);      /* current scaling_cur_freq in khz */
bool retrohh_cpu_is_dynamic(void);    /* true unless pinned (performance) */
int  retrohh_cpu_freq_count(void);    /* supported fixed steps for this build */
int  retrohh_cpu_freq_at(int index);  /* supported fixed step in kHz */
void retrohh_cpu_save_choice(int khz);/* persist choice (khz<=0 -> dynamic) */
void retrohh_cpu_apply_saved(void);   /* apply persisted choice (call at startup) */

/* Trimpod Charge Limit (Settings -> Power). Caps charging at a target % while
 * Trimpod runs, by toggling the AXP2202 charger bit -- in-process, no daemon.
 * Defers entirely (read-only row, loop never started) if the standalone Battery
 * Care daemon is already running. launch.sh re-enables charging on exit. */
void retrohh_charge_limit_init(void);     /* startup: probe + maybe start the loop */
bool retrohh_charge_limit_hidden(void);   /* true -> hide the menu row entirely     */
int  retrohh_charge_limit_get_target(void); /* current cap % (100 = Off)           */
void retrohh_charge_limit_cycle(int dir); /* step the cap                           */
#endif /* _POWER_RHH_H_ */
