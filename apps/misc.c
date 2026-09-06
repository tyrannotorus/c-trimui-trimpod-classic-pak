/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Daniel Stenberg
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
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include "string-extra.h"
#include "config.h"
#include "misc.h"
#include "system.h"
#include "lcd.h"
#include "language.h" /* is_lang_rtl() */
#include "iap-usb.h"

#include "file.h"
#include "pathfuncs.h"
#include "lang.h"
#include "dir.h"
#include "action.h"
#include "timefuncs.h"
#include "screens.h"
#include "usb_screen.h"
#include "audio.h"
#include "settings.h"
#include "storage.h"
#include "ata_idle_notify.h"
#include "kernel.h"
#include "power.h"
#include "powermgmt.h"
#include "backlight.h"
#include "font.h"
#include "splash.h"
#include "sound.h"
#include "playlist.h"
#include "trimpod_library.h"
#include "trimpod_ui.h"      /* trimpod_fullscreen_message */
#include "yesno.h"
#include "viewport.h"
#include "list.h"
#include "fixedpoint.h"
#include "statusbar-skinned.h"

#include "debug.h"


#if (CONFIG_STORAGE & STORAGE_MMC)
#include "ata_mmc.h"
#endif
#include "tree.h"
#include "eeprom_settings.h"
#include "bmp.h"
#include "icons.h"
#include "wps.h"
#include "playback.h"

#ifdef BOOTFILE
#if !defined(USB_NONE) && !defined(USB_HANDLED_BY_OF)
#include "rolo.h"
#endif
#endif

#ifndef PLUGIN
#include "core_alloc.h" /*core_load_bmp()*/
#endif


/* units used with output_dyn_value */
const unsigned char * const byte_units[] =
{
    ID2P(LANG_BYTE),
    ID2P(LANG_KIBIBYTE),
    ID2P(LANG_MEBIBYTE),
    ID2P(LANG_GIBIBYTE)
};

const unsigned char * const * const kibyte_units = &byte_units[1];

/* units used with format_time_auto, option_select.c->option_get_valuestring() */
const unsigned char * const unit_strings_core[] =
{
    [UNIT_INT] = "",    [UNIT_MS]  = "ms",
    [UNIT_SEC] = "s",   [UNIT_MIN] = "min",
    [UNIT_HOUR]= "hr",  [UNIT_KHZ] = "kHz",
    [UNIT_DB]  = "dB",  [UNIT_PERCENT] = "%",
    [UNIT_MAH] = "mAh", [UNIT_PIXEL] = "px",
    [UNIT_PER_SEC] = "per sec",
    [UNIT_HERTZ] = "Hz",
    [UNIT_MB]  = "MB",  [UNIT_KBIT]  = "kb/s",
    [UNIT_PM_TICK] = "units/10ms",
};

/* Format a large-range value for output, using the appropriate unit so that
 * the displayed value is in the range 1 <= display < 1000 (1024 for "binary"
 * units) if possible, and 3 significant digits are shown. If a buffer is
 * given, the result is snprintf()'d into that buffer, otherwise the result is
 * voiced.*/
char *output_dyn_value(char *buf,
                       int buf_size,
                       int64_t value,
                       const unsigned char * const *units,
                       unsigned int unit_count,
                       bool binary_scale)
{
    unsigned int scale = binary_scale ? 1024 : 1000;
    unsigned int fraction = 0;
    unsigned int unit_no = 0;
    uint64_t value_abs = (value < 0) ? -value : value;
    char tbuf[5];
    int value2;

    while (value_abs >= scale && unit_no < (unit_count - 1))
    {
        fraction = value_abs % scale;
        value_abs /= scale;
        unit_no++;
    }

    value2 = (value < 0) ? -value_abs : value_abs; /* preserve sign */
    fraction = (fraction * 1000 / scale) / 10;

    if (value_abs >= 100 || fraction >= 100 || !unit_no)
        tbuf[0] = '\0';
    else if (value_abs >= 10)
        snprintf(tbuf, sizeof(tbuf), "%01u", fraction / 10);
    else
        snprintf(tbuf, sizeof(tbuf), "%02u", fraction);

    if (buf)
    {
        if (*tbuf)
            snprintf(buf, buf_size, "%d%s%s%s", value2, str(LANG_POINT),
                     tbuf, P2STR(units[unit_no]));
        else
            snprintf(buf, buf_size, "%d%s", value2, P2STR(units[unit_no]));
    }
    return buf;
}

/* Ask the user if they really want to erase the current dynamic playlist
 * returns true if the playlist should be replaced */
bool warn_on_pl_erase(void)
{
    if (global_status.resume_index != -1 &&
        TP_WARN_ON_ERASE &&
        playlist_modified(NULL))
    {
        static const char *lines[] =
            {ID2P(LANG_WARN_ERASEDYNPLAYLIST_PROMPT)};
        static const struct text_message message={lines, 1};

        if (gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES)
            return true;
        else
        {
            splash(HZ, ID2P(LANG_CANCEL));
            return false;
        }
    }
    else
        return true;
}

bool show_search_progress(bool init, int display_count, int current, int total)
{
    static long last_tick;

    /* Don't show splashes for 1/2 second after starting search */
    if (init)
    {
        last_tick = current_tick + HZ/2;
        return true;
    }

    /* Update progress every 1/10 of a second */
    if (TIME_AFTER(current_tick, last_tick + HZ/10))
    {
        if (total != current)
            /* (voiced) */
            splash_progress(current, total, str(LANG_PLAYLIST_SEARCH_MSG),
                            display_count, str(LANG_OFF_ABORT));
        else
        {
            splashf(0, str(LANG_PLAYLIST_SEARCH_MSG),
                    display_count, str(LANG_OFF_ABORT));
        }

        if (action_userabort(TIMEOUT_NOBLOCK))
            return false;
        last_tick = current_tick;
        yield();
    }

    return true;
}

