/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Stuart Martin
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

#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <stdbool.h>
#include <stddef.h>
#include "inttypes.h"
#include "config.h"
#include "audiohw.h" /* for the AUDIOHW_* defines */
#include "statusbar.h" /* for the statusbar values */
#include "button.h"
#include "audio.h"
#include "dsp_proc_settings.h"

struct opt_items {
    unsigned const char* string;
};

/** Setting values defines **/
#define MAX_FILENAME 32
#define MAX_PATHNAME 80
#define MAX_PATHLIST (MAX_PATHNAME*2)

/* The values are assigned to the enums so that they correspond to */
/* setting values in settings_list.c                               */

enum
{
    TRIG_TYPE_STOP = 0,
    TRIG_TYPE_PAUSE,
    TRIG_TYPE_NEW_FILE
};

enum {
    PLAYLIST_VIEWER_ENTRY_SHOW_FILE_NAME = 0,
    PLAYLIST_VIEWER_ENTRY_SHOW_FULL_PATH = 1,
    PLAYLIST_VIEWER_ENTRY_SHOW_ID3_TITLE_AND_ALBUM = 2,
    PLAYLIST_VIEWER_ENTRY_SHOW_ID3_TITLE = 3
};

#ifdef HAVE_CROSSFADE
enum {
    CROSSFADE_ENABLE_OFF = 0,
    CROSSFADE_ENABLE_AUTOSKIP,
    CROSSFADE_ENABLE_MANSKIP,
    CROSSFADE_ENABLE_SHUFFLE,
    CROSSFADE_ENABLE_SHUFFLE_OR_MANSKIP,
    CROSSFADE_ENABLE_ALWAYS,
};
#endif

enum {
    FOLDER_ADVANCE_OFF = 0,
    FOLDER_ADVANCE_NEXT,
    FOLDER_ADVANCE_RANDOM,
};

/* repeat mode options */
enum
{
    REPEAT_OFF = 0,
    REPEAT_ALL,
    REPEAT_ONE,
#ifdef AB_REPEAT_ENABLE
    REPEAT_AB,
#endif
    NUM_REPEAT_MODES
};

/* single mode options */
enum {
    SINGLE_MODE_OFF = 0,
    SINGLE_MODE_TRACK,
    SINGLE_MODE_ALBUM,
    SINGLE_MODE_ALBUM_ARTIST,
    SINGLE_MODE_ARTIST,
    SINGLE_MODE_COMPOSER,
    SINGLE_MODE_GROUPING,
    SINGLE_MODE_GENRE
};

enum
{
    QUEUE_HIDE = 0,
    QUEUE_SHOW_AT_TOPLEVEL,
    QUEUE_SHOW_IN_SUBMENU
};



/* dir filter options */
/* Note: Any new filter modes need to be added before NUM_FILTER_MODES.
 *       Any new rockbox browse filter modes (accessible through the menu)
 *       must be added after NUM_FILTER_MODES. */
enum { SHOW_ALL, SHOW_SUPPORTED, SHOW_MUSIC, SHOW_PLAYLIST, SHOW_ID3DB,
       NUM_FILTER_MODES,
       SHOW_WPS, SHOW_RWPS, SHOW_FMS, SHOW_RFMS, SHOW_SBS, SHOW_RSBS, SHOW_FMR, SHOW_CFG,
       SHOW_LNG, SHOW_MOD, SHOW_FONT, SHOW_PLUGINS, SHOW_M3U};

/* file and dir sort options */
enum { SORT_ALPHA, SORT_DATE, SORT_DATE_REVERSED, SORT_TYPE, /* available as settings */
       SORT_ALPHA_REVERSED, SORT_TYPE_REVERSED, SORT_AS_FILE }; /* internal use only */
enum { SORT_INTERPRET_AS_DIGIT, SORT_INTERPRET_AS_NUMBER };

/* recursive dir insert options */
enum { RECURSE_OFF, RECURSE_ON, RECURSE_ASK };

/* show path types */
enum { SHOW_PATH_OFF = 0, SHOW_PATH_CURRENT, SHOW_PATH_FULL };

/* scrollbar visibility/position */
enum { SCROLLBAR_OFF = 0, SCROLLBAR_LEFT, SCROLLBAR_RIGHT };


