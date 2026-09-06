/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 Jonathan Gordon
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
#include <stdbool.h>
#include "string-extra.h"
#include "system.h"
#include "storage.h"
#include "lang.h"
#include "lcd.h"
#include "scroll_engine.h"
#include "button.h"
#include "backlight.h"
#include "sound.h"
#include "pcm_sink.h"
#include "settings.h"
#include "rbpaths.h"
#include "settings_list.h"
#include "usb.h"
#include "audio.h"
#include "power.h"
#include "powermgmt.h"
#include "kernel.h"
#include "misc.h"
#include "playback.h"
#include "list.h"
#include "rbunicode.h"
#include "menus/eq_menu.h"
#include "statusbar.h"

#include "misc.h" /* current activity */

#include "playlist.h"
#include "tree.h"
#include "iap-usb.h"



#define UNUSED {.RESERVED=NULL}
#define INT(a) {.int_ = a}
#define UINT(a) {.uint_ = a}
#define BOOL(a) {.bool_ = a}
#define CHARPTR(a) {.charptr = a}
#define UCHARPTR(a) {.ucharptr = a}
#define FUNCTYPE(a) {.func = a}
#define NODEFAULT INT(0)

/* in all the following macros the args are:
    - flags: bitwise | or the F_ bits in settings_list.h
    - var: pointer to the variable being changed (usually in global_settings)
    - lang_id: LANG_* id to display in menus and setting screens for the setting
    - default: the default value for the variable, set if settings are reset
    - name: the name of the setting in config files
    - cfg_vals: comma separated list of legal values to write to cfg files.
                The values correspond to the values 0,1,2,etc. of the setting.
                NULL if just the number itself should be written to the file.
                No spaces between the values and the commas!
    - cb: the callback used by the setting screen.
*/

/* Use for int settings which use the set_sound() function to set them */
#define SOUND_SETTING(flags,var,lang_id,name,setting)                       {flags|F_T_INT|F_T_SOUND|F_SOUNDSETTING|F_ALLOW_ARBITRARY_VALS, &global_settings.var,  lang_id, NODEFAULT,name,                               {.sound_setting=(struct sound_setting[]){{setting}}} }

/* Use for bool variables which don't use LANG_SET_BOOL_YES and LANG_SET_BOOL_NO
      or dont save as "off" or "on" in the cfg.
   cfgvals are comma separated values (without spaces after the comma!) to write
      for 'false' and 'true' (in this order)
   yes_id is the lang_id for the 'yes' (or 'on') option in the menu
   no_id is the lang_id for the 'no' (or 'off') option in the menu
 */
#define BOOL_SETTING(flags,var,lang_id,default,name,cfgvals,yes_id,no_id,cb) {flags|F_BOOL_SETTING, &global_settings.var,                     lang_id, BOOL(default),name,                                 {.bool_setting=(struct bool_setting[]){{cb,yes_id,no_id,cfgvals}}} }

/* bool setting which does use LANG_YES and _NO and save as "off,on" */
#define OFFON_SETTING(flags,var,lang_id,default,name,cb)                     BOOL_SETTING(flags,var,lang_id,default,name,off_on,              LANG_SET_BOOL_YES,LANG_SET_BOOL_NO,cb)

/*system_status int variable which is saved to resume.cfg */
#define SYSTEM_STATUS(flags,var,default,name)                 {flags|F_RESUMESETTING|F_T_INT, &global_status.var,-1,  INT(default), name, UNUSED}

#define SYSTEM_STATUS_TEXT_SETTING(flags,var,name,default,prefix,suffix)  {flags|F_RESUMESETTING|F_T_UCHARPTR, &global_status.var,-1,   CHARPTR(default),name,                                    {.filename_setting=                                       (struct filename_setting[]){                          {prefix,suffix,sizeof(global_status.var)}}} }
/* system_status settings items will be saved to resume.cfg
   Use for int which use the set_sound() function to set them
   These items WILL be included in the users exported settings files
 */
#define SYSTEM_STATUS_SOUND(flags,var,lang_id,name,setting)                  {flags|F_T_INT|F_T_SOUND|F_SOUNDSETTING|F_ALLOW_ARBITRARY_VALS|  F_RESUMESETTING, &global_status.var, lang_id, NODEFAULT,name,   {.sound_setting=(struct sound_setting[]){{setting}}} }

/* setting which stores as a filename (or another string) in the .cfgvals
    The string must be a char array (which all of our string settings are),
    not just a char pointer.
    prefix: The absolute path to not save in the variable, ex /.rockbox/wps_file
    suffix: The file extention (usually...) e.g .wps_file
    If the prefix is set (not NULL), then the suffix must be set as well.
 */
#define TEXT_SETTING(flags,var,name,default,prefix,suffix)       {flags|F_T_UCHARPTR, &global_settings.var,-1,            CHARPTR(default),name,                               {.filename_setting=                                  (struct filename_setting[]){                     {prefix,suffix,sizeof(global_settings.var)}}} }

#define DIRECTORY_SETTING(flags,var,lang_id,name,default)  {flags|F_DIRNAME|F_T_UCHARPTR, &global_settings.var, lang_id,  CHARPTR(default), name,  {.filename_setting=(struct filename_setting[]){  {NULL, NULL, sizeof(global_settings.var)}}}}

/*  Used for settings which use the set_option() setting screen.
    The ... arg is a list of pointers to strings to display in the setting
    screen. These can either be literal strings, or ID2P(LANG_*) */
#define CHOICE_SETTING(flags,var,lang_id,default,name,cfg_vals,cb,count,...)    {flags|F_CHOICE_SETTING|F_T_INT,  &global_settings.var, lang_id,    INT(default), name,                                   {.choice_setting = (struct choice_setting[]){                   {cb, count, cfg_vals, (const unsigned char*[])     {__VA_ARGS__}}}}}

/* Similar to above, except the strings to display are taken from cfg_vals. */
#define STRINGCHOICE_SETTING(flags,var,lang_id,default,name,cfg_vals,           cb,count)       {flags|F_CHOICE_SETTING|F_T_INT|F_CHOICE_CFGVALS,                   &global_settings.var, lang_id,                                  INT(default), name,                                             {.choice_setting = (struct choice_setting[]){                   {cb, count, cfg_vals, NULL}}}}

/*  for settings which use the set_int() setting screen.
    unit is the UNIT_ define to display/talk.
    the first one saves a string to the config file,
    the second one saves the variable value to the config file */
#define INT_SETTING(flags, var, lang_id, default, name,                  unit, min, max, step, formatter, cb)                 {flags|F_INT_SETTING|F_T_INT, &global_settings.var,          lang_id, INT(default), name,                             {.int_setting = (struct int_setting[]){                 {cb, unit, step, min, max, formatter}}}}
#define INT_SETTING_NOWRAP(flags, var, lang_id, default, name,              unit, min, max, step, formatter, cb)                    {flags|F_INT_SETTING|F_T_INT|F_NO_WRAP, &global_settings.var,   lang_id, INT(default), name,                                {.int_setting = (struct int_setting[]){                    {cb, unit, step, min, max, formatter}}}}
#define TABLE_SETTING(flags, var, lang_id, default, name, cfg_vals,  unit, formatter, cb, count, ...)              {flags|F_TABLE_SETTING|F_T_INT, &global_settings.var,    lang_id, INT(default), name,                         {.table_setting = (struct table_setting[]) {         {cb, formatter, unit, count,        cfg_vals, (const int[]){__VA_ARGS__}}}}}
#define TABLE_SETTING_LIST(flags, var, lang_id, default, name, cfg_vals,  unit, formatter, cb, count, list)             {flags|F_TABLE_SETTING|F_T_INT, &global_settings.var,    lang_id, INT(default), name,                         {.table_setting = (struct table_setting[]) {         {cb, formatter, unit, count, cfg_vals, list}}}}
#define CUSTOM_SETTING(flags, var, lang_id, default, name,               load_from_cfg, write_to_cfg,                      is_change, set_default)                           {flags|F_CUSTOM_SETTING|F_T_CUSTOM|F_BANFROMQS,              &global_settings.var, lang_id,                           {.custom = (void*)default}, name,                        {.custom_setting = (struct custom_setting[]){                {load_from_cfg, write_to_cfg, is_change, set_default}}}}