/* Performance optimized version of the read_line() (see below) function. */
int fast_readline(int fd, char *buf, int buf_size, void *parameters,
                  int (*callback)(int n, char *buf, void *parameters))
{
    char *p, *next;
    int rc, pos = 0;
    int count = 0;

    while ( 1 )
    {
        next = NULL;

        rc = read(fd, &buf[pos], buf_size - pos - 1);
        if (rc >= 0)
            buf[pos+rc] = '\0';

        if ( (p = strchr(buf, '\n')) != NULL)
        {
            *p = '\0';
            next = ++p;
        }

        if ( (p = strchr(buf, '\r')) != NULL)
        {
            *p = '\0';
            if (!next)
                next = ++p;
        }

        rc = callback(count, buf, parameters);
        if (rc < 0)
            return rc;

        count++;
        if (next)
        {
            pos = buf_size - ((intptr_t)next - (intptr_t)buf) - 1;
            memmove(buf, next, pos);
        }
        else
            break ;
    }

    return 0;
}

/* parse a line from a configuration file. the line format is:

   name: value

   Any whitespace before setting name or value (after ':') is ignored.
   A # as first non-whitespace character discards the whole line.
   Function sets pointers to null-terminated setting name and value.
   Returns false if no valid config entry was found.
*/

bool settings_parseline(char* line, char** name, char** value)
{
    char* ptr;

    line = skip_whitespace(line);

    if ( *line == '#' )
        return false;

    ptr = strchr(line, ':');
    if ( !ptr )
        return false;

    *name = line;
    *ptr = '\0'; /* terminate previous */
    ptr++;
    ptr = skip_whitespace(ptr);
    *value = ptr;

    /* strip whitespace from the right side of value */
    ptr += strlen(ptr);
    ptr--;
    while ((ptr > (*value) - 1) && isspace(*ptr))
    {
        *ptr = '\0';
        ptr--;
    }

    return true;
}

static void system_flush(void)
{
    playlist_shutdown();
    trimpod_library_close();    /* close the SQLite index cleanly on shutdown */
    tree_flush();
    call_storage_idle_notifys(true); /*doesnt work on usb and shutdown from ata thread */
}

static void system_restore(void)
{
    tree_restore();
}

static bool clean_shutdown(enum shutdown_type sd_type,
                           void (*callback)(void *), void *parameter)
{
    status_save(true);

#if CONFIG_CHARGING && !defined(HAVE_POWEROFF_WHILE_CHARGING)
    if(!charger_inserted())
#endif
    {
        bool batt_safe = battery_level_safe();

        FOR_NB_SCREENS(i)
        {
            screens[i].clear_display();
            screens[i].update();
        }

        if (batt_safe)
        {
            int level;
            level = battery_level();
            if (level > 10 || level < 0)
            {
                /* Trimpod: always show the shutdown message, full screen (no
                 * splash box), held long enough to be seen before exit. */
                trimpod_fullscreen_message(str(LANG_SHUTTINGDOWN), NULL);
                sleep(HZ);
            }
            else
            {
                splashf(0, "%s %s", str(LANG_WARNING_BATTERY_LOW),
                                    str(LANG_SHUTTINGDOWN));
            }
        }
        else
        {
            splashf(0, "%s %s", str(LANG_WARNING_BATTERY_EMPTY),
                                str(LANG_SHUTTINGDOWN));
        }

        {
            /* audio_stop_recording == audio_stop for HWCODEC */
            audio_stop();

            if (callback != NULL)
                callback(parameter);

            system_flush();
        }

        shutdown_hw(sd_type);
    }
    return false;
}

bool list_stop_handler(void)
{
    bool ret = false;

    /* Stop the music if it is playing */
    if(audio_status())
    {
        audio_stop();
        ret = true;  /* status changed: a refresh may be necessary */
    }
#if CONFIG_CHARGING
#ifndef HAVE_POWEROFF_WHILE_CHARGING
    {
        static long last_off = 0;

        if (TIME_BEFORE(current_tick, last_off + HZ/2))
        {
            if (charger_inserted())
            {
                charging_splash();
                ret = true;  /* screen is dirty, caller needs to refresh */
            }
        }
        last_off = current_tick;
    }
#endif
#endif /* CONFIG_CHARGING */
    return ret;
}




long default_event_handler_ex(long event, void (*callback)(void *), void *parameter)
{

    switch(event)
    {
        case SYS_BATTERY_UPDATE:
            break;
        case SYS_USB_CONNECTED:
        {
            intptr_t seqnum = button_get_data();
            if (callback != NULL)
                callback(parameter);
            system_flush();
#ifdef BOOTFILE
#if !defined(USB_NONE) && !defined(USB_HANDLED_BY_OF)
            check_bootfile(false); /* gets initial size */
#endif
#endif
            gui_usb_screen_run(false, seqnum);
#ifdef BOOTFILE
#if !defined(USB_NONE) && !defined(USB_HANDLED_BY_OF)
            check_bootfile(true);
#endif
#endif
            system_restore();
            return SYS_USB_CONNECTED;
        }

        /* Trimpod: the Bluetooth speaker went away mid-song, posted by the PCM
         * writer (firmware/target/hosted/sdl/pcm-alsa.c).  Pause instead of
         * following the route back to the internal speaker -- a private listen
         * must not become a loudspeaker one.  Deliberately no auto-resume when
         * it reconnects; that is the user's call. */
        case SYS_PHONE_UNPLUGGED:
            if (audio_status() & AUDIO_STATUS_PLAY)
                audio_pause();
            break;

        case SYS_POWEROFF:
        case SYS_REBOOT:
        {
            enum shutdown_type sd_type;
            if (event == SYS_POWEROFF)
                sd_type = SHUTDOWN_POWER_OFF;
            else
                sd_type = SHUTDOWN_REBOOT;

            if (!clean_shutdown(sd_type, callback, parameter))
                return event;
        }
            break;
#if CONFIG_CHARGING
        case SYS_CHARGER_CONNECTED:
            return SYS_CHARGER_CONNECTED;

        case SYS_CHARGER_DISCONNECTED:
            reset_runtime();
            return SYS_CHARGER_DISCONNECTED;
#endif
#if (CONFIG_PLATFORM & PLATFORM_HOSTED) && defined(PLATFORM_HAS_VOLUME_CHANGE)
        case SYS_VOLUME_CHANGED:
        {
            static bool firstvolume = true;
            /* kludge: since this events go to the button_queue,
             * event data is available in the last button data */
            int volume = button_get_data();
            DEBUGF("SYS_VOLUME_CHANGED: %d\n", volume);
            if (global_status.volume != volume) {
                global_status.volume = volume;
                if (firstvolume) {
                    setvol();
                    firstvolume = false;
                }
            }
            return 0;
        }
#endif
    }
    return 0;
}

