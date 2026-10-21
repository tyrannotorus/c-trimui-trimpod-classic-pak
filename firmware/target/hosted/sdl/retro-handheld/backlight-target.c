/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2019 by Roman Stolyarov
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
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "lcd.h"                /* lcd_set_panel_dark: stop presenting while dark */
#include "backlight-target.h"
#include "sysfs.h"
#include "panic.h"
#include "lcd.h"

typedef enum {
  BL_TYPE1 = 0,
  BL_TYPE2 = 1,
} backlight_t;

static char* get_bl_env(const char *env_key)
{
    char *p = getenv(env_key);
    return (p && *p) ? p : NULL;
}

static void set_type1_brightness(int value) {
  sysfs_set_string(get_bl_env("SYSFS_BL_COMMAND"), get_bl_env("BL_COMMAND"));
  sysfs_set_string(get_bl_env("SYSFS_BL_NAME"), get_bl_env("BL_NAME"));
  sysfs_set_int(get_bl_env("SYSFS_BL_PARAM"), value);
  sysfs_set_int(get_bl_env("SYSFS_BL_START"), 1);
}

/* LCD_ON/OFF_COMMAND (set per device in its .sys) power the panel itself:
 * off stops the 60Hz scanout of a dark screen, on re-inits it (~300ms). */
static void set_type1_lcd(const char *cmd_env) {
  char *cmd = get_bl_env(cmd_env);
  if (!cmd)
    return;
  sysfs_set_string(get_bl_env("SYSFS_BL_COMMAND"), cmd);
  sysfs_set_string(get_bl_env("SYSFS_BL_NAME"), get_bl_env("BL_NAME"));
  sysfs_set_int(get_bl_env("SYSFS_BL_START"), 1);
}

static void set_type2_brightness(int value) {
  sysfs_set_int(get_bl_env("SYSFS_BL_BRIGHTNESS"), value);
}

static int get_type2_bl_step(void) {
  int max = 0;

  if (!sysfs_get_int(get_bl_env("SYSFS_BL_MAX"), &max)) {
    return 0;
  }

  return max / 17;
}

static backlight_t get_type(void)
{
    char *bl_type = getenv("BL_TYPE");
    if (!bl_type) return BL_TYPE1;

    if (strcmp(bl_type, "TYPE1") == 0) return BL_TYPE1;
    if (strcmp(bl_type, "TYPE2") == 0) return BL_TYPE2;

    return BL_TYPE1;
}

bool backlight_hw_init(void)
{
    backlight_hw_on();
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
    return true;
}

static int last_bl = -1;

void backlight_hw_on(void)
{
    if (last_bl != 1) {

    /* Panel on first: the present below waits on its vsync. */
    if (get_type() == BL_TYPE1)
        set_type1_lcd("LCD_ON_COMMAND");

    /* Present BEFORE lighting the panel: the surfaces were kept current while
     * dark, so the panel comes up already showing them.  Lighting first would
     * flash the frame from when it went dark until this present landed. */
    lcd_set_panel_dark(false);

    switch (get_type()) {
        case BL_TYPE1: set_type1_brightness(15); break;
        case BL_TYPE2: sysfs_set_int(get_bl_env("SYSFS_BL_POWER"), 0); break;
    }

	  last_bl = 1;
    }
}

void backlight_hw_off(void)
{
    if (last_bl != 0) {

    switch (get_type()) {
        case BL_TYPE1: set_type1_brightness(0); break;
        case BL_TYPE2: sysfs_set_int(get_bl_env("SYSFS_BL_POWER"), 1); break;
    }

	last_bl = 0;
    lcd_set_panel_dark(true);    /* dark: stop presenting to a panel that is off */
    if (get_type() == BL_TYPE1)
        set_type1_lcd("LCD_OFF_COMMAND");
    }
}

void backlight_hw_brightness(int brightness)
{
    /* cap range, just in case */
    if (brightness > MAX_BRIGHTNESS_SETTING)
        brightness = MAX_BRIGHTNESS_SETTING;
    if (brightness < MIN_BRIGHTNESS_SETTING)
        brightness = MIN_BRIGHTNESS_SETTING;

    switch (get_type()) {
          case BL_TYPE1: set_type1_brightness(brightness * 15); break;        
          case BL_TYPE2: set_type2_brightness(brightness * get_type2_bl_step()); break;
    }

}