#define VIEWPORT_SETTING(var,name)       TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY,var,name,"-", NULL, NULL)

/* some sets of values which are used more than once, to save memory */
static const char off[] = "off";
static const char off_on[] = "off,on";
static const int timeout_sec_common[] = {-1,0,1,2,3,4,5,6,7,8,9,10,15,20,25,30,
                                        45,60,90,120,180,240,300,600,900,1200,
                                        1500,1800,2700,3600,4500,5400,6300,7200};
/* Trimpod "Auto Screen Off" timeouts: Never (0), 5s steps up to 1 min, then 2/5/10 min. */
static const int auto_screen_off_values[] =
    { 0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 120, 300, 600 };
/* Trimpod "Visualization Transition": seconds each preset shows before switching;
 * 0 = "Never" (hold the preset until the user manually changes it). */
static const int viz_transition_values[] = { 0, 5, 10, 15, 20, 30, 45, 60, 75, 90 };

static const char graphic_numeric[] = "graphic,numeric";

/* Default theme settings */
#define DEFAULT_WPSNAME  "cabbiev2"
#define DEFAULT_SBSNAME  "-"
#define DEFAULT_FMS_NAME "cabbiev2"

#if LCD_HEIGHT <= 64
  #define DEFAULT_FONT_HEIGHT 8
  #define DEFAULT_FONTNAME "08-Rockfont"
#elif LCD_HEIGHT <= 80
  #define DEFAULT_FONT_HEIGHT 11
  #define DEFAULT_FONTNAME "11-Sazanami-Mincho"
/* sandisk sansa clip zip and samsung yh-820 */
#elif (LCD_HEIGHT == 96) && ((LCD_WIDTH == 96) || (LCD_WIDTH == 128))
  #define DEFAULT_FONT_HEIGHT 8
  #define DEFAULT_FONTNAME "08-Rockfont"
#elif LCD_HEIGHT <= 220
  #define DEFAULT_FONT_HEIGHT 12
#elif LCD_HEIGHT <= 240
  #define DEFAULT_FONT_HEIGHT 15
#elif LCD_HEIGHT <= 320
  #define DEFAULT_FONT_HEIGHT 18
#elif LCD_HEIGHT <= 400
  #define DEFAULT_FONT_HEIGHT 16
#elif LCD_HEIGHT <= 480 && LCD_WIDTH < 800
  #define DEFAULT_FONT_HEIGHT 27
#else
  #define DEFAULT_FONT_HEIGHT 35
#endif
#ifndef DEFAULT_FONTNAME
/* ugly expansion needed */
#define _EXPAND2(x) #x
#define _EXPAND(x) _EXPAND2(x)
#define DEFAULT_FONTNAME _EXPAND(DEFAULT_FONT_HEIGHT) "-Adobe-Helvetica"
#endif

#ifdef HAVE_LCD_COLOR
  #if DEFAULT_FONT_HEIGHT >= 31
    #define DEFAULT_ICONSET "tango_icons.32x32"
    #define DEFAULT_VIEWERS_ICONSET "tango_icons_viewers.32x32"
  #elif DEFAULT_FONT_HEIGHT >= 23
    #define DEFAULT_ICONSET "tango_icons.24x24"
    #define DEFAULT_VIEWERS_ICONSET "tango_icons_viewers.24x24"
  #elif DEFAULT_FONT_HEIGHT >= 15
    #define DEFAULT_ICONSET "tango_icons.16x16"
    #define DEFAULT_VIEWERS_ICONSET "tango_icons_viewers.16x16"
  #elif DEFAULT_FONT_HEIGHT >= 11
    #define DEFAULT_ICONSET "tango_icons.12x12"
    #define DEFAULT_VIEWERS_ICONSET "tango_icons_viewers.12x12"
  #elif DEFAULT_FONT_HEIGHT >= 7
    #define DEFAULT_ICONSET "tango_icons.8x8"
    #define DEFAULT_VIEWERS_ICONSET "tango_icons_viewers.8x8"
  #endif
#elif LCD_DEPTH > 1 /* greyscale */
  #define DEFAULT_ICONSET "tango_small_mono"
  #define DEFAULT_VIEWERS_ICONSET "tango_small_viewers_mono"
#else /* monochrome */
  #define DEFAULT_ICONSET ""
  #define DEFAULT_VIEWERS_ICONSET ""
#endif

/* Trimpod: the Classic Dark pairing (power_menu.c's theme table), so a config
 * with a missing colour line still lands on a named theme, not stock Rockbox's
 * grey-on-black (which the Theme knob would show as "Custom"). */
#define DEFAULT_THEME_FOREGROUND LCD_RGBPACK(0x18, 0x1C, 0x18)
#define DEFAULT_THEME_BACKGROUND LCD_RGBPACK(0xd8, 0xdc, 0xd0)
#define DEFAULT_THEME_SELECTOR_START LCD_RGBPACK(0xff, 0xeb, 0x9c)
#define DEFAULT_THEME_SELECTOR_END LCD_RGBPACK(0xb5, 0x8e, 0x00)
#define DEFAULT_THEME_SELECTOR_TEXT LCD_RGBPACK(0x00, 0x00, 0x00)
#define DEFAULT_THEME_SEPARATOR  LCD_RGBPACK(0x80, 0x80, 0x80)

#define DEFAULT_BACKDROP    BACKDROP_DIR "/cabbiev2.bmp"

#define DEFAULT_BACKLIGHT_TIMEOUT 15


#ifdef AUDIOHW_HAVE_POWER_MODE
# ifndef TARGET_DEFAULT_DAC_POWER_MODE
#   define TARGET_DEFAULT_DAC_POWER_MODE SOUND_HIGH_POWER
# endif
#endif

# define SCROLLBAR_DEFAULT SCROLLBAR_LEFT


#if LCD_DEPTH > 1
static const char* list_pad_formatter(char *buffer, size_t buffer_size,
                                    int val, const char *unit)
{
    switch (val)
    {
        case -1: return str(LANG_AUTO);
        case  0: return str(LANG_OFF);
        default: break;
    }
    snprintf(buffer, buffer_size, "%d %s", val, unit);
    return buffer;
}

#endif

static const char* formatter_time_unit_0_is_off(char *buffer, size_t buffer_size,
                                    int val, const char *unit)
{
    (void) buffer_size;
    (void) unit;
    if (val == 0)
        return str(LANG_OFF);
    return buffer;
}


/* Trimpod: "Auto Screen Off" reframing of the backlight timeout -- the always-on
 * value (0) reads "Never" (the screen never auto-turns-off).  The setting's value
 * list skips the -1 ("screen permanently off") entry, so only 0 and positive
 * timeouts reach this. */
