/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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

/* Trimpod: audiohw for the ALSA PCM output.  Volume is split -- the coarse
 * attenuation goes to a mixer control, software carries only the remainder.
 *
 * A mixer only attenuates what passes through it, so WHICH control depends on
 * where the OS is sending audio: the codec's 'digital volume' for the internal
 * speaker, BlueALSA's A2DP volume for a Bluetooth device.  The active sink
 * comes from the shared settings block NextUI's audiomon maintains.
 *
 * Driving the sink's own control also means the level survives exit, so the
 * volume the user leaves is the volume the system keeps -- nothing to pin and
 * restore, and nothing left wrong if the app is killed.
 *
 * Keep the software remainder small: deep attenuation in a 16-bit multiply
 * quantizes quiet passages to silence (at -40 dB everything under -56 dBFS
 * rounds to zero).  If no control can be driven the code falls back to
 * software-only, which still works, just with that limitation. */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include "config.h"
#include "sound.h"
#include "fixedpoint.h"
#include "pcm_sw_volume.h"
#include "trimpod_alsa.h"

/* Codec 'digital volume': 0..63, 1.16 dB/step, 0 = loudest.  The driver's TLV
 * dB info has the direction backwards -- do not "correct" this from it. */
#define DV_CTL_CARD   "hw:audiocodec"
#define DV_CTL_NAME   "digital volume"
#define DV_STEP_MDB   1160  /* milli-dB per hardware step */
#define DV_MAX_STEPS  63

/* NextUI publishes the active audio sink in its shared settings block, updated
 * by its audiomon daemon on connect/disconnect.  Byte 112 is SettingsV11 field
 * 29; the same arithmetic puts toggled_volume at byte 56, which is where
 * launch.sh pokes MutedVolume, and the file is 132 bytes = that struct's
 * 33 ints. */
#define SHM_SETTINGS      "/dev/shm/SharedSettings"
#define SHM_AUDIOSINK_OFF 112
#define SINK_SPEAKER      TRIMPOD_SINK_SPEAKER

/* stdio, not the Rockbox file API -- that one is macro-redirected and would
 * mangle a host path (firmware/target/hosted/sysfs.c does the same). */
int trimpod_audio_sink(void)
{
    FILE *f = fopen(SHM_SETTINGS, "rb");
    int32_t sink = SINK_SPEAKER;        /* NextUI absent -> speaker */

    if (f)
    {
        if (fseek(f, SHM_AUDIOSINK_OFF, SEEK_SET) != 0 ||
            fread(&sink, sizeof sink, 1, f) != 1)
            sink = SINK_SPEAKER;
        fclose(f);
    }
    return sink;
}

/* A USB DAC arrives as a card of its own, so unlike Bluetooth it can be named.
 * "The first card that is not audiocodec" is NextUI's own rule -- see
 * get_usbc_card_num() in workspace/tg5040/libmsettings/msettings.c. */
int trimpod_usb_card(void)
{
    FILE *f = fopen("/proc/asound/cards", "r");
    char line[256], id[32];
    int card, found = -1;

    if (!f)
        return -1;

    /* " 1 [earphones      ]: USB-Audio - onn. USB-C earphones", followed by a
     * continuation line that cannot match this shape. */
    while (found < 0 && fgets(line, sizeof line, f))
        if (sscanf(line, " %d [%31[^] ]", &card, id) == 2 &&
            strcmp(id, "audiocodec") != 0)
            found = card;

    fclose(f);
    return found;
}

/* snd_config_update_free_global() is not thread-safe, and the PCM writer thread
 * calls it on every (re)open while this file's mixer handles are live on the
 * Rockbox side.  One lock covers both.  Static init because the first volume
 * change can land before any explicit setup. */
static pthread_mutex_t alsa_cfg_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- internal speaker: the codec's own control --------------------------- */

static snd_ctl_t *dv_ctl = NULL;
static snd_ctl_elem_value_t *dv_val = NULL;
static bool dv_open_tried = false;