/* Alarm settings */

/* Keyclick stuff */

 /* Not really a setting but several files should stay synced */
#define KEYCLICK_DURATION 2

/** virtual pointer stuff.. move to another .h maybe? **/
/* These define "virtual pointers", which could either be a literal string,
   or a mean a string ID if the pointer is in a certain range.
   This helps to save space for menus and options.

   While using 0 as the base address is fastest/simplest,
   it could result in a nullptr when ID==0 which C compilers try to
   helpfully "optimize" away.

   NOTE:  VIRT_PTR must point at an *invalid* address in the target
          memory map!
*/

#define VIRT_SIZE 0xFFFF /* more than enough for our string ID range */
/* offset from 0x0 slightly */
#define VIRT_PTR ((unsigned char*)sizeof(char*))

/* form a "virtual pointer" out of a language ID */
#define ID2P(id) (VIRT_PTR + id)

/* resolve a pointer which could be a virtualized ID or a literal */
#define P2STR(p) (char *)((p>=VIRT_PTR && p<VIRT_PTR+VIRT_SIZE) ? str(p-VIRT_PTR) : p)

/* get the string ID from a virtual pointer, -1 if not virtual */
#define P2ID(p) ((p>=VIRT_PTR && p<VIRT_PTR+VIRT_SIZE) ? p-VIRT_PTR : -1)

/* Unit identifiers used by the display formatters (output_dyn_value,
   format_time_auto, set_int, ...). These index unit_strings_core[]. */
enum {
    /* unit definitions for use with the formatters */
    UNIT_INT = 1, /* plain number */
    UNIT_SIGNED,  /* number with mandatory sign (even if positive) */
    UNIT_MS,      /* milliseconds */
    UNIT_SEC,     /* seconds */
    UNIT_MIN,     /* minutes */
    UNIT_HOUR,    /* hours */
    UNIT_KHZ,     /* kHz */
    UNIT_DB,      /* dB, mandatory sign */
    UNIT_PERCENT, /* % */
    UNIT_MAH,     /* milliAmp hours */
    UNIT_PIXEL,   /* pixels */
    UNIT_PER_SEC, /* per second */
    UNIT_HERTZ,   /* hertz */
    UNIT_MB,      /* Megabytes */
    UNIT_KBIT,    /* kilobits per sec */
    UNIT_PM_TICK, /* peak meter units per tick */
    UNIT_TIME,    /* time duration/interval in seconds */
    UNIT_DATEYEAR,/* a year */
    UNIT_LAST     /* END MARKER */
};

/* convenience macro to have both virtual pointer and ID as arguments */
#define STR(id) ID2P(id), id

/* !defined(HAVE_LCD_COLOR) implies HAVE_LCD_CONTRAST with default 40.
   Explicitly define HAVE_LCD_CONTRAST in config file for newer ports for
   simplicity. */



/** function prototypes **/
void reset_runtime(void);
void update_runtime(void);
void zero_runtime(void);
void settings_load(void) INIT_ATTR;
bool settings_load_config(const char* file, bool apply);

void status_save(bool force);
int settings_save(void);

/* defines for the options paramater */
enum {
    SETTINGS_SAVE_CHANGED = 0,
    SETTINGS_SAVE_ALL,
    SETTINGS_SAVE_THEME,
    SETTINGS_SAVE_SOUND,
    SETTINGS_SAVE_EQPRESET,
    SETTINGS_SAVE_RESUMEINFO,
};
bool settings_save_config(int options);

struct settings_list;
struct filename_setting;
void reset_setting(const struct settings_list *setting, void *var);
void settings_reset(void);
void sound_settings_apply(void);

/* call this after loading a .wps/.rwps or other skin files, so that the
 * skin buffer is reset properly
 */
void settings_apply_skins(void);
/* Live-recolour loaded skins' default colours without a reparse (no audio stop). */
void skin_update_bg_color(unsigned old_bg, unsigned new_bg);
void skin_update_fg_color(unsigned old_fg, unsigned new_fg);

void settings_apply(bool read_disk);
void settings_display(void);

enum optiontype { RB_INT, RB_BOOL };