static const char* formatter_time_unit_0_is_never(char *buffer, size_t buffer_size,
                                    int val, const char *unit)
{
    (void) unit;
    if (val <= 0)
        return str(LANG_NEVER);
    if (val >= 60 && (val % 60) == 0)
        snprintf(buffer, buffer_size, "%dm", val / 60);  /* 1m, 2m, 5m, 10m */
    else
        snprintf(buffer, buffer_size, "%ds", val);       /* 5s .. 55s */
    return buffer;
}

/* Like above but always whole seconds (60s/75s/90s, never collapsed to "1m"). */
static const char* formatter_seconds_0_is_never(char *buffer, size_t buffer_size,
                                    int val, const char *unit)
{
    (void) unit;
    if (val <= 0)
        return str(LANG_NEVER);
    snprintf(buffer, buffer_size, "%ds", val);
    return buffer;
}


static const char* formatter_time_unit_0_is_skip_track(char *buffer,
                                size_t buffer_size, int val, const char *unit)
{
    (void)unit;
    (void)buffer_size;
    if (val == -1)
        return str(LANG_SKIP_OUTRO);
    else if (val == 0)
        return str(LANG_SKIP_TRACK);
    return buffer;
}

static const char* scanaccel_formatter(char *buffer, size_t buffer_size,
        int val, const char *unit)
{
    (void)unit;
    if (val == 0)
        return str(LANG_OFF);
    else
        snprintf(buffer, buffer_size, "Speed up every %d s", val);
    return buffer;
}

static const char* formatter_unit_0_is_off(char *buffer, size_t buffer_size,
                                    int val, const char *unit)
{
    if (val == 0)
        return str(LANG_OFF);
    else
        snprintf(buffer, buffer_size, "%d %s", val, unit);
    return buffer;
}

static void crossfeed_cross_set(int val)
{
   (void)val;
   dsp_set_crossfeed_cross_params(global_settings.crossfeed_cross_gain,
                                  global_settings.crossfeed_hf_attenuation,
                                  global_settings.crossfeed_hf_cutoff);
}

static void surround_set_factor(int val)
{
    (void)val;
    dsp_surround_set_cutoff(global_settings.surround_fx1, global_settings.surround_fx2);
}

static void compressor_set(int val)
{
    (void)val;
    dsp_set_compressor(&global_settings.compressor_settings);
}

static const char* db_format(char* buffer, size_t buffer_size, int value,
                      const char* unit)
{
    int v = abs(value);

    snprintf(buffer, buffer_size, "%s%d.%d %s", value < 0 ? "-" : "",
             v / 10, v % 10, unit);
    return buffer;
}


struct eq_band_setting eq_defaults[EQ_NUM_BANDS] = {
    { 32, 7, 0 },
    { 64, 10, 0 },
    { 125, 10, 0 },
    { 250, 10, 0 },
    { 500, 10, 0 },
    { 1000, 10, 0 },
    { 2000, 10, 0 },
    { 4000, 10, 0 },
    { 8000, 10, 0 },
    { 16000, 7, 0 },
};


static void eq_load_from_cfg(void *setting, char *value)
{
    struct eq_band_setting *eq = setting;
    char *val_end, *end;

    val_end = value + strlen(value);

    /* cutoff/center */
    end = strchr(value, ',');
    if (!end) return;
    *end = '\0';
    eq->cutoff = atoi(value);

    /* q */
    value = end + 1;
    if (value > val_end) return;
    end = strchr(value, ',');
    if (!end) return;
    *end = '\0';
    eq->q = atoi(value);

    /* gain */
    value = end + 1;
    if (value > val_end) return;
    eq->gain = atoi(value);
}

static char* eq_write_to_cfg(void *setting, char *buf, int buf_len)
{
    struct eq_band_setting *eq = setting;

    snprintf(buf, buf_len, "%d, %d, %d", eq->cutoff, eq->q, eq->gain);
    return buf;
}

static bool eq_is_changed(void *setting, void *defaultval)
{
    struct eq_band_setting *eq = setting;

    return memcmp(eq, defaultval, sizeof(struct eq_band_setting));
}

static void eq_set_default(void* setting, void* defaultval)
{
    memcpy(setting, defaultval, sizeof(struct eq_band_setting));
}



/* perform shuffle/unshuffle of the current playlist based on the boolean provided */
static void shuffle_playlist_callback(bool shuffle)
{
    struct playlist_info *playlist = playlist_get_current();
    if (playlist->started)
    {
        if ((audio_status() & AUDIO_STATUS_PLAY) == AUDIO_STATUS_PLAY)
        {
            replaygain_update();
            if (shuffle)
            {
                playlist_randomise(playlist, current_tick, true);
            }
            else
            {
                playlist_sort(playlist, true);
            }
        }
    }
    iap_on_shuffle_state(shuffle);
}

static void repeat_mode_callback(int repeat)
{
    (void)repeat;
    if ((audio_status() & AUDIO_STATUS_PLAY) == AUDIO_STATUS_PLAY)
    {
        audio_flush_and_reload_tracks();
    }
    iap_on_repeat_state(repeat);
}



/* volume limiter */
static void volume_limit_load_from_cfg(void* var, char*value)
{
    *(int*)var = atoi(value);
}
static char* volume_limit_write_to_cfg(void* setting, char*buf, int buf_len)
{
    int current = *(int*)setting;
    itoa_buf(buf, buf_len, current);
    return buf;
}
static bool volume_limit_is_changed(void* setting, void* defaultval)
{
    (void)defaultval;
    int current = *(int*)setting;
    return (current != sound_max(SOUND_VOLUME));
}
static void volume_limit_set_default(void* setting, void* defaultval)
{
    (void)defaultval;
    *(int*)setting = sound_max(SOUND_VOLUME);
}