long default_event_handler(long event)
{
    return default_event_handler_ex(event, NULL, NULL);
}

#ifdef BOOTFILE
#if !defined(USB_NONE) && !defined(USB_HANDLED_BY_OF)
/*
    memorize/compare details about the BOOTFILE
    we don't use dircache because it may not be up to date after
    USB disconnect (scanning in the background)
*/
void check_bootfile(bool do_rolo)
{
    static time_t mtime = 0;
    DIR* dir = NULL;
    struct dirent* entry = NULL;

    /* 1. open BOOTDIR and find the BOOTFILE dir entry */
    dir = opendir(BOOTDIR);

    if(!dir) return; /* do we want an error splash? */

    /* loop all files in BOOTDIR */
    while(0 != (entry = readdir(dir)))
    {
        if(!strcasecmp(entry->d_name, BOOTFILE))
        {
            struct dirinfo info = dir_get_info(dir, entry);
            /* found the bootfile */
            if(mtime && do_rolo)
            {
                if(info.mtime != mtime)
                {
                    static const char *lines[] = { ID2P(LANG_BOOT_CHANGED),
                                                   ID2P(LANG_REBOOT_NOW) };
                    static const struct text_message message={ lines, 2 };
                    button_clear_queue(); /* Empty the keyboard buffer */
                    if(gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES)
                    {
                        audio_hard_stop();
                        rolo_load(BOOTDIR "/" BOOTFILE);
                    }
                }
            }
            mtime = info.mtime;
            break;
        }
    }
    closedir(dir);
}
#endif
#endif

/* set volume and save settings */
void setvol(void)
{
    sound_set_volume(global_status.volume);
    iap_on_volume(global_status.volume);
    global_status.last_volume_change = current_tick;
    status_save(false);
}

#ifdef HAVE_PERCEPTUAL_VOLUME
static short norm_tab[MAX_NORM_VOLUME_STEPS+2];
static int norm_tab_num_steps;
static int norm_tab_size;

static void update_norm_tab(void)
{
    const int lim = global_settings.volume_adjust_norm_steps;
    if (lim == norm_tab_num_steps)
        return;
    norm_tab_num_steps = lim;

    const int min = sound_min(SOUND_VOLUME);
    const int max = sound_max(SOUND_VOLUME);
    const int step = sound_steps(SOUND_VOLUME);

    /* Ensure the table contains the minimum volume */
    norm_tab[0] = min;
    norm_tab_size = 1;

    /* Trimpod: bound is lim-1 -- lim-2 drops the next-to-top index and leaves a
     * double-width final step. */
    for (int i = 1; i < lim; ++i)
    {
        int vol = from_normalized_volume(i, min, max, lim);
        int rem = vol % step;

        vol -= rem;
        if (abs(rem) > step/2)
            vol += rem < 0 ? -step : step;

        /* Add volume step, ignoring any duplicate entries that may
         * occur due to rounding */
        if (vol != norm_tab[norm_tab_size-1])
            norm_tab[norm_tab_size++] = vol;
    }

    /* Ensure the table contains the maximum volume */
    if (norm_tab[norm_tab_size-1] != max)
        norm_tab[norm_tab_size++] = max;
}

void set_normalized_volume(int vol)
{
    update_norm_tab();

    if (vol < 0)
        vol = 0;
    if (vol >= norm_tab_size)
        vol = norm_tab_size - 1;

    global_status.volume = norm_tab[vol];
}

int get_normalized_volume(void)
{
    update_norm_tab();

    int a = 0, b = norm_tab_size - 1;
    while (a != b)
    {
        int i = (a + b + 1) / 2;
        if (global_status.volume < norm_tab[i])
            b = i - 1;
        else
            a = i;
    }

    return a;
}

int get_normalized_volume_steps(void)
{
    update_norm_tab();
    return norm_tab_size - 1;      /* notches run 0 (min) .. size-1 (max) */
}
#else
void set_normalized_volume(int vol)
{
    global_status.volume = vol * sound_steps(SOUND_VOLUME);
}

int get_normalized_volume(void)
{
    return global_status.volume / sound_steps(SOUND_VOLUME);
}

int get_normalized_volume_steps(void)
{
    return (sound_max(SOUND_VOLUME) - sound_min(SOUND_VOLUME))
           / sound_steps(SOUND_VOLUME);
}
#endif

void adjust_volume(int steps)
{
#ifdef HAVE_PERCEPTUAL_VOLUME
    adjust_volume_ex(steps, global_settings.volume_adjust_mode);
#else
    adjust_volume_ex(steps, VOLUME_ADJUST_DIRECT);
#endif
}

void adjust_volume_ex(int steps, enum volume_adjust_mode mode)
{
    switch (mode)
    {
    case VOLUME_ADJUST_PERCEPTUAL:
#ifdef HAVE_PERCEPTUAL_VOLUME
        set_normalized_volume(get_normalized_volume() + steps);
        break;
#endif
    case VOLUME_ADJUST_DIRECT:
    default:
        global_status.volume += steps * sound_steps(SOUND_VOLUME);
        break;
    }

    setvol();
}

char* strrsplt(char* str, int c)
{
    char* s = strrchr(str, c);

    if (s != NULL)
    {
        *s++ = '\0';
    }
    else
    {
        s = str;
    }

    return s;
}

/*
 * removes the extension of filename (if it doesn't start with a .)
 * puts the result in buffer
 */
