/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Trimpod: the Power (Device) settings menu -- Theme, Brightness, Display
 * Poweroff, Idle Poweroff, CPU Frequency, Charge Limit -- on one do_menu page.
 * Theme, CPU and Charge Limit are inline value knobs (LEFT/RIGHT cycle, applied
 * live); the others are plain settings rows.
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
#include <stddef.h>
#include <stdio.h>          /* snprintf for the Charge Limit value label */
#include "config.h"
#include "lang.h"
#include "settings.h"
#include "menu.h"
#include "screen_access.h" /* screens[].set_background for the bg colour presets */
#include "viewport.h"       /* viewportmanager_theme_changed: full redraw on colour change */
#include "exported_menus.h"

/* Theme: a named background + ink pair, cycled as one knob.  Backgrounds are
 * the iPod-family colour lineup (iPod mini + nano tones; Apple never published
 * official hexes, so these are the recognised approximations).  Every
 * background comes in three inks:
 *   Dark  - the near-black mono-LCD ink (#181C18), the classic look;
 *   Light - white, for light-on-dark pairings (white on (PRODUCT)RED etc);
 *   a third, per-family ink picked for hue harmony with readable contrast
 *   (complementary navy on yellow/orange, monochromatic deep tones elsewhere,
 *   the Game Boy green on Mono Green).
 * Written straight into the bg_color/fg_color theme settings. */
#define TRIMPOD_INK_DARK  0x181C18   /* classic mono-LCD ink */
#define TRIMPOD_INK_LIGHT 0xFFFFFF

struct trimpod_theme
{
    const char *name;
    unsigned    bg;
    unsigned    fg;
};

static const struct trimpod_theme trimpod_themes[] = {
    /* Classic (0xD8DCD0, the mono LCD): Dark is the shipped default look */
    { "Classic Dark",     0xD8DCD0, TRIMPOD_INK_DARK  },
    { "Classic Light",    0xD8DCD0, TRIMPOD_INK_LIGHT },
    { "Classic Sepia",    0xD8DCD0, 0x5D4037 },  /* warm book-ink sepia   */
    { "Silver Dark",      0xC4C7CC, TRIMPOD_INK_DARK  },
    { "Silver Light",     0xC4C7CC, TRIMPOD_INK_LIGHT },
    { "Silver Slate",     0xC4C7CC, 0x37474F },  /* cool monochrome       */
    { "Slate Dark",       0x637D8C, TRIMPOD_INK_DARK  },
    { "Slate Light",      0x637D8C, TRIMPOD_INK_LIGHT },
    { "Slate Gold",       0x637D8C, 0xFFD97D },  /* warm on cool          */
    { "Blue Dark",        0x0094E1, TRIMPOD_INK_DARK  },
    { "Blue Light",       0x0094E1, TRIMPOD_INK_LIGHT },
    { "Blue Navy",        0x0094E1, 0x002B4A },  /* deep monochrome       */
    { "Green Dark",       0xA0CB3B, TRIMPOD_INK_DARK  },
    { "Green Light",      0xA0CB3B, TRIMPOD_INK_LIGHT },
    { "Green Forest",     0xA0CB3B, 0x2E4A1C },  /* deep monochrome       */
    { "Yellow Dark",      0xFFD400, TRIMPOD_INK_DARK  },
    { "Yellow Light",     0xFFD400, TRIMPOD_INK_LIGHT },
    { "Yellow Navy",      0xFFD400, 0x1D3557 },  /* complementary         */
    { "Gold Dark",        0xFAB71F, TRIMPOD_INK_DARK  },
    { "Gold Light",       0xFAB71F, TRIMPOD_INK_LIGHT },
    { "Gold Espresso",    0xFAB71F, 0x4E342E },  /* warm analogous        */
    { "Orange Dark",      0xF08A1C, TRIMPOD_INK_DARK  },
    { "Orange Light",     0xF08A1C, TRIMPOD_INK_LIGHT },
    { "Orange Navy",      0xF08A1C, 0x1D3557 },  /* complementary         */
    { "Red Dark",         0xEA323C, TRIMPOD_INK_DARK  },   /* (PRODUCT)RED */
    { "Red Light",        0xEA323C, TRIMPOD_INK_LIGHT },
    { "Red Cream",        0xEA323C, 0xFFF3E0 },  /* soft warm white       */
    { "Pink Dark",        0xEC5298, TRIMPOD_INK_DARK  },
    { "Pink Light",       0xEC5298, TRIMPOD_INK_LIGHT },
    { "Pink Berry",       0xEC5298, 0x4A0728 },  /* deep monochrome       */
    { "Purple Dark",      0x9B72B0, TRIMPOD_INK_DARK  },
    { "Purple Light",     0x9B72B0, TRIMPOD_INK_LIGHT },
    { "Purple Plum",      0x9B72B0, 0x2A1438 },  /* deep monochrome       */
    { "Mono Green Dark",  0xBFCBB0, TRIMPOD_INK_DARK  },
    { "Mono Green Light", 0xBFCBB0, TRIMPOD_INK_LIGHT },
    { "Mono Green Pixel", 0xBFCBB0, 0x0F380F },  /* the Game Boy pairing  */
};
#define TRIMPOD_NTHEMES \
    ((int)(sizeof(trimpod_themes)/sizeof(trimpod_themes[0])))