const struct settings_list settings[] = {
/* system_status settings .resume.cfg */
    /* Trimpod: key is versioned "volume_ddb" -- the unit is deci-dB, so a stale
     * whole-dB "volume: -30" must not be readable (it would load as -3.0 dB,
     * near max).  Unknown key falls back to sound_default(). */
    SYSTEM_STATUS_SOUND(F_NO_WRAP, volume, LANG_VOLUME, "volume_ddb", SOUND_VOLUME),
    SYSTEM_STATUS(0, resume_index,   -1,     "IDX"),
    SYSTEM_STATUS(0, resume_crc32,   -1,     "CRC"),
    SYSTEM_STATUS(0, resume_elapsed, -1,     "ELA"),
    SYSTEM_STATUS(0, resume_offset,  -1,     "OFF"),
    SYSTEM_STATUS(0, resume_modified, 0,     "PLM"),
    SYSTEM_STATUS(0, runtime,         0,     "CRT"),
    SYSTEM_STATUS(0, topruntime,      0,     "TRT"),
    SYSTEM_STATUS(0, last_screen,    -1,     "PVS"),
    SYSTEM_STATUS(0, last_browser,    0,     "BRS"),
/* sound settings */
    /* Trimpod: same unit versioning as volume_ddb -- a stale "volume limit" is
     * ignored (no cap) rather than misread. */
    CUSTOM_SETTING(F_NO_WRAP, volume_limit, LANG_VOLUME_LIMIT,
                  NULL, "volume_limit_ddb",
                  volume_limit_load_from_cfg, volume_limit_write_to_cfg,
                  volume_limit_is_changed, volume_limit_set_default),
    SOUND_SETTING(0, balance, LANG_BALANCE, "balance", SOUND_BALANCE),
/* Tone controls */
#ifdef AUDIOHW_HAVE_BASS
    SOUND_SETTING(F_NO_WRAP,bass, LANG_BASS, "bass", SOUND_BASS),
#endif
#ifdef AUDIOHW_HAVE_TREBLE
    SOUND_SETTING(F_NO_WRAP,treble, LANG_TREBLE, "treble", SOUND_TREBLE),
#endif
/* Hardware EQ tone controls */
#ifdef AUDIOHW_HAVE_EQ
/* Band gain is generic */
    SOUND_SETTING(F_NO_WRAP, hw_eq_bands[AUDIOHW_EQ_BAND1].gain,
                  LANG_HW_EQ_GAIN, "tone band1 gain", SOUND_EQ_BAND1_GAIN),
#ifdef AUDIOHW_HAVE_EQ_BAND2
    SOUND_SETTING(F_NO_WRAP, hw_eq_bands[AUDIOHW_EQ_BAND2].gain,
                  LANG_HW_EQ_GAIN, "tone band2 gain", SOUND_EQ_BAND2_GAIN),
#endif /* AUDIOHW_HAVE_EQ_BAND2 */
#ifdef AUDIOHW_HAVE_EQ_BAND3
    SOUND_SETTING(F_NO_WRAP, hw_eq_bands[AUDIOHW_EQ_BAND3].gain,
                  LANG_HW_EQ_GAIN, "tone band3 gain", SOUND_EQ_BAND3_GAIN),
#endif /* AUDIOHW_HAVE_EQ_BAND3 */
#ifdef AUDIOHW_HAVE_EQ_BAND4
    SOUND_SETTING(F_NO_WRAP, hw_eq_bands[AUDIOHW_EQ_BAND4].gain,
                  LANG_HW_EQ_GAIN, "tone band4 gain", SOUND_EQ_BAND4_GAIN),
#endif /* AUDIOHW_HAVE_EQ_BAND4 */
#ifdef AUDIOHW_HAVE_EQ_BAND5
    SOUND_SETTING(F_NO_WRAP, hw_eq_bands[AUDIOHW_EQ_BAND5].gain,
                  LANG_HW_EQ_GAIN, "tone band5 gain", SOUND_EQ_BAND5_GAIN),
#endif /* AUDIOHW_HAVE_EQ_BAND5 */
#endif /* AUDIOHW_HAVE_EQ */
/* 3-d enhancement effect */
    CHOICE_SETTING(0, channel_config, LANG_CHANNEL_CONFIGURATION,
                   0,"channels",
                   "stereo,mono,custom,mono left,mono right,karaoke,swap",
                   sound_set_channels, 7,
                   ID2P(LANG_CHANNEL_STEREO), ID2P(LANG_CHANNEL_MONO),
                   ID2P(LANG_CHANNEL_CUSTOM), ID2P(LANG_CHANNEL_LEFT),
                   ID2P(LANG_CHANNEL_RIGHT), ID2P(LANG_CHANNEL_KARAOKE),
                   ID2P(LANG_CHANNEL_SWAP)),
    SOUND_SETTING(0, stereo_width, LANG_STEREO_WIDTH,
                  "stereo_width", SOUND_STEREO_WIDTH),
#ifdef AUDIOHW_HAVE_DEPTH_3D
    SOUND_SETTING(0,depth_3d, LANG_DEPTH_3D, "3-d enhancement",
                  SOUND_DEPTH_3D),
#endif

#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    CHOICE_SETTING(F_SOUNDSETTING, roll_off, LANG_FILTER_ROLL_OFF, 0,
#if defined(AUDIOHW_HAVE_ES9218_ROLL_OFF)
                   "roll_off", "linear fast,linear slow,minimum fast,minimum slow,apodizing 1,apodizing 2,hybrid fast,brick wall", sound_set_filter_roll_off,
                   8, ID2P(LANG_FILTER_LINEAR_FAST), ID2P(LANG_FILTER_LINEAR_SLOW), ID2P(LANG_FILTER_MINIMUM_FAST), ID2P(LANG_FILTER_MINIMUM_SLOW),
                   ID2P(LANG_FILTER_APODIZING_1), ID2P(LANG_FILTER_APODIZING_2), ID2P(LANG_FILTER_HYBRID_FAST), ID2P(LANG_FILTER_BRICK_WALL)),
#elif defined(AUDIOHW_HAVE_SHORT2_ROLL_OFF)
                   "roll_off", "sharp,slow,short sharp,short slow", sound_set_filter_roll_off,
                   4, ID2P(LANG_FILTER_SHARP), ID2P(LANG_FILTER_SLOW), ID2P(LANG_FILTER_SHORT_SHARP), ID2P(LANG_FILTER_SHORT_SLOW)),
#elif defined(AUDIOHW_HAVE_SS_ROLL_OFF)
                   "roll_off", "sharp,slow,short sharp,short slow,super slow", sound_set_filter_roll_off,
                   5, ID2P(LANG_FILTER_SHARP), ID2P(LANG_FILTER_SLOW), ID2P(LANG_FILTER_SHORT_SHARP), ID2P(LANG_FILTER_SHORT_SLOW), ID2P(LANG_FILTER_SUPER_SLOW)),
#elif defined(AUDIOHW_HAVE_SHORT_ROLL_OFF)
                   "roll_off", "sharp,slow,short,bypass", sound_set_filter_roll_off,
                   4, ID2P(LANG_FILTER_SHARP), ID2P(LANG_FILTER_SLOW), ID2P(LANG_FILTER_SHORT), ID2P(LANG_FILTER_BYPASS)),
#else
                   "roll_off", "sharp,slow", sound_set_filter_roll_off,
                   2, ID2P(LANG_FILTER_SHARP), ID2P(LANG_FILTER_SLOW)),
#endif
#endif

#ifdef AUDIOHW_HAVE_POWER_MODE
    CHOICE_SETTING(F_SOUNDSETTING, power_mode, LANG_DAC_POWER_MODE,
                   TARGET_DEFAULT_DAC_POWER_MODE,
                   "dac_power_mode", "high,low", sound_set_power_mode,
                   2, ID2P(LANG_DAC_POWER_HIGH), ID2P(LANG_DAC_POWER_LOW)),