/* One open attempt; on failure software carries the whole volume range. */
static void hw_dv_open(void)
{
    if (dv_open_tried)
        return;
    dv_open_tried = true;

    if (snd_ctl_open(&dv_ctl, DV_CTL_CARD, 0) < 0)
    {
        dv_ctl = NULL;
        return;
    }

    if (snd_ctl_elem_value_malloc(&dv_val) < 0)
    {
        snd_ctl_close(dv_ctl);
        dv_ctl = NULL;
        return;
    }

    snd_ctl_elem_value_set_interface(dv_val, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_value_set_name(dv_val, DV_CTL_NAME);
}

/* ---- Bluetooth: BlueALSA's A2DP volume, out of process ------------------
 *
 * Deliberately NOT snd_ctl_open("bluealsa").  Doing that loads
 * libasound_module_ctl_bluealsa.so -- with its own D-Bus connection and thread
 * -- into this address space, and when the speaker is switched off that plugin
 * faults and takes the app with it.  Every crash log named that library, a
 * PCM-only probe survived the same disconnect repeatedly, and NextUI (which
 * never calls snd_ctl_open anywhere) does not crash.
 *
 * So the mixer runs as a child process, exactly as NextUI's SetRawVolume does:
 * "bluealsa is a mixer plugin, not exposed as a separate card".  A dying plugin
 * can then only kill the child.  amixer inherits our HOME, so ctl.!default
 * resolves to the same BlueALSA control audiomon configured.
 *
 * Volume changes are user keypresses, so the fork/exec cost is irrelevant. */

static char bt_ctl_name[128];
static bool bt_ctl_looked_up;

/* H700 exposes BlueALSA as an explicit control device; tg5040 historically
 * resolves it through ctl.!default in audiomon's .asoundrc. */
static const char *bt_amixer_device_arg(void)
{
    const char *device = getenv("TRIMPOD_BT_MIXER_DEVICE");
    return device && strcmp(device, "bluealsa") == 0 ? "-D bluealsa " : "";
}

/* First A2DP simple control, as listed by amixer.  Same scan NextUI does. */
static void bt_find_control(void)
{
    FILE *fp;
    char line[256], cmd[128];

    if (bt_ctl_looked_up)
        return;
    bt_ctl_looked_up = true;
    bt_ctl_name[0] = '\0';

    snprintf(cmd, sizeof cmd, "amixer %sscontrols 2>/dev/null",
             bt_amixer_device_arg());
    fp = popen(cmd, "r");
    if (!fp)
        return;

    while (fgets(line, sizeof line, fp))
    {
        char *start = strchr(line, '\'');
        char *end = strrchr(line, '\'');

        if (!start || !end || end <= start)
            continue;
        *end = '\0';
        if (!strstr(start + 1, "A2DP") || strchr(start + 1, '"'))
            continue;               /* skip anything we would have to escape */
        snprintf(bt_ctl_name, sizeof bt_ctl_name, "%s", start + 1);
        break;
    }

    pclose(fp);
}

static void bt_forget(void)
{
    bt_ctl_looked_up = false;
    bt_ctl_name[0] = '\0';
}

/* ---- USB DAC: the card's own playback volume, out of process ---------------
 *
 * A child again, but for the stack rather than for safety: this runs on the
 * 33 KB audio-thread stack at every song start, where snd_mixer_* costs
 * 11,920 B and wedges the app (auto-memory never-reopen-alsa-handles-per-call).
 * Only snd_ctl_elem_write is cheap enough to stay in process and it cannot
 * discover an element, so amixer does the discovery.
 *
 * The element is NextUI's choice (SetRawVolume) plus a dB range, which NextUI
 * ignores and we cannot: the dial is in dB, and a bare 0..N scale cannot say
 * how much attenuation landed.  No dB info means no hardware stage.
 *
 * Two amixer traps, both measured: `sset <ctl> <v> playback` writes the CAPTURE
 * volume too, and `cset` reads "-12dB" as 0, i.e. silence.  Hence cset by
 * element name, with a raw value. */
static char usb_ctl_name[128];
static int  usb_ctl_card;
static int  usb_ctl_min, usb_ctl_max;       /* raw range */
static int  usb_ctl_mindb, usb_ctl_maxdb;   /* milli-dB at min / at max */
static bool usb_ctl_looked_up;

/* "-60.00dB" -> -60000.  Sign is stripped first: %d on "-0.50" yields 0 and
 * loses it. */
static bool parse_mdb(const char *s, int *out)
{
    int sign = 1, whole, cents;

    if (*s == '-')
    {
        sign = -1;
        s++;
    }
    else if (*s == '+')
        s++;

    if (sscanf(s, "%d.%2d", &whole, &cents) != 2)
        return false;

    *out = sign * (whole * 1000 + cents * 10);
    return true;
}

#define USB_DB_TAG "dBminmax-min="
#define USB_DBMAX_TAG ",max="

/* One element spans several lines of `amixer contents`, and the name comes
 * first, so a candidate is held until its type and dB lines confirm it:
 *
 *   numid=6,iface=MIXER,name='Headset Playback Volume'
 *     ; type=INTEGER,access=rw---R--,values=1,min=0,max=120,step=0
 *     : values=60
 *     | dBminmax-min=-60.00dB,max=0.00dB
 */
static void usb_find_control(void)
{
    FILE *fp;
    char line[256], cmd[64], cand[128] = "";
    bool typed = false;

    if (usb_ctl_looked_up)
        return;
    usb_ctl_looked_up = true;
    usb_ctl_name[0] = '\0';

    usb_ctl_card = trimpod_usb_card();
    if (usb_ctl_card < 0)
        return;

    snprintf(cmd, sizeof cmd, "amixer -c %d contents 2>/dev/null", usb_ctl_card);
    fp = popen(cmd, "r");
    if (!fp)
        return;

    while (!usb_ctl_name[0] && fgets(line, sizeof line, fp))
    {
        char name[128];
        char *db;
        int lo, hi, mindb, maxdb;

        if (sscanf(line, " numid=%*d,iface=MIXER,name='%127[^']'", name) == 1)
        {
            typed = false;
            cand[0] = '\0';
            /* NextUI's rule, plus two guards it lacks: its match also hits
             * "PCM Capture Volume", an input, and the name is re-emitted
             * inside single quotes. */
            if ((strstr(name, "Playback") || strstr(name, "PCM")) &&
                (strstr(name, "Volume") || strstr(name, "volume")) &&
                !strstr(name, "Capture") && !strchr(name, '\''))
                snprintf(cand, sizeof cand, "%s", name);
            continue;
        }

        if (!cand[0])
            continue;

        if (sscanf(line, " ; type=INTEGER,%*[^,],values=%*d,min=%d,max=%d",
                   &lo, &hi) == 2)
        {
            usb_ctl_min = lo;
            usb_ctl_max = hi;
            typed = hi > lo;
            continue;
        }

        db = strstr(line, USB_DB_TAG);
        if (!typed || !db)
            continue;

        db += sizeof USB_DB_TAG - 1;
        if (!parse_mdb(db, &mindb))
            continue;
        db = strstr(db, USB_DBMAX_TAG);
        if (!db || !parse_mdb(db + sizeof USB_DBMAX_TAG - 1, &maxdb) ||
            maxdb <= mindb)
            continue;

        usb_ctl_mindb = mindb;
        usb_ctl_maxdb = maxdb;
        snprintf(usb_ctl_name, sizeof usb_ctl_name, "%s", cand);
    }

    pclose(fp);
}

static void usb_forget(void)
{
    usb_ctl_looked_up = false;
    usb_ctl_name[0] = '\0';
}

/* Attenuate by `att` milli-dB from the control's top, reporting back what it
 * actually reached: claiming the REQUEST would leave the output too loud by
 * whatever the control clamped away. */
static bool usb_set_attenuation(int att, int *achieved)
{
    long span_db, span_raw;
    int raw;
    char cmd[256];

    usb_find_control();
    if (!usb_ctl_name[0])
        return false;

    span_db  = usb_ctl_maxdb - usb_ctl_mindb;
    span_raw = usb_ctl_max - usb_ctl_min;

    /* USB Audio Class volume is linear in dB, and this card measures exactly
     * so: 0..120 over -60..0 dB, i.e. 0.5 dB a step.
     *
     * Truncate, never round to nearest: the control must not overshoot the
     * request, or the remainder handed back to pcm_set_master_volume() is
     * NEGATIVE, and pcm_centibels_to_factor() turns that into a gain above
     * unity with nothing clamping it.  The codec path truncates for the same
     * reason.  Software then makes up under one step. */
    raw = usb_ctl_max - (int)(att * span_raw / span_db);
    if (raw < usb_ctl_min)
        raw = usb_ctl_min;
    if (raw > usb_ctl_max)
        raw = usb_ctl_max;

    snprintf(cmd, sizeof cmd,
             "amixer -c %d cset name='%s' %d >/dev/null 2>&1",
             usb_ctl_card, usb_ctl_name, raw);
    if (system(cmd) != 0)
        return false;

    *achieved = (int)(((long)usb_ctl_max - raw) * span_db / span_raw);
    return true;
}

/* Returns true if the control took the level. */
static bool bt_set_percent(int percent)
{
    char cmd[256];

    bt_find_control();
    if (!bt_ctl_name[0])
        return false;

    snprintf(cmd, sizeof cmd,
             "amixer %s-M sset \"%s\" %d%% >/dev/null 2>&1",
             bt_amixer_device_arg(), bt_ctl_name, percent);
    return system(cmd) == 0;
}

void trimpod_alsa_lock(void)   { pthread_mutex_lock(&alsa_cfg_lock); }
void trimpod_alsa_unlock(void) { pthread_mutex_unlock(&alsa_cfg_lock); }

void trimpod_alsa_forget_sink(void)
{
    pthread_mutex_lock(&alsa_cfg_lock);
    bt_forget();
    usb_forget();
    pthread_mutex_unlock(&alsa_cfg_lock);
}

/* Q16 amplitude factor for an attenuation in milli-dB (unity = 1<<16). */
static int32_t mdb_to_factor(int att_mdb)
{
    if (att_mdb <= 0)
        return 1 << 16;
    long f = fp_factor(fp_div(-att_mdb, 1000, 16), 16);
    return f >= (1 << 16) ? (1 << 16) : (int32_t)f;
}

/* Volume arrives in tenth-dB (trimpod_codec.h numdecimals=1) and is <= 0; the
 * bottom of the range is a hard mute.
 *
 * "Arrives <= 0" is not something the settings layer guarantees.  The volume is
 * F_ALLOW_ARBITRARY_VALS, and a cfg value with no cfgvals table is loaded by a
 * bare atoi() with no range check at all (apps/settings.c string_to_cfg), so a
 * stale or hand-edited .resume.cfg lands here verbatim -- and sound_set() does
 * not clamp either.  Above maxval every attenuation below goes NEGATIVE, which
 * ends in pcm_centibels_to_factor() as a gain factor above unity, unclamped.
 * Clamp on the way in so no route's mapping has to defend itself. */
static int clamp_volume(int vol)
{
    int lo = sound_min(SOUND_VOLUME), hi = sound_max(SOUND_VOLUME);

    return vol < lo ? lo : vol > hi ? hi : vol;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    static int last_sink = -1;
    int sink = trimpod_audio_sink();

    vol_l = clamp_volume(vol_l);
    vol_r = clamp_volume(vol_r);

    /* Held across the whole body: the PCM writer thread can free the config
     * tree these handles came from at any moment. */
    pthread_mutex_lock(&alsa_cfg_lock);

    if (sink != last_sink)
    {
        last_sink = sink;
        bt_forget();                    /* re-probe once for the new route */
        usb_forget();
    }

    /* attenuations in milli-dB, INT_MAX = mute */
    int att_l = vol_l <= sound_min(SOUND_VOLUME) ? INT_MAX : -vol_l * 100;
    int att_r = vol_r <= sound_min(SOUND_VOLUME) ? INT_MAX : -vol_r * 100;
    int common = att_l < att_r ? att_l : att_r;
    int hw_mdb = 0;

    if (sink == SINK_SPEAKER)
    {
        hw_dv_open();
        if (dv_ctl)
        {
            int steps = common == INT_MAX ? DV_MAX_STEPS
                                          : common / DV_STEP_MDB;
            if (steps > DV_MAX_STEPS)
                steps = DV_MAX_STEPS;

            snd_ctl_elem_value_set_integer(dv_val, 0, steps);
            snd_ctl_elem_write(dv_ctl, dv_val);
            hw_mdb = steps * DV_STEP_MDB;
        }
    }
    /* Mute stays a software zero on the off-codec routes -- instant, and it
     * leaves the device's own level where the user set it. */
    else if (common != INT_MAX)
    {
        if (sink == TRIMPOD_SINK_USB)
        {
            /* A USB DAC reports a real dB scale, so ask for the attenuation
             * directly and take back only what the control reached. */
            int got;

            if (usb_set_attenuation(common, &got))
                hw_mdb = got;
        }
        else
        {
            /* A2DP volume is defined as linear in LOUDNESS, so step it ~2x per
             * 10 dB; an amplitude map would collapse the dial.  When the
             * control takes it, it carries the whole attenuation and software
             * stays at unity, which keeps the 16-bit truncation floor out of
             * the way. */
            int percent = (int)((mdb_to_factor(common * 602 / 1000) * 100) >> 16);

            if (percent < 0)
                percent = 0;
            if (percent > 100)
                percent = 100;

            if (bt_set_percent(percent))
                hw_mdb = common;
        }
    }

    pthread_mutex_unlock(&alsa_cfg_lock);

    /* software covers what the control did not, rounded to the setting's
     * tenth-dB unit (<=0.05 dB of rounding error) */
    int rem_l = att_l == INT_MAX ? PCM_MUTE_LEVEL : -((att_l - hw_mdb + 50) / 100);
    int rem_r = att_r == INT_MAX ? PCM_MUTE_LEVEL : -((att_r - hw_mdb + 50) / 100);

    pcm_set_master_volume(rem_l, rem_r);
}

/* shutdown_hw() calls this after audio_stop(); stopping the writer here is what
 * lets the process actually exit instead of hanging on "Shutting Down". */
void audiohw_close(void)
{
    pcm_alsa_shutdown();
}
