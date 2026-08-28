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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <SDL.h>
#include "button.h"
#include "button-target.h"
#include "kernel.h"

/* H700 built-in input ----------------------------------------------------- */

#define H700_INPUT_COUNT 12
#define H700_EV_KEY      0x01
#define H700_EV_ABS      0x03
#define H700_ABS_HATX    16
#define H700_ABS_HATY    17
#define H700_ABS_LSX      2
#define H700_ABS_LSY      3
#define H700_AXIS_DEADZONE 1024

struct h700_input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

static int h700_input_fds[H700_INPUT_COUNT];
static long h700_last_scan;
static int h700_keys;
static int h700_hat;
static int h700_stick;
static bool h700_power_held;

bool retrohh_h700_active(void)
{
    const char *platform = getenv("PLATFORM");
    return platform && strcmp(platform, "h700") == 0;
}

bool retrohh_should_open_joystick(const char *name)
{
    return !(retrohh_h700_active() && name &&
             strcmp(name, "ANBERNIC-keys") == 0);
}

static bool h700_input_name_allowed(const char *name)
{
    return strcmp(name, "axp2202-pek") == 0 ||
           strcmp(name, "ANBERNIC-keys") == 0 ||
           strcmp(name, "dierct-keys-polled") == 0;
}

static void h700_close_input(int index)
{
    if (index >= 0 && index < H700_INPUT_COUNT && h700_input_fds[index] >= 0)
    {
        close(h700_input_fds[index]);
        h700_input_fds[index] = -1;
    }
}