#endif

    /* playback */
    /* Shuffle shows On/Off (not the OFFON_SETTING default "Yes"/"No"); config
     * still stores off_on for compatibility. */
    BOOL_SETTING(F_CB_ON_SELECT_ONLY|F_CB_ONLY_IF_CHANGED, playlist_shuffle,
                 LANG_SHUFFLE, false, "shuffle", off_on,
                 LANG_ON, LANG_OFF, shuffle_playlist_callback),

    CHOICE_SETTING(F_CB_ON_SELECT_ONLY|F_CB_ONLY_IF_CHANGED, repeat_mode,
                   LANG_REPEAT, REPEAT_OFF, "repeat", "off,all,one"
#ifdef AB_REPEAT_ENABLE
                   ",ab"
#endif
                   , repeat_mode_callback,
#ifdef AB_REPEAT_ENABLE
                   4,
#else
                   3,
#endif
                   ID2P(LANG_OFF), ID2P(LANG_ALL), ID2P(LANG_REPEAT_ONE)
#ifdef AB_REPEAT_ENABLE
                   ,ID2P(LANG_REPEAT_AB)
#endif
                  ), /* CHOICE_SETTING( repeat_mode ) */


    /* LCD */
    /* Trimpod: presented as "Auto Screen Off" (Device Settings).  0 = "Never"
     * (screen never auto-turns-off); the -1 ("screen permanently off") entry is
     * skipped via &timeout_sec_common[1] / count 22. */
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS,
                    backlight_timeout, LANG_TRIMPOD_AUTO_SCREEN_OFF,
                    DEFAULT_BACKLIGHT_TIMEOUT, "backlight timeout",
                    off_on, UNIT_SEC, formatter_time_unit_0_is_never,
                    backlight_set_timeout,
                    16, auto_screen_off_values),
    /* display */
     CHOICE_SETTING(F_TEMPVAR|F_THEMESETTING, cursor_style, LANG_INVERT_CURSOR,
 #ifdef HAVE_LCD_COLOR
                    3, "selector type",
                    "pointer,bar (inverse),bar (color),bar (gradient)", NULL, 4,
                    ID2P(LANG_INVERT_CURSOR_POINTER),
                    ID2P(LANG_INVERT_CURSOR_BAR),
                    ID2P(LANG_INVERT_CURSOR_COLOR),
                    ID2P(LANG_INVERT_CURSOR_GRADIENT)),
 #else
                    1, "selector type", "pointer,bar (inverse)", NULL, 2,
                    ID2P(LANG_INVERT_CURSOR_POINTER),
                    ID2P(LANG_INVERT_CURSOR_BAR)),
 #endif
    CHOICE_SETTING(F_THEMESETTING|F_TEMPVAR|F_NEEDAPPLY, statusbar,
                  LANG_STATUS_BAR, STATUSBAR_TOP, "statusbar","off,top,bottom",
                  NULL, 3, ID2P(LANG_OFF), ID2P(LANG_STATUSBAR_TOP),
                  ID2P(LANG_STATUSBAR_BOTTOM)),
    CHOICE_SETTING(F_THEMESETTING|F_TEMPVAR, scrollbar,
                  LANG_SCROLL_BAR, SCROLLBAR_DEFAULT, "scrollbar","off,left,right",
                  NULL, 3, ID2P(LANG_OFF), ID2P(LANG_LEFT), ID2P(LANG_RIGHT)),
    INT_SETTING(F_THEMESETTING, scrollbar_width, LANG_SCROLLBAR_WIDTH, 6,
                "scrollbar width",UNIT_INT, 3, MAX(LCD_WIDTH/10,25), 1,
                NULL, NULL),
#if LCD_DEPTH > 1
    TABLE_SETTING(F_ALLOW_ARBITRARY_VALS, list_separator_height, LANG_LIST_SEPARATOR,
                  0, "list separator height", "auto,off", UNIT_PIXEL,
                  list_pad_formatter, NULL, 15,
                  -1,0,1,2,3,4,5,7,9,11,13,16,20,25,30),
#ifdef HAVE_LCD_COLOR
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.list_separator_color,-1,
        INT(DEFAULT_THEME_SEPARATOR),"list separator color",UNUSED},
#endif
#endif
    CHOICE_SETTING(F_THEMESETTING, volume_type, LANG_VOLUME_DISPLAY, 0,
                   "volume display", graphic_numeric, NULL, 2,
                   ID2P(LANG_DISPLAY_GRAPHIC),
                   ID2P(LANG_DISPLAY_NUMERIC)),
    CHOICE_SETTING(F_THEMESETTING, battery_display, LANG_BATTERY_DISPLAY, 0,
                   "battery display", graphic_numeric, NULL, 2,
                   ID2P(LANG_DISPLAY_GRAPHIC), ID2P(LANG_DISPLAY_NUMERIC)),
    CHOICE_SETTING(0, timeformat, LANG_TIMEFORMAT, 0,
        "time format", "24hour,12hour", NULL, 2,
        ID2P(LANG_24_HOUR_CLOCK), ID2P(LANG_12_HOUR_CLOCK)),
    /* system */
    /* Idle Power Off: same value list + label format as Display Poweroff
     * (backlight_timeout); value is seconds, 0 = "Never". */
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS,
                poweroff, LANG_POWEROFF_IDLE, 0, "idle poweroff",
                off_on, UNIT_SEC, formatter_time_unit_0_is_never,
                set_poweroff_timeout, 16, auto_screen_off_values),
    /* Trimpod visualizer: idle-on-WPS seconds before auto-starting (0 = Never),
     * and how long each preset shows before switching.  Same value list/format
     * as Display Poweroff; read directly (no apply callback needed). */
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS,
                viz_start_delay, LANG_TRIMPOD_VIZ_ACTIVATE_AFTER, 0, "viz start delay",
                off_on, UNIT_SEC, formatter_time_unit_0_is_never,
                NULL, 16, auto_screen_off_values),
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS,
                viz_transition, LANG_TRIMPOD_VIZ_TRANSITION, 20, "viz transition",
                off_on, UNIT_SEC, formatter_seconds_0_is_never,
                NULL, 10, viz_transition_values),
#ifdef HAVE_PERCEPTUAL_VOLUME
    /* Trimpod: perceptual by default with 40 even loudness increments (a 0..10
     * dial); not exposed in the menu. */
    CHOICE_SETTING(0, volume_adjust_mode, LANG_VOLUME_ADJUST_MODE,
                   VOLUME_ADJUST_PERCEPTUAL, "volume adjustment mode",
                   "direct,perceptual", NULL, 2,
                   ID2P(LANG_DIRECT), ID2P(LANG_PERCEPTUAL)),
    INT_SETTING_NOWRAP(0, volume_adjust_norm_steps, LANG_VOLUME_ADJUST_NORM_STEPS,
                       40, "perceptual volume step count", UNIT_INT,
                       MIN_NORM_VOLUME_STEPS, MAX_NORM_VOLUME_STEPS, 5,
                       NULL, NULL),
#endif
/* use this setting for user code even if there's no exchangable battery
 * support enabled */
#if BATTERY_CAPACITY_INC > 0
    INT_SETTING(0, battery_capacity, LANG_BATTERY_CAPACITY,
                BATTERY_CAPACITY_DEFAULT, "battery capacity", UNIT_MAH,
                BATTERY_CAPACITY_MIN, BATTERY_CAPACITY_MAX,
                BATTERY_CAPACITY_INC, NULL, set_battery_capacity),
#endif

    OFFON_SETTING(0, bl_filter_first_keypress,
                  LANG_BACKLIGHT_FILTER_FIRST_KEYPRESS, false,
                  "backlight filters first keypress", NULL),

/** End of old RTC config block **/

#ifndef HAS_BUTTON_HOLD
    OFFON_SETTING(F_BANFROMQS, bt_selective_softlock_actions,
                  LANG_ACTION_ENABLED, false,
                  "No Screen Lock For Selected Actions", NULL),
    INT_SETTING(F_BANFROMQS, bt_selective_softlock_actions_mask,
                LANG_SOFTLOCK_SELECTIVE,
                0, "Selective Screen Lock Actions", UNIT_INT,
                0, 2048,2, NULL, NULL),
#endif /* !HAS_BUTTON_HOLD */

    OFFON_SETTING(0, caption_backlight, LANG_CAPTION_BACKLIGHT,
                  false, "caption backlight", NULL),

    OFFON_SETTING(F_BANFROMQS, bl_selective_actions,
                  LANG_ACTION_ENABLED, false,
                  "No Backlight On Selected Actions", NULL),

    INT_SETTING(F_BANFROMQS, bl_selective_actions_mask,
                LANG_BACKLIGHT_SELECTIVE,
                0, "Selective Backlight Actions", UNIT_INT,
                0, 2048,2, NULL, NULL),
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    INT_SETTING(F_NO_WRAP, brightness, LANG_BRIGHTNESS,
                2, "brightness",UNIT_INT,
                MIN_BRIGHTNESS_SETTING, MAX_BRIGHTNESS_SETTING, 1,
                NULL, backlight_set_brightness),