static void trimpod_theme_apply(int idx)
{
    if (idx < 0 || idx >= TRIMPOD_NTHEMES)
        return;
    unsigned old_bg = global_settings.bg_color;
    unsigned old_fg = global_settings.fg_color;
    global_settings.bg_color = trimpod_themes[idx].bg;
    global_settings.fg_color = trimpod_themes[idx].fg;
    screens[SCREEN_MAIN].set_background(global_settings.bg_color);
    screens[SCREEN_MAIN].set_foreground(global_settings.fg_color);
    /* Recolour the loaded skins' cached default colours in place, then force
     * the UI viewports to re-fetch them and fully redraw.  A full skin reload
     * would apply the same change but stop playback. */
    skin_update_bg_color(old_bg, global_settings.bg_color);
    skin_update_fg_color(old_fg, global_settings.fg_color);
    viewportmanager_theme_changed(THEME_UI_VIEWPORT | THEME_STATUSBAR |
                                  THEME_LISTS);
    settings_save();               /* self-persisting: no page-close hook needed */
}

/* CPU Frequency: inline selector over the A133's cpufreq steps; applied and
 * persisted live (power-target.c owns cpu_freq.txt). */
extern void retrohh_cpu_set_freq(int khz);
extern void retrohh_cpu_set_dynamic(void);
extern int  retrohh_cpu_get_freq(void);
extern bool retrohh_cpu_is_dynamic(void);
extern int  retrohh_cpu_freq_count(void);
extern int  retrohh_cpu_freq_at(int index);
extern void retrohh_cpu_save_choice(int khz);   /* persistence lives in power-target.c */

/* Charge Limit: caps charging at a % while Trimpod runs (power-target.c). The
 * row is hidden entirely when the standalone Battery Care daemon owns charging. */
extern bool retrohh_charge_limit_hidden(void);
extern int  retrohh_charge_limit_get_target(void);
extern void retrohh_charge_limit_cycle(int dir);

static int trimpod_cpu_nearest_index(int khz)
{
    int count = retrohh_cpu_freq_count();
    int idx = count - 1;
    for (int i = 0; i < count; i++)
        if (retrohh_cpu_freq_at(i) >= khz) { idx = i; break; }
    return idx;
}

static void trimpod_cpu_changed(int khz, void *ctx)
{
    (void)ctx;
    if (khz <= 0)
        retrohh_cpu_set_dynamic();       /* "Dynamic": tuned interactive, full range */
    else
        retrohh_cpu_set_freq(khz);       /* pin to a fixed step, live */
    retrohh_cpu_save_choice(khz);        /* persists (khz<=0 -> "dynamic") */
}

/* ---- do_menu inline value-row callbacks (struct menu_value_cb) ----------- *
 * get() returns the current label (static strings, so no copy), cycle() steps
 * the value and applies + persists it live.  Stateless -- the current index is
 * derived from the live freq / bg_color each call. */
static const char *tp_cpu_value_get(void *ctx, char *buf, int len)
{
    (void)ctx;
    if (retrohh_cpu_is_dynamic())
        return (const char *)str(LANG_TRIMPOD_DYNAMIC);
    int khz = retrohh_cpu_freq_at(
        trimpod_cpu_nearest_index(retrohh_cpu_get_freq()));
    snprintf(buf, len, "%d MHz", khz / 1000);
    return buf;
}
static void tp_cpu_value_cycle(void *ctx, int dir)
{
    (void)ctx;
    int count = retrohh_cpu_freq_count();
    /* Virtual list: index 0 = "Dynamic" (first), 1..N = the fixed steps. */
    int vidx = retrohh_cpu_is_dynamic()
                 ? 0
                 : trimpod_cpu_nearest_index(retrohh_cpu_get_freq()) + 1;
    vidx += (dir < 0 ? -1 : 1);
    if (vidx < 0) vidx = 0;
    if (vidx > count) vidx = count;
    trimpod_cpu_changed(vidx == 0 ? 0 : retrohh_cpu_freq_at(vidx - 1), NULL);
}
static const struct menu_value_cb trimpod_cpu_value =
    { tp_cpu_value_get, tp_cpu_value_cycle, NULL };