static void h700_open_input(int index)
{
    char path[64];
    char name[128] = {0};
    int fd;

    snprintf(path, sizeof path, "/sys/class/input/event%d/device/name", index);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    ssize_t n = read(fd, name, sizeof name - 1);
    close(fd);
    if (n <= 0)
        return;
    while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r'))
        name[--n] = '\0';
    if (!h700_input_name_allowed(name))
        return;

    snprintf(path, sizeof path, "/dev/input/event%d", index);
    h700_input_fds[index] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

static void h700_scan_inputs(void)
{
    if (h700_last_scan && current_tick - h700_last_scan < 2 * HZ)
        return;
    h700_last_scan = current_tick ? current_tick : 1;

    for (int i = 0; i < H700_INPUT_COUNT; i++)
    {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if (h700_input_fds[i] < 0 && access(path, R_OK) == 0)
            h700_open_input(i);
        else if (h700_input_fds[i] >= 0 && access(path, R_OK) != 0)
            h700_close_input(i);
    }
}

void retrohh_evdev_init(void)
{
    for (int i = 0; i < H700_INPUT_COUNT; i++)
        h700_input_fds[i] = -1;
    h700_last_scan = 0;
    h700_keys = h700_hat = h700_stick = BUTTON_NONE;
    h700_power_held = false;
    if (retrohh_h700_active())
        h700_scan_inputs();
}

static int h700_button_from_code(int code)
{
    switch (code)
    {
        case 103: return BUTTON_UP;
        case 108: return BUTTON_DOWN;
        case 105: return BUTTON_LEFT;
        case 106: return BUTTON_RIGHT;
        case 310: return BUTTON_SELECT;
        case 311: return BUTTON_START;
        /* Xbox semantics by position: bottom confirms, right goes back. */
        case 304: return BUTTON_A;
        case 305: return BUTTON_B;
        case 307: return BUTTON_X;
        case 306: return BUTTON_Y;
        case 308: return BUTTON_L;
        case 309: return BUTTON_R;
        case 314: return BUTTON_L2;
        case 315: return BUTTON_R2;
        case 312: return BUTTON_MENU;
        case 115: return BUTTON_VOL_UP;
        case 114: return BUTTON_VOL_DOWN;
        /* 354 is a synthetic post-tap MENU pulse and must be ignored. */
        default:  return BUTTON_NONE;
    }
}

static void h700_axis_update(int code, int value)
{
    if (code == H700_ABS_HATX)
    {
        h700_hat &= ~(BUTTON_LEFT | BUTTON_RIGHT);
        if (value < 0) h700_hat |= BUTTON_LEFT;
        if (value > 0) h700_hat |= BUTTON_RIGHT;
    }
    else if (code == H700_ABS_HATY)
    {
        h700_hat &= ~(BUTTON_UP | BUTTON_DOWN);
        if (value < 0) h700_hat |= BUTTON_UP;
        if (value > 0) h700_hat |= BUTTON_DOWN;
    }
    else if (code == H700_ABS_LSX)
    {
        h700_stick &= ~(BUTTON_LEFT | BUTTON_RIGHT);
        if (value < -H700_AXIS_DEADZONE) h700_stick |= BUTTON_LEFT;
        if (value > H700_AXIS_DEADZONE) h700_stick |= BUTTON_RIGHT;
    }
    else if (code == H700_ABS_LSY)
    {
        h700_stick &= ~(BUTTON_UP | BUTTON_DOWN);
        if (value < -H700_AXIS_DEADZONE) h700_stick |= BUTTON_UP;
        if (value > H700_AXIS_DEADZONE) h700_stick |= BUTTON_DOWN;
    }
}

int retrohh_evdev_buttons(bool *power_held)
{
    struct h700_input_event event;

    if (!retrohh_h700_active())
    {
        if (power_held) *power_held = false;
        return BUTTON_NONE;
    }

    h700_scan_inputs();
    for (int i = 0; i < H700_INPUT_COUNT; i++)
    {
        if (h700_input_fds[i] < 0)
            continue;
        errno = 0;
        while (read(h700_input_fds[i], &event, sizeof event) == sizeof event)
        {
            if (event.type == H700_EV_KEY)
            {
                if (event.value > 1)
                    continue;
                if (event.code == 116)
                    h700_power_held = event.value != 0;
                else
                {
                    int button = h700_button_from_code(event.code);
                    if (event.value)
                        h700_keys |= button;
                    else
                        h700_keys &= ~button;
                }
            }
            else if (event.type == H700_EV_ABS)
                h700_axis_update(event.code, event.value);
        }
        if (errno && errno != EAGAIN && errno != EWOULDBLOCK)
            h700_close_input(i);
    }

    if (power_held) *power_held = h700_power_held;
    return h700_keys | h700_hat | h700_stick;
}

bool retrohh_lid_closed(void)
{
    static int fd = -2;
    const char *path;
    char value = '1';

    if (!retrohh_h700_active())
        return false;
    if (fd == -2)
    {
        path = getenv("HALL_KEY");
        if (!path || !*path)
            path = "/sys/class/power_supply/axp2202-battery/hallkey";
        fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0 || pread(fd, &value, 1, 0) != 1)
        return false;
    return value != '1'; /* kernel reports 1=open, 0=closed */
}

/* Hold/lock switch (Brick side toggle = gpio243, value 1 = engaged, same line
 * NextUI reads). Engaged -> button_read_device() returns BUTTON_NONE. */
#define HOLD_SWITCH_PATH "/sys/class/gpio/gpio243/value"

bool retrohh_hold_switch(void)
{
    static int fd = -2;            /* -2 = not opened, -1 = unavailable */
    if (retrohh_h700_active())
        return false;
    if (fd == -2)
        fd = open(HOLD_SWITCH_PATH, O_RDONLY);
    if (fd < 0)
        return false;

    char v = '0';
    if (pread(fd, &v, 1, 0) < 1)   /* sysfs: re-read from offset 0 each poll */
        return false;
    return v == '1';               /* 1 = engaged -> block input */
}

/* Gamepad -> Rockbox button mapping.
 *
 * We read the TrimUI's gamepad through SDL's joystick layer directly (the pad
 * is opened with SDL_JoystickOpen in button-sdl.c), exactly like NextUI does --
 * no gptokeyb2 shim.  The raw SDL joystick button indices below are the Brick's
 * (matching NextUI's JOY_* constants, workspace/tg5040/platform/platform.h), so
 * the physical buttons land where the launcher puts them.  The volume rocker is
 * NOT a separate device: SDL enumerates the pad node's KEY_VOLUMEUP/DOWN as
 * joystick buttons 14/13, so the rocker arrives here for free.  Index 8 is
 * NextUI's dedicated MENU key; 9/10 (L3/R3) remain unused by Trimpod. */
int joybutton_to_button(int joybtn)
{
    if (retrohh_h700_active())
    {
        switch (joybtn)
        {
            case 0:  return BUTTON_A;
            case 1:  return BUTTON_B;
            case 3:  return BUTTON_X;
            case 2:  return BUTTON_Y;
            case 4:  return BUTTON_L;
            case 5:  return BUTTON_R;
            case 6:  return BUTTON_SELECT;
            case 7:  return BUTTON_START;
            case 8:  return BUTTON_MENU;
            case 10: return BUTTON_L2;
            case 11: return BUTTON_R2;
            case 16: return BUTTON_VOL_UP;
            case 15: return BUTTON_VOL_DOWN;
            default: return BUTTON_NONE;
        }
    }
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
        case 8:  return BUTTON_MENU;
        case 14: return BUTTON_VOL_UP;   /* volume rocker, on the pad node */
        case 13: return BUTTON_VOL_DOWN;
        default: return BUTTON_NONE;     /* 9/10 = L3/R3 (unused) */
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

/* Keyboard -> Rockbox button mapping.  On the device nothing reaches this path
 * any more (the pad is read as a joystick); it is the DESKTOP SIMULATOR's keymap
 * -- you press l/k/i/j/w/s/a/d/... on the dev machine.  Kept so simulator builds
 * stay usable. */
int key_to_button(int keyboard_key)
{
    int new_btn = BUTTON_NONE;
    switch (keyboard_key)
    {
        case SDLK_l:
            new_btn = BUTTON_A;
            break;
        case SDLK_k:
            new_btn = BUTTON_B;
            break;
        case SDLK_i:
            new_btn = BUTTON_X;
            break;
        case SDLK_j:
            new_btn = BUTTON_Y;
            break;
        case SDLK_w:
            new_btn = BUTTON_UP;
            break;
        case SDLK_s:
            new_btn = BUTTON_DOWN;
            break;
        case SDLK_a:
            new_btn = BUTTON_LEFT;
            break;
        case SDLK_d:
            new_btn = BUTTON_RIGHT;
            break;
        case SDLK_g:
            new_btn = BUTTON_START;
            break;
        case SDLK_f:
            new_btn = BUTTON_SELECT;
            break;
        case SDLK_m:
            new_btn = BUTTON_MENU;
            break;
        case SDLK_q:
            new_btn = BUTTON_L;
            break;
        case SDLK_e:
            new_btn = BUTTON_R;
            break;
        case SDLK_z:
            new_btn = BUTTON_L2;
            break;
        case SDLK_c:
            new_btn = BUTTON_R2;
            break;
        /* Bluetooth media controls */
        case SDLK_AUDIOPLAY:
            new_btn = RC_BUTTON_PLAY;
            break;
        case SDLK_AUDIOPREV:
            new_btn = RC_BUTTON_PREVSONG;
            break;
        case SDLK_AUDIONEXT:
            new_btn = RC_BUTTON_NEXTSONG;
            break;
        default:
            break;
        /* SDLK_x is used for shutdown */
        /* SDLK_h is used for hold */
    }
    return new_btn;
}