#endif
    /* backlight fading */
#if   defined(HAVE_BACKLIGHT_FADING_BOOL_SETTING)
    OFFON_SETTING(0, backlight_fade_in, LANG_BACKLIGHT_FADE_IN,
                    true, "backlight fade in", backlight_set_fade_in),
    OFFON_SETTING(0, backlight_fade_out, LANG_BACKLIGHT_FADE_OUT,
                    true, "backlight fade out", backlight_set_fade_out),
#endif
    INT_SETTING(F_PADTITLE, scroll_speed, LANG_SCROLL_SPEED, 9,"scroll speed",
                UNIT_INT, 0, 17, 1, NULL, lcd_scroll_speed),
    INT_SETTING(F_TIME_SETTING | F_PADTITLE, scroll_delay, LANG_SCROLL_DELAY,
                1000, "scroll delay", UNIT_MS, 0, 3000, 100,
                formatter_time_unit_0_is_off,
                lcd_scroll_delay),
    INT_SETTING(0, bidir_limit, LANG_BIDIR_SCROLL, 50, "bidir limit",
                UNIT_PERCENT, 0, 200, 25, NULL, lcd_bidir_scroll),
    OFFON_SETTING(0, offset_out_of_view, LANG_SCREEN_SCROLL_VIEW,
                  false, "Screen Scrolls Out Of View", NULL),
    OFFON_SETTING(0, disable_mainmenu_scrolling, LANG_DISABLE_MAINMENU_SCROLLING,
                  false, "Disable main menu scrolling", NULL),
    INT_SETTING(F_PADTITLE, scroll_step, LANG_SCROLL_STEP, 6, "scroll step",
                UNIT_PIXEL, 1, LCD_WIDTH, 1, NULL, lcd_scroll_step),
    INT_SETTING(F_PADTITLE, screen_scroll_step, LANG_SCREEN_SCROLL_STEP, 16,
                "screen scroll step", UNIT_PIXEL, 1, LCD_WIDTH, 1, NULL, NULL),
    OFFON_SETTING(0,scroll_paginated,LANG_SCROLL_PAGINATED,
                  false,"scroll paginated",NULL),
    OFFON_SETTING(0,list_wraparound,LANG_LIST_WRAPAROUND,
                  true,"list wraparound",NULL),
    CHOICE_SETTING(0, list_order, LANG_LIST_ORDER,
                   0,
                   /* values are defined by the enum in option_select.h */
                   "list order", "descending,ascending",
                   NULL, 2, ID2P(LANG_DESCENDING), ID2P(LANG_ASCENDING)),
#ifdef HAVE_LCD_COLOR

    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.fg_color,-1,
        INT(DEFAULT_THEME_FOREGROUND),"foreground color",UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.bg_color,-1,
        INT(DEFAULT_THEME_BACKGROUND),"background color",UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.lss_color,-1,
        INT(DEFAULT_THEME_SELECTOR_START),"line selector start color",UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.lse_color,-1,
        INT(DEFAULT_THEME_SELECTOR_END),"line selector end color",UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.lst_color,-1,
        INT(DEFAULT_THEME_SELECTOR_TEXT),"line selector text color",UNUSED},

#endif
    /* more playback */
    OFFON_SETTING(0,play_selected,LANG_PLAY_SELECTED,true,"play selected",NULL),
    CHOICE_SETTING(0, single_mode, LANG_SINGLE_MODE, 0,
                  "single mode",
                  "off,track,album,album artist,artist,composer,work,genre",
                  NULL, 8,
                  ID2P(LANG_OFF),
                  ID2P(LANG_TRACK),
                  ID2P(LANG_ID3_ALBUM),
                  ID2P(LANG_ID3_ALBUMARTIST),
                  ID2P(LANG_ID3_ARTIST),
                  ID2P(LANG_ID3_COMPOSER),
                  ID2P(LANG_ID3_GROUPING),
                  ID2P(LANG_ID3_GENRE)),
    OFFON_SETTING(0,fade_on_stop,LANG_FADE_ON_STOP,true,"volume fade",NULL),
    INT_SETTING(F_TIME_SETTING, ff_rewind_min_step, LANG_FFRW_STEP, 1,
                "scan min step", UNIT_SEC, 1, 60, 1, NULL, NULL),
    CHOICE_SETTING(0, ff_rewind_accel, LANG_FFRW_ACCEL, 2,
                   "seek acceleration", "very fast,fast,normal,slow,very slow", NULL, 5,
                   ID2P(LANG_VERY_FAST), ID2P(LANG_FAST), ID2P(LANG_NORMAL),
                   ID2P(LANG_SLOW) , ID2P(LANG_VERY_SLOW)),
    /* disk */
    /* browser */
    TEXT_SETTING(0, start_directory, "start directory", "/", NULL, NULL),
    SYSTEM_STATUS_TEXT_SETTING(0, browse_last_folder, "last folder", "/", NULL, NULL),
    CHOICE_SETTING(0, next_folder, LANG_NEXT_FOLDER, FOLDER_ADVANCE_OFF,
                   "folder navigation", "off,on,random",NULL ,3,
                   ID2P(LANG_SET_BOOL_NO), ID2P(LANG_SET_BOOL_YES),
                   ID2P(LANG_RANDOM)),
    BOOL_SETTING(0, constrain_next_folder, LANG_CONSTRAIN_NEXT_FOLDER, false,
                 "constrain next folder", off_on,
                 LANG_SET_BOOL_YES, LANG_SET_BOOL_NO, NULL),

    /* replay gain */
    CHOICE_SETTING(F_SOUNDSETTING, replaygain_settings.type,
                   LANG_REPLAYGAIN_MODE, REPLAYGAIN_SHUFFLE, "replaygain type",
                   "track,album,track shuffle,off", NULL, 4, ID2P(LANG_TRACK_GAIN),
                   ID2P(LANG_ALBUM_GAIN), ID2P(LANG_SHUFFLE_GAIN), ID2P(LANG_OFF)),
    OFFON_SETTING(F_SOUNDSETTING, replaygain_settings.noclip,
                  LANG_REPLAYGAIN_NOCLIP, false, "replaygain noclip", NULL),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, replaygain_settings.preamp,
                       LANG_REPLAYGAIN_PREAMP, 0, "replaygain preamp",
                       UNIT_DB, -120, 120, 5, db_format, NULL),

    CHOICE_SETTING(0, beep, LANG_BEEP, 0, "beep", "off,weak,moderate,strong",
                   NULL, 4, ID2P(LANG_OFF), ID2P(LANG_WEAK),
                   ID2P(LANG_MODERATE), ID2P(LANG_STRONG)),