/* Current theme = the pair (bg_color, fg_color) matched against the table;
 * -1 when the saved settings predate the selector (an unlisted combo). */
static int tp_theme_cur_index(void)
{
    for (int i = 0; i < TRIMPOD_NTHEMES; i++)
        if (trimpod_themes[i].bg == (unsigned)global_settings.bg_color &&
            trimpod_themes[i].fg == (unsigned)global_settings.fg_color)
            return i;
    return -1;
}
static const char *tp_theme_value_get(void *ctx, char *buf, int len)
{
    (void)ctx; (void)buf; (void)len;
    int idx = tp_theme_cur_index();
    if (idx >= 0)
        return trimpod_themes[idx].name;
    return (const char *)str(LANG_TRIMPOD_CUSTOM);
}
static void tp_theme_value_cycle(void *ctx, int dir)
{
    (void)ctx;
    int cur = tp_theme_cur_index();
    /* from an unlisted combo any step lands on the first theme */
    int idx = (cur < 0) ? 0 : cur + (dir < 0 ? -1 : 1);
    if (idx < 0) idx = 0;
    if (idx >= TRIMPOD_NTHEMES) idx = TRIMPOD_NTHEMES - 1;
    if (idx != cur)
        trimpod_theme_apply(idx);
}
static const struct menu_value_cb trimpod_theme_value =
    { tp_theme_value_get, tp_theme_value_cycle, NULL };

/* Charge Limit: inline value 75/80/85/90/95/100% (100% = no cap), wrapping. The
 * row is omitted from the menu entirely when the Battery Care daemon is running
 * (see trimpod_power_page), so it's only shown when Trimpod manages. */
static const char *tp_charge_value_get(void *ctx, char *buf, int len)
{
    (void)ctx;
    snprintf(buf, len, "%d%%", retrohh_charge_limit_get_target());
    return buf;
}
static void tp_charge_value_cycle(void *ctx, int dir)
{
    (void)ctx;
    retrohh_charge_limit_cycle(dir);
}
static const struct menu_value_cb trimpod_charge_value =
    { tp_charge_value_get, tp_charge_value_cycle, NULL };

/* The page, in order: Theme, Brightness, Display Poweroff, Idle Poweroff, CPU
 * Frequency, Charge Limit.  Setting rows label from their setting lang_id. */
MENUITEM_VALUE(tp_pw_cpu, ID2P(LANG_TRIMPOD_CPU), &trimpod_cpu_value, Icon_NOICON);
MENUITEM_SETTING(tp_pw_brightness, &global_settings.brightness, NULL);
MENUITEM_VALUE(tp_pw_theme, ID2P(LANG_TRIMPOD_THEME), &trimpod_theme_value, Icon_NOICON);
MENUITEM_SETTING(tp_pw_screenoff, &global_settings.backlight_timeout, NULL);
MENUITEM_SETTING(tp_pw_idlepoweroff, &global_settings.poweroff, NULL);
MENUITEM_VALUE(tp_pw_charge, ID2P(LANG_TRIMPOD_CHARGE_LIMIT),
               &trimpod_charge_value, Icon_NOICON);
/* Two variants: the Charge Limit row is present only when Trimpod manages
 * charging.  When the Battery Care daemon owns the bit the row is simply absent
 * (the "_nobatt" menu) rather than shown disabled -- see trimpod_power_page. */
MAKE_MENU(trimpod_power_menu, ID2P(LANG_TRIMPOD_DEVICE), NULL, Icon_Submenu_Entered,
          &tp_pw_theme, &tp_pw_brightness, &tp_pw_screenoff,
          &tp_pw_idlepoweroff, &tp_pw_cpu, &tp_pw_charge);
MAKE_MENU(trimpod_power_menu_nobatt, ID2P(LANG_TRIMPOD_DEVICE), NULL, Icon_Submenu_Entered,
          &tp_pw_theme, &tp_pw_brightness, &tp_pw_screenoff,
          &tp_pw_idlepoweroff, &tp_pw_cpu);

int trimpod_power_page(void)
{
    const struct menu_item_ex *m = retrohh_charge_limit_hidden()
                                 ? &trimpod_power_menu_nobatt : &trimpod_power_menu;
    do_menu(m, NULL, NULL, false);
    return 0;
}
