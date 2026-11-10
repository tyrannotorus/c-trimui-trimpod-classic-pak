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


#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include "button.h"
#include "button-target.h"

/* Hold/lock switch (Brick side toggle = gpio243, value 1 = engaged, same line
 * NextUI reads). Engaged -> button_read_device() passes only the volume rocker. */
#define HOLD_SWITCH_PATH "/sys/class/gpio/gpio243/value"

bool retrohh_hold_switch(void)
{
    static int fd = -2;            /* -2 = not opened, -1 = unavailable */
    if (fd == -2)
        fd = open(HOLD_SWITCH_PATH, O_RDONLY);
    if (fd < 0)
        return false;

    char v = '0';
    if (pread(fd, &v, 1, 0) < 1)   /* sysfs: re-read from offset 0 each poll */
        return false;
    return v == '1';               /* 1 = engaged -> lock input */
}

/* Gamepad -> Rockbox button mapping.
 *
 * We read the TrimUI's gamepad through SDL's joystick layer directly (the pad
 * is opened with SDL_JoystickOpen in button-sdl.c), exactly like NextUI does --
 * no gptokeyb2 shim.  The raw SDL joystick button indices below are the Brick's
 * (matching NextUI's JOY_* constants, workspace/tg5040/platform/platform.h), so
 * the physical buttons land where the launcher puts them.  The volume rocker is
 * NOT a separate device: SDL enumerates the pad node's KEY_VOLUMEUP/DOWN as
 * joystick buttons 14/13, so the rocker arrives here for free.  Indices 8
 * (MENU) and 9/10 (L3/R3) are unused by Trimpod. */
int joybutton_to_button(int joybtn)
{
    switch (joybtn)
    {
        case 1:  return BUTTON_A;        /* face buttons (NextUI swaps A/B, X/Y) */
        case 0:  return BUTTON_B;
        case 3:  return BUTTON_X;
        case 2:  return BUTTON_Y;
        case 4:  return BUTTON_L;        /* L1 */
        case 5:  return BUTTON_R;        /* R1 */
        case 6:  return BUTTON_SELECT;
        case 7:  return BUTTON_START;
        case 14: return BUTTON_VOL_UP;   /* volume rocker, on the pad node */
        case 13: return BUTTON_VOL_DOWN;
        default: return BUTTON_NONE;     /* 8 = MENU, 9/10 = L3/R3 (unused) */
    }
}

/* L2/R2 are reported as analog axes (ABS_Z / ABS_RZ = axes 2/5 on tg5040), not
 * buttons -- same as NextUI's AXIS_L2/AXIS_R2. */
int joyaxis_to_button(int axis)
{
    switch (axis)
    {
        case 2:  return BUTTON_L2;
        case 5:  return BUTTON_R2;
        default: return BUTTON_NONE;
    }
}