#ifdef HAVE_CROSSFADE
    /* crossfade */
    CHOICE_SETTING(F_SOUNDSETTING, crossfade, LANG_CROSSFADE_ENABLE, 0,
                   "crossfade",
                   "off,auto track change,man track skip,shuffle,shuffle or man track skip,always",
                   NULL, 6, ID2P(LANG_OFF), ID2P(LANG_AUTOTRACKSKIP),
                   ID2P(LANG_MANTRACKSKIP), ID2P(LANG_SHUFFLE),
                   ID2P(LANG_SHUFFLE_TRACKSKIP), ID2P(LANG_ALWAYS)),
    INT_SETTING(F_TIME_SETTING | F_SOUNDSETTING, crossfade_fade_in_delay,
                LANG_CROSSFADE_FADE_IN_DELAY, 0,
                "crossfade fade in delay", UNIT_SEC, 0, 7, 1, NULL, NULL),
    INT_SETTING(F_TIME_SETTING | F_SOUNDSETTING, crossfade_fade_out_delay,
                LANG_CROSSFADE_FADE_OUT_DELAY, 0,
                "crossfade fade out delay", UNIT_SEC, 0, 7, 1, NULL,NULL),
    INT_SETTING(F_TIME_SETTING | F_SOUNDSETTING, crossfade_fade_in_duration,
                LANG_CROSSFADE_FADE_IN_DURATION, 2,
                "crossfade fade in duration", UNIT_SEC, 0, 15, 1, NULL, NULL),
    INT_SETTING(F_TIME_SETTING | F_SOUNDSETTING, crossfade_fade_out_duration,
                LANG_CROSSFADE_FADE_OUT_DURATION, 2,
                "crossfade fade out duration", UNIT_SEC, 0, 15, 1, NULL, NULL),
    CHOICE_SETTING(F_SOUNDSETTING, crossfade_fade_out_mixmode,
                   LANG_CROSSFADE_FADE_OUT_MODE, 0,
                   "crossfade fade out mode", "crossfade,mix", NULL, 2,
                   ID2P(LANG_CROSSFADE), ID2P(LANG_MIX)),
#endif

    /* crossfeed */
    CHOICE_SETTING(F_SOUNDSETTING, crossfeed, LANG_CROSSFEED, 0,"crossfeed",
                   "off,meier,custom", dsp_set_crossfeed_type, 3,
                   ID2P(LANG_OFF), ID2P(LANG_CROSSFEED_MEIER),
                   ID2P(LANG_CROSSFEED_CUSTOM)),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, crossfeed_direct_gain,
                       LANG_CROSSFEED_DIRECT_GAIN, -15,
                       "crossfeed direct gain", UNIT_DB, -60, 0, 5,
                       db_format, dsp_set_crossfeed_direct_gain),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, crossfeed_cross_gain,
                       LANG_CROSSFEED_CROSS_GAIN, -60,
                       "crossfeed cross gain", UNIT_DB, -120, -30, 5,
                       db_format, crossfeed_cross_set),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, crossfeed_hf_attenuation,
                       LANG_CROSSFEED_HF_ATTENUATION, -160,
                       "crossfeed hf attenuation", UNIT_DB, -240, -60, 5,
                       db_format, crossfeed_cross_set),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, crossfeed_hf_cutoff,
                       LANG_CROSSFEED_HF_CUTOFF, 700,
                       "crossfeed hf cutoff", UNIT_HERTZ, 500, 2000, 100,
                       NULL, crossfeed_cross_set),

    /* equalizer */
    OFFON_SETTING(F_EQSETTING, eq_enabled, LANG_EQUALIZER_ENABLED, false,
                  "eq enabled", eq_enabled_option_callback),

    INT_SETTING_NOWRAP(F_EQSETTING, eq_precut, LANG_EQUALIZER_PRECUT, 0,
                       "eq precut", UNIT_DB, 0, 240, 1, eq_precut_format,
                       dsp_set_eq_precut),

#define EQ_BAND(id, string)  CUSTOM_SETTING(F_EQSETTING, eq_band_settings[id], -1,    &eq_defaults[id], string,                      eq_load_from_cfg, eq_write_to_cfg,             eq_is_changed, eq_set_default)
    EQ_BAND(0, "eq low shelf filter"),
    EQ_BAND(1, "eq peak filter 1"),
    EQ_BAND(2, "eq peak filter 2"),
    EQ_BAND(3, "eq peak filter 3"),
    EQ_BAND(4, "eq peak filter 4"),
    EQ_BAND(5, "eq peak filter 5"),
    EQ_BAND(6, "eq peak filter 6"),
    EQ_BAND(7, "eq peak filter 7"),
    EQ_BAND(8, "eq peak filter 8"),
    EQ_BAND(9, "eq high shelf filter"),
#undef EQ_BAND

    /* dithering */
    OFFON_SETTING(F_SOUNDSETTING, dithering_enabled, LANG_DITHERING, false,
                  "dithering enabled", dsp_dither_enable),
    /* surround */
     TABLE_SETTING(F_TIME_SETTING | F_SOUNDSETTING, surround_enabled,
                  LANG_SURROUND, 0, "surround enabled", off,
                  UNIT_MS, formatter_time_unit_0_is_off,
                  dsp_surround_enable, 6,
                  0,5,8,10,15,30),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, surround_balance,
                       LANG_BALANCE, 35,
                       "surround balance", UNIT_PERCENT, 0, 99,
                       1, NULL, dsp_surround_set_balance),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, surround_fx1,
                       LANG_SURROUND_FX1, 3400,
                       "surround_fx1", UNIT_HERTZ, 600, 8000,
                       200, NULL, surround_set_factor),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, surround_fx2,
                       LANG_SURROUND_FX2, 320,
                       "surround_fx2", UNIT_HERTZ, 40, 400,
                       40, NULL, surround_set_factor),
    OFFON_SETTING(F_SOUNDSETTING, surround_method2, LANG_SURROUND_METHOD2, false,
                  "side only", dsp_surround_side_only),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, surround_mix,
                       LANG_SURROUND_MIX, 50,
                       "surround mix", UNIT_PERCENT, 0, 100,
                       5, NULL, dsp_surround_mix),
    /* auditory fatigue reduction */
    CHOICE_SETTING(F_SOUNDSETTING|F_NO_WRAP, afr_enabled,
                       LANG_AFR, 0,"afr enabled",
                       "off,weak,moderate,strong", dsp_afr_enable, 4,
                       ID2P(LANG_OFF), ID2P(LANG_WEAK),ID2P(LANG_MODERATE),ID2P(LANG_STRONG)),
    /* PBE */
    INT_SETTING_NOWRAP(F_SOUNDSETTING, pbe,
                       LANG_PBE_ENABLE, 0,
                       "pbe", UNIT_PERCENT, 0, 100,
                       25, NULL, dsp_pbe_enable),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, pbe_precut,
                       LANG_EQUALIZER_PRECUT, -25,
                       "pbe precut", UNIT_DB, -45, 0,
                       1, db_format, dsp_pbe_precut),

    /* compressor */
    INT_SETTING_NOWRAP(F_SOUNDSETTING, compressor_settings.threshold,
                       LANG_COMPRESSOR_THRESHOLD, 0,
                       "compressor threshold", UNIT_DB, 0, -24,
                       -3, formatter_unit_0_is_off, compressor_set),
    CHOICE_SETTING(F_SOUNDSETTING|F_NO_WRAP, compressor_settings.makeup_gain,
                   LANG_COMPRESSOR_GAIN, 1, "compressor makeup gain",
                   "off,auto", compressor_set, 2,
                   ID2P(LANG_OFF), ID2P(LANG_AUTO)),
    CHOICE_SETTING(F_SOUNDSETTING|F_NO_WRAP, compressor_settings.ratio,
                   LANG_COMPRESSOR_RATIO, 1, "compressor ratio",
                   "2:1,4:1,6:1,10:1,limit", compressor_set, 5,
                   ID2P(LANG_COMPRESSOR_RATIO_2), ID2P(LANG_COMPRESSOR_RATIO_4),
                   ID2P(LANG_COMPRESSOR_RATIO_6), ID2P(LANG_COMPRESSOR_RATIO_10),
                   ID2P(LANG_COMPRESSOR_RATIO_LIMIT)),
    CHOICE_SETTING(F_SOUNDSETTING|F_NO_WRAP, compressor_settings.knee,
                   LANG_COMPRESSOR_KNEE, 1, "compressor knee",
                   "hard knee,soft knee", compressor_set, 2,
                   ID2P(LANG_COMPRESSOR_HARD_KNEE), ID2P(LANG_COMPRESSOR_SOFT_KNEE)),
    INT_SETTING_NOWRAP(F_TIME_SETTING | F_SOUNDSETTING,
                       compressor_settings.attack_time,
                       LANG_COMPRESSOR_ATTACK, 5,
                       "compressor attack time", UNIT_MS, 0, 30,
                       5, NULL, compressor_set),
    INT_SETTING_NOWRAP(F_TIME_SETTING | F_SOUNDSETTING,
                       compressor_settings.release_time,
                       LANG_COMPRESSOR_RELEASE, 500,
                       "compressor release time", UNIT_MS, 100, 1000,
                       100, NULL, compressor_set),