char *strip_extension(char* buffer, int buffer_size, const char *filename)
{
    if (!buffer || !filename || buffer_size <= 0)
    {
        return NULL;
    }

    off_t dotpos = (strrchr(filename, '.') - filename) + 1;

    /* no match on filename beginning with '.' or beyond buffer_size */
    if(dotpos > 1 && dotpos < buffer_size)
        buffer_size = dotpos;
    strmemccpy(buffer, filename, buffer_size);

    return buffer;
}

/* Trimpod: keypress click is fixed off (no Keyclick setting). */
static int keyclick_level = TP_KEYCLICK;

/* Play a standard sound */
void system_sound_play(enum system_sound sound)
{
    static const struct beep_params
    {
        int *setting;
        unsigned short frequency;
        unsigned short duration;
        unsigned short amplitude;
    } beep_params[] =
    {
        [SOUND_KEYCLICK] =
        { &keyclick_level,
          4000, KEYCLICK_DURATION, 2500 },
        [SOUND_TRACK_SKIP] =
        { &global_settings.beep,
          2000, 100, 2500 },
        [SOUND_TRACK_NO_MORE] =
        { &global_settings.beep,
          1000, 100, 1500 },
        [SOUND_LIST_EDGE_BEEP_NOWRAP] =
        { &keyclick_level,
          1000, 40, 1500 },
        [SOUND_LIST_EDGE_BEEP_WRAP] =
        { &keyclick_level,
          2000, 20, 1500 },

    };

    const struct beep_params *params = &beep_params[sound];

    if (*params->setting)
    {
        beep_play(params->frequency, params->duration,
                  params->amplitude * *params->setting);
    }
}

static keyclick_callback keyclick_current_callback = NULL;
static void* keyclick_data = NULL;
void keyclick_set_callback(keyclick_callback cb, void* data)
{
    keyclick_current_callback = cb;
    keyclick_data = data;
}

/* Produce keyclick based upon button and global settings */
void keyclick_click(bool rawbutton, int action)
{
    int button = action;
    static long last_button = BUTTON_NONE;
    bool do_beep = false;

    if (!rawbutton)
        get_action_statuscode(&button);

    /* Settings filters */
    if (
        TP_KEYCLICK
        )
    {
        if (TP_KEYCLICK_REPEATS || !(button & BUTTON_REPEAT))
        {
            /* Button filters */
            if (button != BUTTON_NONE && !(button & BUTTON_REL)
                && !(button & (SYS_EVENT|BUTTON_MULTIMEDIA)) )
            {
                do_beep = true;
            }
        }
        else if ((button & BUTTON_REPEAT) && (last_button == BUTTON_NONE))
        {
            do_beep = true;
        }
    }
    if (button&BUTTON_REPEAT)
        last_button = button;
    else
        last_button = BUTTON_NONE;

    if (do_beep && keyclick_current_callback)
        do_beep = keyclick_current_callback(action, keyclick_data);
    keyclick_current_callback = NULL;

    if (do_beep)
    {
        system_sound_play(SOUND_KEYCLICK);
    }
}

/* Return the ReplayGain mode adjusted by other relevant settings */
static int replaygain_setting_mode(int type)
{
    switch (type)
    {
    case REPLAYGAIN_SHUFFLE:
        type = global_settings.playlist_shuffle ?
            REPLAYGAIN_TRACK : REPLAYGAIN_ALBUM;
    case REPLAYGAIN_ALBUM:
    case REPLAYGAIN_TRACK:
    case REPLAYGAIN_OFF:
    default:
        break;
    }

    return type;
}

/* Return the ReplayGain mode adjusted for display purposes */
int id3_get_replaygain_mode(const struct mp3entry *id3)
{
    if (!id3)
        return -1;

    int type = global_settings.replaygain_settings.type;
    type = replaygain_setting_mode(type);

    return (type != REPLAYGAIN_TRACK && id3->album_gain != 0) ?
        REPLAYGAIN_ALBUM : (id3->track_gain != 0 ? REPLAYGAIN_TRACK : -1);
}

/* Update DSP's replaygain from global settings */
void replaygain_update(void)
{
    struct replaygain_settings settings = global_settings.replaygain_settings;
    settings.type = replaygain_setting_mode(settings.type);
    dsp_replaygain_set_settings(&settings);
}

void format_sound_value_ex(char *buf, size_t buf_sz, int snd, int val, bool skin_token)
{
    int numdec = sound_numdecimals(snd);
    const char *unit = sound_unit(snd);
    int physval = sound_val2phys(snd, val);

    unsigned int factor = ipow(10, numdec);
    if (factor == 0)
    {
        DEBUGF("DIVISION BY ZERO: format_sound_value s:%d v:%d", snd, val);
        factor = 1;
    }
    unsigned int av = abs(physval);
    unsigned int i = av / factor;
    unsigned int d = av - i*factor;

    snprintf(buf, buf_sz, "%s%u%.*s%.*u%s%s", physval < 0 ? "-" : &" "[skin_token],
             i, numdec, ".", numdec, d, &" "[skin_token], skin_token ? "" : unit);
}

/* format a sound value as "-1.05 dB", or " 1.05 dB" */
void format_sound_value(char *buf, size_t buf_sz, int snd, int val)
{
    format_sound_value_ex(buf, buf_sz, snd, val, false);
}


/* Read (up to) a line of text from fd into buffer and return number of bytes
 * read (which may be larger than the number of bytes stored in buffer). If
 * an error occurs, -1 is returned (and buffer contains whatever could be
 * read). A line is terminated by a LF char. Neither LF nor CR chars are
 * stored in buffer.
 */