const struct settings_list* find_setting(const void* variable);
const struct settings_list* find_setting_by_cfgname(const char* name);
bool cfg_int_to_string(const struct settings_list *setting, int val, char* buf, int buf_len);
bool cfg_string_to_int(const struct settings_list *setting, int* out, const char* str);
void cfg_to_string(const struct settings_list *setting, char* buf, int buf_len);
bool string_to_cfg(const char *name, char* value, bool *theme_changed);

bool copy_filename_setting(char *buf, size_t buflen, const char *input,
                           const struct filename_setting *fs);
bool set_bool_options(const char* string, const bool* variable,
                      const char* yes_str, int yes_voice,
                      const char* no_str, int no_voice,
                      void (*function)(bool));

bool set_bool(const char* string, const bool* variable);
bool set_int(const unsigned char* string, int unit,
             const int* variable,
             void (*function)(int), int step, int min, int max,
             const char* (*formatter)(char*, size_t, int, const char*) );

bool set_int_ex(const unsigned char* string, int unit,
             const int* variable,
             void (*function)(int), int step, int min, int max,
             const char* (*formatter)(char*, size_t, int, const char*));

void set_file(const char* filename, char* setting);

bool set_option(const char* string, const void* variable, enum optiontype type,
                const struct opt_items* options, int numoptions, void (*function)(int));

const char* setting_get_cfgvals(const struct settings_list *setting);

/** global_settings and global_status struct definitions **/

struct system_status
{
    int volume;     /* audio output volume in decibels range depends on the dac */
    int resume_index;  /* index in playlist (-1 for no active resume) */
    uint32_t resume_crc32; /* crc32 of the name of the file */
    uint32_t resume_elapsed; /* elapsed time in last file */
    uint32_t resume_offset; /* byte offset in mp3 file */
    int runtime;       /* current runtime since last charge */
    int topruntime;    /* top known runtime */
    int last_screen;
    int last_browser;
    int  viewer_icon_count;
    int last_volume_change; /* tick the last volume change happened. skins use this */
    int font_id[NB_SCREENS]; /* font id of the settings font for each screen */

    int resume_modified; /* playlist is modified (=> warn before erase) */
    char browse_last_folder[MAX_PATH];/* last visited file-browser folder */
};

#define TRIMPOD_MAX_FILES_IN_DIR      5000  /* file-browser dir cache size */
#define TRIMPOD_MAX_FILES_IN_PLAYLIST 100000 /* queue index slots (8 B each) */
#define TRIMPOD_GLYPHS_TO_CACHE       512   /* font glyph allocation (CJK lists thrash 250) */

struct user_settings
{
    /* audio settings */
    int balance;    /* stereo balance: -100 - +100 -100=left  0=bal +100=right  */
    int bass;       /* bass boost/cut in decibels                               */
    int treble;     /* treble boost/cut in decibels                             */
    int channel_config; /* Stereo, Mono, Custom, Mono left, Mono right, Karaoke */
    int stereo_width; /* 0-255% */

#ifdef AUDIOHW_HAVE_BASS_CUTOFF
    int bass_cutoff;
#endif
#ifdef AUDIOHW_HAVE_TREBLE_CUTOFF
    int treble_cutoff;
#endif

#ifdef HAVE_CROSSFADE
    /* Crossfade */
    int crossfade;     /* Enable crossfade (0=off, 1=shuffle, 2=trackskip,
                                            3=shuff&trackskip, 4=always) */
    int crossfade_fade_in_delay;      /* Fade in delay (0-15s)             */
    int crossfade_fade_out_delay;     /* Fade out delay (0-15s)            */
    int crossfade_fade_in_duration;   /* Fade in duration (0-15s)          */
    int crossfade_fade_out_duration;  /* Fade out duration (0-15s)         */
    int crossfade_fade_out_mixmode;   /* Fade out mode (0=crossfade,1=mix) */
#endif

    /* Replaygain */
    struct replaygain_settings replaygain_settings;

    /* Crossfeed */
    int crossfeed;                              /* crossfeed type */
    unsigned int crossfeed_direct_gain;         /* dB x 10 */
    unsigned int crossfeed_cross_gain;          /* dB x 10 */
    unsigned int crossfeed_hf_attenuation;      /* dB x 10 */
    unsigned int crossfeed_hf_cutoff;           /* Frequency in Hz */

