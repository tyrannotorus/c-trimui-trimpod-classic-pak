/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Björn Stenberg
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
#include "config.h"
#include "system.h"

#include "version.h"
#include "gcc_extensions.h"
#include "storage.h"
#include "disk.h"
#include "file_internal.h"
#include "lcd.h"
#include "rtc.h"
#include "debug.h"
#include "led.h"
#include "../kernel-internal.h"
#include "button.h"
#include "core_keymap.h"
#include "tree.h"
#include "filetypes.h"
#include "panic.h"
#include "menu.h"
#include "usb.h"
#include "wifi.h"
#include "powermgmt.h"
#include "adc.h"
#include "i2c.h"
#include "serial.h"
#include "audio.h"
#include "settings.h"
#include "backlight.h"
#include "status.h"
#include "font.h"
#include "language.h"
#include "wps.h"
#include "playlist.h"
#include "core_alloc.h"
#include "rolo.h"
#include "screens.h"
#include "usb_screen.h"
#include "power.h"
#include "misc.h"
#include "trimpod_hold.h"
#include "trimpod_ui.h"
#include "trimpod_library.h"
#include "trimpod_m4b.h"
#include "rbunicode.h"
#include "dsp_core.h"
#include "dircache.h"
#include "lang.h"
#include "string.h"
#include "splash.h"
#include "eeprom_settings.h"
#include "icon.h"
#include "viewport.h"
#include "skin_engine/skin_engine.h"
#include "statusbar-skinned.h"
#include "bootchart.h"
#include "logdiskf.h"
#include "bootdata.h"

#include "shortcuts.h"


#include "audio_thread.h"
#include "playback.h"
#include "tdspeed.h"

#ifdef BUTTON_REC
    #define SETTINGS_RESET BUTTON_REC
#endif

#if (CONFIG_STORAGE & STORAGE_MMC)
#include "ata_mmc.h"
#endif

#if CONFIG_USBOTG == USBOTG_ISP1362
#include "isp1362.h"
#endif

#if CONFIG_USBOTG == USBOTG_M5636
#include "m5636.h"
#endif


/* gcc adds an implicit 'return 0;' at the end of main(), causing a warning
 * with noreturn attribute */
#define MAIN_NORETURN_ATTR


#include "system-sdl.h"
#define HAVE_ARGV_MAIN
/* Don't use SDL_main on windows -> no more stdio redirection */

// #define AUTOROCK /* define this to check for "autostart.rock" on boot */
/* Alternatively, you can define autostart plugin path and its argument: */
// #define AUTOROCK     VIEWERS_DATA_DIR"/imageviewer.rock"
// #define AUTOROCK_ARG "/jpegs/sample.jpg"

static void init(void);
/* main(), and various functions called by main() and init() may be
 * be INIT_ATTR. These functions must not be called after the final call
 * to root_menu() at the end of main()
 * see definition of INIT_ATTR in config.h */
#ifdef HAVE_ARGV_MAIN
int main(int argc, char *argv[]) INIT_ATTR MAIN_NORETURN_ATTR ;
int main(int argc, char *argv[])
{
    sys_handle_argv(argc, argv);
#else
int main(void) INIT_ATTR MAIN_NORETURN_ATTR;
int main(void)
{
#endif
    CHART(">init");
    init();
    CHART("<init");
    FOR_NB_SCREENS(i)
    {
        screens[i].clear_display();
        screens[i].update();
    }
    list_init();
    tree_init();
    /* Keep the order of this 3
     * Must be done before any code uses the multi-screen API */

#if !defined(DISABLE_ACTION_REMAP) && defined(CORE_KEYREMAP_FILE)
    if (file_exists(CORE_KEYREMAP_FILE))
    {
        int mapct = core_load_key_remap(CORE_KEYREMAP_FILE);
        if (mapct <= 0)
            splashf(HZ, "key remap failed: %d,  %s", mapct, CORE_KEYREMAP_FILE);
    }
#endif

    if (!file_exists(ROCKBOX_DIR"/playername.txt"))
    {
        int fd = open(ROCKBOX_DIR"/playername.txt", O_CREAT|O_WRONLY|O_TRUNC, 0666);
        if(fd >= 0)
        {
            fdprintf(fd, "%s", MODEL_NAME);
            close(fd);
        }
    }

    global_status.last_volume_change = 0;
    validate_start_directory_init();
    /* Build/refresh the SQLite library index (stat-gated: a blink when nothing
     * changed; shows "Scanning Library NN%" while parsing new files) so
     * Shuffle Songs and the library browse are ready. */
    trimpod_library_init();
    /* no calls INIT_ATTR functions after this point anymore!
     * see definition of INIT_ATTR in config.h */
    CHART(">root_menu");
    /* Warm the About glyph cache now, before playback resumes, so the first
     * About open doesn't fault glyphs off disk and stall the codec. */
    trimpod_about_prewarm();
    /* Launched with the HOLD switch engaged: show the lock page instead of the
     * menu until it's released (no-op when the switch is off). */
    trimpod_hold_run();
    root_menu();
}

/* The disk isn't ready at boot, rblogo is stored in bin and erased after boot */
int show_logo_boot( void ) INIT_ATTR;
int show_logo_boot( void )
{
    return 0; /* no boot splash; load straight into the UI */
}



static void init(void)
{
    system_init();
    core_allocator_init();
    kernel_init();
#ifdef APPLICATION
    paths_init();
#endif
    enable_irq();
    lcd_init();
    FOR_NB_SCREENS(i)
        global_status.font_id[i] = FONT_SYSFIXED;
    font_init();
    show_logo_boot();
    button_init();
    powermgmt_init();
    backlight_init();
    unicode_init();
    lang_init(core_language_builtin, language_strings,
              LANG_LAST_INDEX_IN_ARRAY);
    /* Keep the order of this 3 (viewportmanager handles statusbars)
     * Must be done before any code uses the multi-screen API */
    gui_syncstatusbar_init(&statusbars);
    gui_sync_skin_init();
    sb_skin_init();
    viewportmanager_init();

    storage_init();
    pcm_init();
    dsp_init();
    settings_reset();
    settings_load();
    settings_apply(true);
    /* Trimpod: m4b chapters ride the cuesheet engine, so force it on (the
     * setting is unexposed and stale configs may say off; must precede
     * audio_init so the scratch buffer reserves the cuesheet slot), and load
     * the per-file audiobook resume positions. */
    global_settings.cuesheet = true;
    trimpod_m4b_resume_init();
#ifdef RETRO_HANDHELD
    /* Apply the persisted CPU Frequency choice (Settings -> Power -> CPU).
     * power-target.c is the single owner of CPU policy; launch.sh only saves and
     * restores the system's original cpufreq state. */
    {
        extern void retrohh_cpu_apply_saved(void);
        retrohh_cpu_apply_saved();
        /* Charge Limit (Settings -> Power): probe + start the in-app poll, or
         * defer to the standalone Battery Care daemon if it's already running. */
        extern void retrohh_charge_limit_init(void);
        retrohh_charge_limit_init();
    }
#endif
    init_battery_tables();
    tree_mem_init();
    filetype_init();
    playlist_init();
    shortcuts_init();

    audio_init();
    settings_apply_skins();

/* do USB last so prompt (if enabled) can work correctly if USB was inserted with device off,
 * also doesn't hurt that it will display the nice pretty backdrop this way too. */
#ifndef USB_NONE
    usb_init();
    usb_start_monitoring();
#endif
}