int read_line(int fd, char* buffer, int buffer_size)
{
    if (!buffer || buffer_size-- <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    unsigned char rdbuf[32];
    off_t rdbufend = 0;
    int rdbufidx = 0;
    int count = 0;
    int num_read = 0;

    while (count < buffer_size)
    {
        if (rdbufidx >= rdbufend)
        {
            rdbufidx = 0;
            rdbufend = read(fd, rdbuf, sizeof (rdbuf));

            if (rdbufend <= 0)
                break;

            num_read += rdbufend;
        }

        int c = rdbuf[rdbufidx++];

        if (c == '\n')
            break;

        if (c == '\r')
            continue;

        buffer[count++] = c;
    }

    rdbufidx -= rdbufend;

    if (rdbufidx < 0)
    {
        /* "put back" what wasn't read from the buffer */
        num_read += rdbufidx;
        rdbufend = lseek(fd, rdbufidx, SEEK_CUR);
    }

    buffer[count] = '\0';

    return rdbufend >= 0 ? num_read : -1;
}

char* skip_whitespace(char* const str)
{
    char *s = str;

    while (isspace(*s))
        s++;

    return s;
}


int confirm_delete_yesno(const char *name, const char *title)
{
    const char *lines[] = { ID2P(LANG_REALLY_DELETE), name };
    const char *yes_lines[] = { ID2P(LANG_DELETING), name };
    const struct text_message message = { lines, 2 };
    const struct text_message yes_message = { yes_lines, 2 };
    return gui_syncyesno_run_w_title(title, &message, &yes_message, NULL);
}

/*  time_split_units()
    split time values depending on base unit
    unit_idx: UNIT_HOUR, UNIT_MIN, UNIT_SEC, UNIT_MS
    abs_value: absolute time value
    units_in: array of unsigned ints with UNIT_IDX_TIME_COUNT fields
*/
unsigned int time_split_units(int unit_idx, unsigned long abs_val,
                              unsigned long (*units_in)[UNIT_IDX_TIME_COUNT])
{
    unsigned int base_idx = UNIT_IDX_HR;
    unsigned long hours;
    unsigned long minutes  = 0;
    unsigned long seconds  = 0;
    unsigned long millisec = 0;

    switch (unit_idx & UNIT_IDX_MASK) /*Mask off upper bits*/
    {
            case UNIT_MS:
                base_idx = UNIT_IDX_MS;
                millisec = abs_val;
                abs_val  = abs_val  /  1000U;
                millisec = millisec - (1000U * abs_val);
                /* fallthrough and calculate the rest of the units */
            case UNIT_SEC:
                if (base_idx == UNIT_IDX_HR)
                    base_idx = UNIT_IDX_SEC;
                seconds  = abs_val;
                abs_val  = abs_val  / 60U;
                seconds  = seconds - (60U * abs_val);
                /* fallthrough and calculate the rest of the units */
            case UNIT_MIN:
                if (base_idx == UNIT_IDX_HR)
                    base_idx = UNIT_IDX_MIN;
                minutes  = abs_val;
                abs_val  = abs_val / 60U;
                minutes  = minutes -(60U * abs_val);
                /* fallthrough and calculate the rest of the units */
            case UNIT_HOUR:
            default:
                hours    = abs_val;
                break;
    }

    (*units_in)[UNIT_IDX_HR]  = hours;
    (*units_in)[UNIT_IDX_MIN] = minutes;
    (*units_in)[UNIT_IDX_SEC] = seconds;
    (*units_in)[UNIT_IDX_MS]  = millisec;

    return base_idx;
}

/* format_time_auto - return an auto ranged time string;
   buffer:  needs to be at least 25 characters for full range

   unit_idx: specifies lowest or base index of the value
   add | UNIT_LOCK_ to keep place holder of units that would normally be
   discarded.. For instance, UNIT_LOCK_HR would keep the hours place, ex: string
   00:10:10 (0 HRS 10 MINS 10 SECONDS) normally it would return as 10:10
   add | UNIT_TRIM_ZERO to supress leading zero on the largest unit

   value: should be passed in the same form as unit_idx

   supress_unit: may be set to true and in this case the
   hr, min, sec, ms identifiers will be left off the resulting string but
   since right to left languages are handled it is advisable to leave units
   as an indication of the text direction
*/

const char *format_time_auto(char *buffer, int buf_len, long value,
                                  int unit_idx, bool supress_unit)
{
    const char * const sign        = &"-"[value < 0 ? 0 : 1];
    bool               is_rtl      = lang_is_rtl();
    char               timebuf[25]; /* -2147483648:00:00.00\0 */
    int                len;
    unsigned char      base_idx, max_idx;

    unsigned long  units_in[UNIT_IDX_TIME_COUNT];
    unsigned char  fwidth[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = 0, /* hr is variable length */
                        [UNIT_IDX_MIN] = 2,
                        [UNIT_IDX_SEC] = 2,
                        [UNIT_IDX_MS]  = 3,
                   }; /* {0,2,2,3}; Field Widths */
    unsigned char  offsets[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = 10,/* ?:59:59.999 Std offsets    */
                        [UNIT_IDX_MIN] = 7, /*0?:+1:+4.+7 need calculated */
                        [UNIT_IDX_SEC] = 4,/* 999.59:59:0  RTL offsets    */
                        [UNIT_IDX_MS]  = 0,/* 0  .4 :7 :10 won't change   */
                   }; /* {10,7,4,0}; Offsets */
    static const uint16_t unitlock[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = UNIT_LOCK_HR,
                        [UNIT_IDX_MIN] = UNIT_LOCK_MIN,
                        [UNIT_IDX_SEC] = UNIT_LOCK_SEC,
                        [UNIT_IDX_MS]  = 0,
                   }; /* unitlock */
    static const uint16_t units[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = UNIT_HOUR,
                        [UNIT_IDX_MIN] = UNIT_MIN,
                        [UNIT_IDX_SEC] = UNIT_SEC,
                        [UNIT_IDX_MS]  = UNIT_MS,
                   }; /* units */


    base_idx = time_split_units(unit_idx, labs(value), &units_in);

    if (units_in[UNIT_IDX_HR] || (unit_idx & unitlock[UNIT_IDX_HR]))
        max_idx = UNIT_IDX_HR;
    else if (units_in[UNIT_IDX_MIN] || (unit_idx & unitlock[UNIT_IDX_MIN]))
        max_idx = UNIT_IDX_MIN;
    else if (units_in[UNIT_IDX_SEC] || (unit_idx & unitlock[UNIT_IDX_SEC]))
        max_idx = UNIT_IDX_SEC;
    else if (units_in[UNIT_IDX_MS])
        max_idx = UNIT_IDX_MS;
    else /* value is 0 */
        max_idx = base_idx;

    if (!is_rtl)
    {
        len = snprintf(timebuf, sizeof(timebuf),
                       "%02lu:%02lu:%02lu.%03lu",
                       units_in[UNIT_IDX_HR],
                       units_in[UNIT_IDX_MIN],
                       units_in[UNIT_IDX_SEC],
                       units_in[UNIT_IDX_MS]);

        fwidth[UNIT_IDX_HR]   = len - offsets[UNIT_IDX_HR];

        /* calculate offsets of the other fields based on length of previous */
        offsets[UNIT_IDX_MS]  = fwidth[UNIT_IDX_HR] + offsets[UNIT_IDX_MIN];
        offsets[UNIT_IDX_SEC] = fwidth[UNIT_IDX_HR] + offsets[UNIT_IDX_SEC];
        offsets[UNIT_IDX_MIN] = fwidth[UNIT_IDX_HR] + 1;
        offsets[UNIT_IDX_HR]  = 0;

        timebuf[offsets[base_idx] + fwidth[base_idx]] = '\0';

        strlcpy(buffer, sign, buf_len);

        /* trim leading zero on the max_idx */
        if ((unit_idx & UNIT_TRIM_ZERO) == UNIT_TRIM_ZERO &&
            timebuf[offsets[max_idx]] == '0' && fwidth[max_idx] > 1)
        {
            offsets[max_idx]++;
        }

        strlcat(buffer, &timebuf[offsets[max_idx]], buf_len);

        if (!supress_unit)
        {
            strlcat(buffer, " ", buf_len);
            strlcat(buffer, unit_strings_core[units[max_idx]], buf_len);
        }
    }
    else /*RTL Languages*/
    {
        len = snprintf(timebuf, sizeof(timebuf),
                       "%03lu.%02lu:%02lu:%02lu",
                       units_in[UNIT_IDX_MS],
                       units_in[UNIT_IDX_SEC],
                       units_in[UNIT_IDX_MIN],
                       units_in[UNIT_IDX_HR]);

        fwidth[UNIT_IDX_HR] = len - offsets[UNIT_IDX_HR];


        /* trim leading zero on the max_idx */
        if ((unit_idx & UNIT_TRIM_ZERO) == UNIT_TRIM_ZERO &&
            timebuf[offsets[max_idx]] == '0' && fwidth[max_idx] > 1)
        {
            timebuf[offsets[max_idx]] = timebuf[offsets[max_idx]+1];
            fwidth[max_idx]--;
        }

        timebuf[offsets[max_idx] + fwidth[max_idx]] = '\0';

        if (!supress_unit)
        {
            strmemccpy(buffer, unit_strings_core[units[max_idx]], buf_len);
            strlcat(buffer, " ", buf_len);
            strlcat(buffer, &timebuf[offsets[base_idx]], buf_len);
        }
        else
            strmemccpy(buffer, &timebuf[offsets[base_idx]], buf_len);

        strlcat(buffer, sign, buf_len);
    }

    return buffer;
}

/* Format time into buf.
 *
 * buf      - buffer to format to.
 * buf_size - size of buffer.
 * t        - time to format, in milliseconds.
 */
void format_time(char* buf, int buf_size, long t)
{
    unsigned long units_in[UNIT_IDX_TIME_COUNT] = {0};
    time_split_units(UNIT_MS, labs(t), &units_in);
    int hashours = units_in[UNIT_IDX_HR] > 0;
    snprintf(buf, buf_size, "%.*s%.0lu%.*s%.*lu:%.2lu",
             t < 0, "-", units_in[UNIT_IDX_HR], hashours, ":",
             hashours+1, units_in[UNIT_IDX_MIN], units_in[UNIT_IDX_SEC]);
}

const char* format_sleeptimer(char* buffer, size_t buffer_size,
                              int value, const char* unit)
{
    (void) unit;
    int minutes, hours;

    if (value) {
        hours = value / 60;
        minutes = value - (hours * 60);
        snprintf(buffer, buffer_size, "%d:%02d", hours, minutes);
        return buffer;
    } else {
        return str(LANG_OFF);
    }
}

static int seconds_to_min(int secs)
{
    return (secs + 10) / 60;  /* round up for 50+ seconds */
}

char* string_sleeptimer(char *buffer, size_t buffer_len)
{
    int sec = get_sleep_timer();
    char timer_buf[10];

    snprintf(buffer, buffer_len, "%s (%s)",
             str(sec ? LANG_SLEEP_TIMER_CANCEL_CURRENT
                 : LANG_SLEEP_TIMER_START_CURRENT),
             format_sleeptimer(timer_buf, sizeof(timer_buf),
                sec ? seconds_to_min(sec)
                    : global_settings.sleeptimer_duration, NULL));
    return buffer;
}

/* If a sleep timer is running, cancel it, otherwise start one */
int toggle_sleeptimer(void)
{
    set_sleeptimer_duration(get_sleep_timer() ? 0
                    : global_settings.sleeptimer_duration);
    return 0;
}


/**
 * Splits str at each occurence of split_char and puts the substrings into vector,
 * but at most vector_lenght items. Empty substrings are ignored.
 *
 * Modifies str by replacing each split_char following a substring with nul
 *
 * Returns the number of substrings found, i.e. the number of valid strings
 * in vector
 */
int split_string(char *str, const char split_char, char *vector[], const int vector_length)
{
    int i;
    char sep[2] = {split_char, '\0'};
    char *e, *p = strtok_r(str, sep, &e);

    /* strtok takes care of leading & trailing splitters */
    for(i = 0; i < vector_length; i++)
    {
        vector[i] = p;
        if (!p)
            break;
        p = strtok_r(NULL, sep, &e);
    }

    return i;
}

/* returns match index from option list
 * returns -1 if option was not found
 * option list is array of char pointers with the final item set to null
 * ex - const char * const option[] = { "op_a", "op_b", "op_c", NULL}
 */
int string_option(const char *option, const char *const oplist[], bool ignore_case)
{
    const char *op;
    int (*cmp_fn)(const char*, const char*) = &strcasecmp;
    if (!ignore_case)
        cmp_fn = strcmp;
    for (int i=0; (op=oplist[i]) != NULL; i++)
    {
        if (cmp_fn(op, option) == 0)
            return i;
    }
    return -1;
}

/* Make sure part of path only contain chars valid for a FAT32 long name.
 * Double quotes are replaced with single quotes, other unsupported chars
 * are replaced with an underscore.
 *
 * path   - path to modify.
 * offset - where in path to start checking.
 * count  - number of chars to check.
 */
void fix_path_part(char* path, int offset, int count)
{
    static const char invalid_chars[] = "*/:<>?\\|";
    int i;

    path += offset;

    for (i = 0; i <= count; i++, path++)
    {
        if (*path == 0)
            return;
        if (*path == '"')
            *path = '\'';
        else if (strchr(invalid_chars, *path))
            *path = '_';
    }
}

/* open but with a builtin printf for assembling the path */
int open_pathfmt(char *buf, size_t size, int oflag, const char *pathfmt, ...)
{
    va_list ap;
    va_start(ap, pathfmt);
    vsnprintf(buf, size, pathfmt, ap);
    va_end(ap);
    if ((oflag & O_PATH) == O_PATH)
        return -1;
    return open(buf, oflag, 0666);
}

/** Open a UTF-8 file and set file descriptor to first byte after BOM.
 *  If no BOM is present this behaves like open().
 *  If the file is opened for writing and O_TRUNC is set, write a BOM to
 *  the opened file and leave the file pointer set after the BOM.
 */

int open_utf8(const char* pathname, int flags)
{
    ssize_t ret;
    int fd;
    unsigned char bom[BOM_UTF_8_SIZE];

    fd = open(pathname, flags, 0666);
    if(fd < 0)
        return fd;

    if(flags & (O_TRUNC | O_WRONLY))
    {
        ret = write(fd, BOM_UTF_8, BOM_UTF_8_SIZE);
    }
    else
    {
        ret = read(fd, bom, BOM_UTF_8_SIZE);
        /* check for BOM */
        if (ret == BOM_UTF_8_SIZE)
        {
            if(memcmp(bom, BOM_UTF_8, BOM_UTF_8_SIZE))
                lseek(fd, 0, SEEK_SET);
        }
    }
    /* read or write failure, do not continue */
    if (ret < 0)
        close(fd);

    return ret >= 0 ? fd : -1;
}


#ifdef HAVE_LCD_COLOR
/*
 * Helper function to convert a string of 6 hex digits to a native colour
 */

static int hex2dec(int c)
{
    return  (((c) >= '0' && ((c) <= '9')) ? (c) - '0' :
                                            (toupper(c)) - 'A' + 10);
}

int hex_to_rgb(const char* hex, int* color)
{
    int red, green, blue;
    int i = 0;

    while ((i < 6) && (isxdigit(hex[i])))
        i++;

    if (i < 6)
        return -1;

    red = (hex2dec(hex[0]) << 4) | hex2dec(hex[1]);
    green = (hex2dec(hex[2]) << 4) | hex2dec(hex[3]);
    blue = (hex2dec(hex[4]) << 4) | hex2dec(hex[5]);

    *color = LCD_RGBPACK(red,green,blue);

    return 0;
}
#endif /* HAVE_LCD_COLOR */

/* '0'-'3' are ASCII 0x30 to 0x33 */
#define is0123(x) (((x) & 0xfc) == 0x30)
bool parse_color(enum screen_type screen, char *text, int *value)
{
    (void)text; (void)value; /* silence warnings on mono bitmap */
    (void)screen;

#ifdef HAVE_LCD_COLOR
    if (screens[screen].depth > 2)
    {
        if (hex_to_rgb(text, value) < 0)
            return false;
        else
            return true;
    }
#endif

#if LCD_DEPTH == 2
    if (screens[screen].depth == 2)
    {
        if (text[1] != '\0' || !is0123(*text))
            return false;
        *value = *text - '0';
        return true;
    }
#endif
    return false;
}




#define MAX_ACTIVITY_DEPTH 12
static enum current_activity
        current_activity[MAX_ACTIVITY_DEPTH] = {ACTIVITY_UNKNOWN};
static int current_activity_top = 0;

static void push_current_activity_refresh(enum current_activity screen, bool refresh)
{
    current_activity[current_activity_top++] = screen;
    FOR_NB_SCREENS(i)
    {
        skinlist_set_cfg(i, NULL);
        if (refresh)
            skin_update(CUSTOM_STATUSBAR, i, SKIN_REFRESH_ALL);
    }
    if (refresh)
        sb_skin_force_next_update();
}

static void pop_current_activity_refresh(bool refresh)
{
    current_activity_top--;
    FOR_NB_SCREENS(i)
    {
        skinlist_set_cfg(i, NULL);
        if (refresh)
            skin_update(CUSTOM_STATUSBAR, i, SKIN_REFRESH_ALL);
    }
    if (refresh)
        sb_skin_force_next_update();
}

void push_current_activity(enum current_activity screen)
{
    push_current_activity_refresh(screen, true);
}

void push_activity_without_refresh(enum current_activity screen)
{
    push_current_activity_refresh(screen, false);
}

void pop_current_activity(void)
{
    pop_current_activity_refresh(true);
}

void pop_current_activity_without_refresh(void)
{
    pop_current_activity_refresh(false);
}

enum current_activity get_current_activity(void)
{
    return current_activity[current_activity_top?current_activity_top-1:0];
}

/* core_load_bmp opens bitmp filename and allocates space for it
*  you must set bm->data with the result from core_get_data(handle)
*  you must also call core_free(handle) when finished with the bitmap
*  returns handle, ALOC_ERR(0) on failure
* ** Extended error info truth table **
*  [ Handle ][buf_reqd]
*  [  > 0   ][  > 0   ] buf_reqd indicates how many bytes were used
*  [ALOC_ERR][  > 0   ] buf_reqd indicates how many bytes are needed
*  [ALOC_ERR][READ_ERR] there was an error reading the file or it is empty
*/
int core_load_bmp(const char * filename, struct bitmap *bm, const int bmformat,
                  ssize_t *buf_reqd, struct buflib_callbacks *ops)
{
    ssize_t buf_size;
    ssize_t size_read = 0;
    int handle = CLB_ALOC_ERR;

    int fd = open(filename, O_RDONLY);
    *buf_reqd = CLB_READ_ERR;

    if (fd < 0) /* Exit if file opening failed */
    {
        DEBUGF("read_bmp_file: can't open '%s', rc: %d\n", filename, fd);
        return CLB_ALOC_ERR;
    }

    buf_size = read_bmp_fd(fd, bm, 0, bmformat|FORMAT_RETURN_SIZE, NULL);

    if (buf_size > 0)
    {
        handle = core_alloc_ex(buf_size, ops);
        if (handle > 0)
        {
            bm->data = core_get_data_pinned(handle);
            lseek(fd, 0, SEEK_SET); /* reset to beginning of file */
            size_read = read_bmp_fd(fd, bm, buf_size, bmformat, NULL);

            if (size_read > 0) /* free unused alpha channel, if any */
            {
                core_shrink(handle, bm->data, size_read);
                *buf_reqd = size_read;
            }

            core_put_data_pinned(bm->data);
            bm->data = NULL; /* do this to force a crash later if the
                                    caller doesnt call core_get_data() */
        }
        else
            *buf_reqd = buf_size; /* couldn't allocate pass bytes needed */

        if (size_read <= 0)
        {
            /* error reading file */
            core_free(handle); /* core_free() ignores free handles (<= 0) */
            handle = CLB_ALOC_ERR;
        }
    }

    close(fd);
    return handle;
}

/*
 * Normalized volume routines adapted from alsamixer volume_mapping.c
 */
/*
 * Copyright (c) 2010 Clemens Ladisch <clemens@ladisch.de>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/*
 * "The mapping is designed so that the position in the interval is proportional
 * to the volume as a human ear would perceive it (i.e., the position is the
 * cubic root of the linear sample multiplication factor).  For controls with
 * a small range (24 dB or less), the mapping is linear in the dB values so
 * that each step has the same size visually.  Only for controls without dB
 * information, a linear mapping of the hardware volume register values is used
 * (this is the same algorithm as used in the old alsamixer)."
 */

#define NVOL_FRACBITS 16
#define NVOL_UNITY    (1L << NVOL_FRACBITS)

#define nvol_div(x,y) fp_div((x), (y), NVOL_FRACBITS)
#define nvol_mul(x,y) fp_mul((x), (y), NVOL_FRACBITS)
#define nvol_exp10(x) fp_exp10((x), NVOL_FRACBITS)
#define nvol_log10(x) fp_log10((x), NVOL_FRACBITS)

static long get_nvol_factor(void)
{
    long factor = 600L << NVOL_FRACBITS;
    long numdecimals = sound_numdecimals(SOUND_VOLUME);

    if (numdecimals == 0)
        factor /= 10;

    /* nothing *actually* needs this, but: */
    while (numdecimals-- > 1)
        factor *= 10;

    return factor;
}

long to_normalized_volume(long vol, long min_vol, long max_vol, long max_norm)
{
    long norm, min_norm;
    long factor = get_nvol_factor();

    vol <<= NVOL_FRACBITS;
    min_vol <<= NVOL_FRACBITS;
    max_vol <<= NVOL_FRACBITS;
    max_norm <<= NVOL_FRACBITS;

    min_norm = nvol_exp10(nvol_div(min_vol - max_vol, factor));
    norm = nvol_exp10(nvol_div(vol - max_vol, factor));
    norm = nvol_div(norm - min_norm, NVOL_UNITY - min_norm);

    return nvol_mul(norm, max_norm) >> NVOL_FRACBITS;
}

long from_normalized_volume(long norm, long min_vol, long max_vol, long max_norm)
{
    long vol, min_norm;
    long factor = get_nvol_factor();

    norm <<= NVOL_FRACBITS;
    min_vol <<= NVOL_FRACBITS;
    max_vol <<= NVOL_FRACBITS;
    max_norm <<= NVOL_FRACBITS;

    vol = nvol_div(norm, max_norm);

    min_norm = nvol_exp10(nvol_div(min_vol - max_vol, factor));
    vol = nvol_mul(vol, NVOL_UNITY - min_norm) + min_norm;
    vol = nvol_mul(nvol_log10(vol), factor) + max_vol;

    return vol >> NVOL_FRACBITS;
}

void clear_screen_buffer(bool update)
{
    struct viewport vp;
    struct viewport *last_vp;
    FOR_NB_SCREENS(i)
    {
        struct screen * screen = &screens[i];
        viewport_set_defaults(&vp, screen->screen_type);
        last_vp = screen->set_viewport(&vp);
        screen->clear_viewport();
        if (update) {
            screen->update_viewport();
        }
        screen->set_viewport(last_vp);
    }
}

void validate_start_directory_init(void) /* INIT_ATTR */
{
    char * const dirpath = global_settings.start_directory;
    char *slash = strrchr(dirpath, PATH_SEPCH);

    if (!slash)
    {
        path_append(dirpath, PATH_ROOTSTR, PA_SEP_HARD,
                    sizeof(global_settings.start_directory));
        return; /* if this doesn't exist we have bigger issues */
    }
    else if(slash[1] != '\0') /* ending slash required */
    {
        path_append(dirpath, dirpath, PA_SEP_HARD,
                    sizeof(global_settings.start_directory));
    }
    if (slash != dirpath && !dir_exists(dirpath))
    {
        path_append(dirpath, PATH_ROOTSTR,
            PA_SEP_HARD, sizeof(global_settings.start_directory));
    }

    if (!TP_KEEP_DIRECTORY) /* reset to root / if !keep_directory */
    {
        path_append(global_status.browse_last_folder, PATH_ROOTSTR, PA_SEP_HARD,
                    sizeof(global_status.browse_last_folder));
    }
}