#ifdef AUDIOHW_HAVE_BASS_CUTOFF
    SOUND_SETTING(F_NO_WRAP, bass_cutoff, LANG_BASS_CUTOFF,
                  "bass cutoff", SOUND_BASS_CUTOFF),
#endif
#ifdef AUDIOHW_HAVE_TREBLE_CUTOFF
    SOUND_SETTING(F_NO_WRAP, treble_cutoff, LANG_TREBLE_CUTOFF,
                  "treble cutoff", SOUND_TREBLE_CUTOFF),
#endif

    CHOICE_SETTING(F_TEMPVAR, default_codepage, LANG_DEFAULT_CODEPAGE, 14,
                   "default codepage",
                   /* The order must match with that in unicode.c */
                   "iso8859-1,iso8859-7,iso8859-8,cp1251,iso8859-11,cp1256,"
                   "iso8859-9,iso8859-2,cp1250,cp1252,sjis,gb2312,ksx1001,big5,utf-8",
                   NULL, 15,
                   ID2P(LANG_CODEPAGE_LATIN1),
                   ID2P(LANG_CODEPAGE_GREEK),
                   ID2P(LANG_CODEPAGE_HEBREW), ID2P(LANG_CODEPAGE_CYRILLIC),
                   ID2P(LANG_CODEPAGE_THAI), ID2P(LANG_CODEPAGE_ARABIC),
                   ID2P(LANG_CODEPAGE_TURKISH),
                   ID2P(LANG_CODEPAGE_LATIN_EXTENDED),
                   ID2P(LANG_CODEPAGE_CENTRAL_EUROPEAN),
                   ID2P(LANG_CODEPAGE_WESTERN_EUROPEAN),
                   ID2P(LANG_CODEPAGE_JAPANESE),
                   ID2P(LANG_CODEPAGE_SIMPLIFIED), ID2P(LANG_CODEPAGE_KOREAN),
                   ID2P(LANG_CODEPAGE_TRADITIONAL), ID2P(LANG_CODEPAGE_UTF8)),

    CHOICE_SETTING(0, backlight_on_button_hold,
                   LANG_BACKLIGHT_ON_BUTTON_HOLD,
                   /* Trimpod: the Brick's side switch is input-lock only and must
                    * NOT blank the screen, so default to 0 ("normal") rather than
                    * upstream's 1 ("off") for HAS_BUTTON_HOLD targets. */
                   0,
                   "backlight on button hold", "normal,off,on",
                   backlight_set_on_button_hold, 3,
                   ID2P(LANG_NORMAL), ID2P(LANG_OFF), ID2P(LANG_ON)),


    OFFON_SETTING(0, hold_lr_for_scroll_in_list, -1, true,
                  "hold_lr_for_scroll_in_list",NULL),


    INT_SETTING(F_TIME_SETTING, pause_rewind, LANG_PAUSE_REWIND, 0,
                "rewind duration on pause", UNIT_SEC, 0, 15, 1,
                formatter_time_unit_0_is_off, NULL),
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY, font_file, "font",
                     DEFAULT_FONTNAME, FONT_DIR "/", ".fnt"),
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY,wps_file, "wps",
                     DEFAULT_WPSNAME, WPS_DIR "/", ".wps"),
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY,sbs_file, "sbs",
                     DEFAULT_SBSNAME, SBS_DIR "/", ".sbs"),
    TEXT_SETTING(0,lang_file,"lang","",LANG_DIR "/",".lng"),
#if LCD_DEPTH > 1
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY,backdrop_file,"backdrop",
                     DEFAULT_BACKDROP, NULL, NULL),
#endif
    TEXT_SETTING(0,kbd_file,"kbd","-",ROCKBOX_DIR "/",".kbd"),
    OFFON_SETTING(F_BANFROMQS,cuesheet,LANG_CUESHEET_ENABLE,false,"cuesheet support",
                  NULL),
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS, skip_length,
                  LANG_SKIP_LENGTH, 0, "skip length",
                  "outro,track",
                  UNIT_SEC, formatter_time_unit_0_is_skip_track,
                  NULL,
                  25, timeout_sec_common),
    /* Customizable icons */
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY, viewers_icon_file, "viewers iconset",
                     DEFAULT_VIEWERS_ICONSET,
                     ICON_DIR "/", ".bmp"),
#ifdef HAVE_LCD_COLOR
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY, colors_file, "filetype colours", "-",
                     THEME_DIR "/", ".colours"),
#endif
    INT_SETTING(F_TIME_SETTING, list_accel_start_delay, LANG_LISTACCEL_START_DELAY,
                2, "list_accel_start_delay", UNIT_SEC, 0, 10, 1,
                formatter_time_unit_0_is_off, NULL),
    INT_SETTING(0, list_accel_wait, LANG_LISTACCEL_ACCEL_SPEED,
                3, "list_accel_wait", UNIT_SEC, 1, 10, 1,
                scanaccel_formatter, NULL),
    TEXT_SETTING(0, playlist_catalog_dir, "playlist catalog directory",
                     PLAYLIST_CATALOG_DEFAULT_DIR, NULL, NULL),
    TABLE_SETTING(0, sleeptimer_duration, LANG_SLEEP_TIMER, 30,
                "sleeptimer duration", NULL, UNIT_MIN, NULL,
                set_sleeptimer_duration, 8,
                5,10,15,30,45,60,75,90),



    OFFON_SETTING(0, prevent_skip, LANG_PREVENT_SKIPPING, false, "prevent track skip", NULL),
    OFFON_SETTING(0, rewind_across_tracks, LANG_REWIND_ACROSS_TRACKS, false, "rewind across tracks", NULL),




    /* Customizable list */
    VIEWPORT_SETTING(ui_vp_config, "ui viewport"),

    INT_SETTING(F_TIME_SETTING, resume_rewind, LANG_RESUME_REWIND, 0,
                "resume rewind", UNIT_SEC, 0, 60, 5,
                formatter_time_unit_0_is_off, NULL),
   CUSTOM_SETTING(0, root_menu_customized,
                  LANG_ROCKBOX_TITLE, /* lang string here is never actually used */
                  NULL, "root menu order",
                  root_menu_load_from_cfg, root_menu_write_to_cfg,
                  root_menu_is_changed, root_menu_set_default),

};

const int nb_settings = sizeof(settings)/sizeof(*settings);

const struct settings_list* get_settings_list(int*count)
{
    *count = nb_settings;
    return settings;
}