    /* EQ */
    bool eq_enabled;            /* Enable equalizer */
    unsigned int eq_precut;     /* dB */
    struct eq_band_setting eq_band_settings[EQ_NUM_BANDS]; /* for each band */

    /* Misc. swcodec */
    int  beep;              /* system beep volume when changing tracks etc. */
    bool dithering_enabled;



    /* misc options */
    int list_accel_start_delay; /* ms before we start increaseing step size */
    int list_accel_wait; /* ms between increases */



    int  pause_rewind; /* time in s to rewind when pausing */


    int timeformat;    /* time format: 0=24 hour clock, 1=12 hour clock */


    int default_codepage;   /* set default codepage for tag conversion */
    bool hold_lr_for_scroll_in_list; /* hold L/R scrolls the list left/right */
    bool play_selected; /* Plays selected file even in shuffle mode */
    int single_mode;    /* single mode - stop after every track, album, album artist,
                           artist, composer, work, or genre */
    bool cuesheet;
    int ff_rewind_min_step; /* FF/Rewind minimum step size */
    int ff_rewind_accel; /* FF/Rewind acceleration (in seconds per doubling) */

    unsigned char wps_file[MAX_FILENAME+1];  /* last wps */
    unsigned char sbs_file[MAX_FILENAME+1];  /* last statusbar skin */
    unsigned char lang_file[MAX_FILENAME+1]; /* last language */
    unsigned char playlist_catalog_dir[MAX_PATHNAME+1];
    int skip_length; /* skip length */
    int volume_type;   /* how volume is displayed: 0=graphic, 1=percent */
    int battery_display; /* how battery is displayed: 0=graphic, 1=percent */
    int statusbar;    /* STATUSBAR_* enum values */

    int scrollbar;    /* SCROLLBAR_* enum values */
    int scrollbar_width;

#if LCD_DEPTH > 1
    int list_separator_height; /* -1=auto (== 1 currently), 0=disabled, X=height in pixels */
    int list_separator_color;
#endif
    bool scroll_paginated; /* 0=dont 1=do */
    bool list_wraparound;  /* wrap around to opposite end of list when scrolling */
    int  list_order;       /* order for numeric lists (ascending or descending) */
    int  scroll_speed;     /* long texts scrolling speed: 1-30 */
    int  bidir_limit;      /* bidir scroll length limit */
    int  scroll_delay;     /* delay (in 1/10s) before starting scroll */
    int  scroll_step;      /* pixels to advance per update */



#if LCD_DEPTH > 1
    unsigned char backdrop_file[MAX_PATHNAME+1];  /* backdrop bitmap file */
#endif

#ifdef HAVE_LCD_COLOR
    int bg_color; /* background color native format */
    int fg_color; /* foreground color native format */
    int lss_color; /* background color for the selector or start color for the gradient */
    int lse_color; /* end color for the selector gradient */
    int lst_color; /* color of the text for the selector */
    unsigned char colors_file[MAX_FILENAME+1];
#endif

    /* playlist/playback settings */
    int  repeat_mode; /* 0=off 1=repeat all 2=repeat one 3=shuffle 4=ab */
    int  next_folder; /* move to next folder */
    bool constrain_next_folder; /* whether next_folder is constrained to
                                   directories within start_directory */
    bool fade_on_stop; /* fade on pause/unpause/stop */
    bool playlist_shuffle;
    bool rewind_across_tracks;

    /* power settings */
    int poweroff;   /* idle power off timer */

    /* Trimpod visualizer */
    int viz_start_delay;  /* WPS idle seconds before auto-starting the visualizer (0=never) */
    int viz_transition;   /* seconds each preset shows before switching to a random next */
#if BATTERY_CAPACITY_INC > 0
    int battery_capacity; /* in mAh */
#endif
    /* device settings */

    int  cursor_style; /* style of the selection cursor */
    int  screen_scroll_step;
    bool offset_out_of_view;
    bool disable_mainmenu_scrolling;
    unsigned char viewers_icon_file[MAX_FILENAME+1];
    unsigned char font_file[MAX_FILENAME+1]; /* last font */
    unsigned char kbd_file[MAX_FILENAME+1];  /* last keyboard */
    int  backlight_timeout;  /* backlight off timeout:  -1=never,
                                0=always, or time in seconds */
    bool caption_backlight; /* turn on backlight at end and start of track */
    bool bl_filter_first_keypress;   /* filter first keypress when dark? */
#if CONFIG_CHARGING
    int backlight_timeout_plugged;
#endif
#ifndef HAS_BUTTON_HOLD
    bool bt_selective_softlock_actions;
    int bt_selective_softlock_actions_mask;
#endif
    bool bl_selective_actions; /* backlight disable on some actions */
    int  bl_selective_actions_mask;/* mask of actions that will not enable backlight */
    int backlight_on_button_hold; /* what to do with backlight when hold
                                     switch is on */

#if   defined(HAVE_BACKLIGHT_FADING_BOOL_SETTING)
    bool backlight_fade_in;
    bool backlight_fade_out;
#endif
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    int brightness;
#endif




    bool prevent_skip;


    /* If values are just added to the end, no need to bump plugin API
       version. */
    /* new stuff to be added at the end */




    unsigned char ui_vp_config[64]; /* viewport string for the lists */

    struct compressor_settings compressor_settings;

    int sleeptimer_duration; /* In minutes */


    /* When resuming playback (after a stop), rewind this number of seconds */
    int resume_rewind;

#ifdef AUDIOHW_HAVE_DEPTH_3D
    int depth_3d;
#endif

#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    int roll_off;
#endif

#ifdef AUDIOHW_HAVE_POWER_MODE
    int power_mode;
#endif

#ifdef AUDIOHW_HAVE_EQ
    /** Hardware EQ tone controls **/
    struct hw_eq_band
    {
        /* Maintain the order of members or sound_menu has to be changed */
        int gain;
#ifdef AUDIOHW_HAVE_EQ_FREQUENCY
        int frequency;
#endif
#ifdef AUDIOHW_HAVE_EQ_WIDTH
        int width;
#endif
    } hw_eq_bands[AUDIOHW_EQ_BAND_NUM];
#endif /* AUDIOHW_HAVE_EQ */


    char start_directory[MAX_PATHNAME+1];
    /* Has the root been customized from the .cfg file? false = no, true = loaded from cfg */
    bool root_menu_customized;

    int volume_limit; /* maximum volume limit */

#ifdef HAVE_PERCEPTUAL_VOLUME
    int volume_adjust_mode;
    int volume_adjust_norm_steps;
#endif

    int surround_enabled;
    int surround_balance;
    int surround_fx1;
    int surround_fx2;
    bool surround_method2;
    int surround_mix;

    int pbe;
    int pbe_precut;

    int afr_enabled;

#if defined(BUTTON_REC) || \
    (CONFIG_KEYPAD == GIGABEAT_PAD) || \
    (CONFIG_KEYPAD == IPOD_4G_PAD) || \
    (CONFIG_KEYPAD == IRIVER_H10_PAD)
    bool clear_settings_on_hold;
#endif
};

/* Trimpod: the stock "Other" (General Settings) menus were removed -- these
 * formerly-user-facing settings are baked in at their read sites (no serialized
 * setting). Values are the prior defaults, except warn-on-erase (off: no
 * iPod-style prompt). Sorting is fixed to folders-first natural-alpha. */
#define TP_SHOW_FILENAME_EXT         3              /* file browser ext display */
#define TP_SHOW_PATH_IN_BROWSER      SHOW_PATH_CURRENT
#define TP_KEEP_DIRECTORY            true           /* resume last browse folder */
#define TP_RECURSIVE_DIR_INSERT      RECURSE_ON
#define TP_WARN_ON_ERASE             false          /* no erase-dynamic-playlist prompt */
#define TP_KEYCLICK                  0              /* off */
#define TP_KEYCLICK_REPEATS          false
#define TP_PL_VIEWER_INDICES         true
#define TP_PL_VIEWER_TRACK_DISPLAY   0
#define TP_SORT_PLAYLISTS            0              /* alpha */
#define TP_DIRFILTER                 SHOW_SUPPORTED /* file browser show-files */
#define TP_SORT_DIR                  SORT_ALPHA     /* folders sorted alpha */

/* global settings */
extern struct user_settings global_settings;
/* global status */
extern struct system_status global_status;

#endif /* __SETTINGS_H__ */
