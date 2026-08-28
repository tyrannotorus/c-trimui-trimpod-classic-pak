/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Nick Lanham
 * Copyright (C) 2010 by Thomas Martitz
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

/* Trimpod: PCM output via ALSA, ported from upstream Rockbox
 * firmware/target/hosted/pcm-alsa.c.  set_hwparams(), set_swparams(),
 * copy_frames() and the XRUN/prepare/start state machine are upstream's.
 *
 * Four deviations, each forced by this device.  Do not "restore" them:
 *
 * 1. THE DEVICE IS NAMED PER ROUTE, never the hardware and never `default`.
 *    Upstream uses plughw:0,0, which does not even open here.  Naming the codec
 *    directly is worse: its driver advertises RATE (0 4294967295], claiming
 *    every rate and honouring none -- fed 44100 it still clocks 48000, so a
 *    4.000 s file plays in 3.66 s, 8.8% fast, about 1.5 semitones sharp.
 *    Measured.  That is the pitch shift v1.0.4 shipped.
 *
 * 2. NO SIGIO.  Upstream is driven by snd_async_add_pcm_handler(), which does
 *    not work on BlueALSA's ioplug, so a writer thread paces on snd_pcm_wait()
 *    instead.  This is the only structural change.
 *
 * 3. NOTHING IS FATAL.  Upstream panicf()s when a device will not open, which
 *    ends in a while(1) only adb can clear.  Upstream has no Bluetooth, so it
 *    never contemplates a device that vanishes mid-song; here that is routine.
 *
 * 4. BLUETOOTH OUTPUT RUNS IN A CHILD PROCESS.  bluez-alsa's plugin crashes
 *    its host process when a speaker is powered off; see bt_pipe below.
 *
 * 5. LOSING A PRIVATE ROUTE PAUSES PLAYBACK -- Bluetooth or USB-C headphones --
 *    rather than following the route back to the internal speaker; see
 *    route_notify_lost(). */

#include "autoconf.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <SDL.h>
#include "config.h"
#include "debug.h"
#include "sound.h"
#include "audiohw.h"
#include "system.h"
#include "system-sdl.h"
#include "kernel.h"
#include "button.h"
#include "panic.h"
#include "trimpod_alsa.h"

#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sampr.h"
#include "pcm_mixer.h"
#include "pcm_sink.h"

/*#define LOGF_ENABLE*/
#include "logf.h"


extern const char *audiodev;        /* --audiodev; NULL = ALSA "default" */

static const snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
static const unsigned int channels = 2;
#define FRAME_BYTES 4               /* S16_LE stereo */

/* snd_pcm_wait() timeout.  Bounds how long a reopen or shutdown waits behind
 * the writer, and how quickly a dead route is noticed. */
#define WAIT_TIMEOUT_MS 50
#define REOPEN_DELAY_MS 500
/* Re-reading NextUI's settings block is a syscall round trip; keep it off the
 * ~23 ms write path. */
#define ROUTE_POLL_MS   500

/* Depth of the Bluetooth child's ALSA buffer, in periods of MIX_FRAME_SAMPLES
 * (~23 ms each at 44.1 kHz).  Eight is ~186 ms: comparable to the ~170 ms dmix
 * imposes on the speaker route, which plays without underruns, and short enough
 * to keep the visualiser tap near the sound. */
#define BT_BUFFER_PERIODS 8

/* Linux-specific and only declared under _GNU_SOURCE, which this build does not
 * define.  The value is fixed (F_LINUX_SPECIFIC_BASE + 7) on every arch. */
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

/* handle, and everything derived from it, belongs to the writer thread alone.
 * Nothing else opens, closes or touches it -- see sink_set_freq(). */
static snd_pcm_t *handle;
static snd_pcm_uframes_t buffer_size;
static snd_pcm_uframes_t period_size;
static unsigned int real_sample_rate;
static unsigned int last_sample_rate;
static volatile unsigned int req_sample_rate;
static int16_t *frames;

/* Bluetooth output runs in a CHILD PROCESS.  Never open a BlueALSA PCM here.
 *
 * bluez-alsa's plugin starts its own "pcm-io" thread inside whatever process
 * opens it.  When the speaker is switched off (an abrupt link loss, not a clean
 * disconnect) that thread calls through a function pointer that is no longer
 * valid and dies, taking the process with it.  The kernel names it:
 *
 *     CPU: 3 PID: 21686 Comm: pcm-io
 *     pc : [<00000000052c8770>] lr : [<000000000044f7d0>]
 *
 * pc there is Rockbox's data buffer, i.e. it branched into non-executable
 * memory.  We do not own that thread and never call into it, and BOTH leaving
 * the dead PCM open AND closing it were tried -- it dies either way.
 *
 * Piping the samples to aplay puts the plugin in a child.  When the speaker
 * dies the child dies, our write returns EPIPE, and we reopen on whatever the
 * route became.  Measured: 4.22 s for a 4.000 s file, i.e. aplay paces itself
 * against the sink and the pipe back-pressures us, so no extra clock is needed
 * and the pitch is unaffected. */
static FILE *bt_pipe;
static bool route_lost_sent;        /* pause posted for this route already */
static int  open_route;             /* the sink dev_open() opened for */

static const void *pcm_data;
static size_t pcm_size;

/* Upstream's recursive pcm_mtx, and for the same reason: the pcm layer calls
 * ops.stop() from inside pcm_play_dma_complete_callback(), i.e. re-entrantly on
 * whichever thread is pulling data. */
static pthread_mutex_t pcm_mtx;

static SDL_Thread *writer_thread;
static volatile bool writer_run;
static volatile bool dev_playing;   /* Rockbox has handed us a buffer */
static int cur_route;               /* sink audiomon last published */

static void viz_pcm_push(const void *src_v, unsigned frames_n);

/* Bluetooth uses bt_pipe and leaves `handle` NULL, so every "do we have an
 * output?" test must ask this, not `handle`.  Getting that wrong meant the
 * writer never reached writer_step() on Bluetooth -- no audio at all -- while
 * re-running dev_open() every 20 ms and spawning an aplay child each time. */
static bool dev_is_open(void)
{
    return handle != NULL || bt_pipe != NULL;
}

static void sink_lock(void)
{
    pthread_mutex_lock(&pcm_mtx);
}

static void sink_unlock(void)
{
    pthread_mutex_unlock(&pcm_mtx);
}

/* --- device parameters (upstream) ----------------------------------------- */

static int set_hwparams(snd_pcm_t *pcm, unsigned long sampr)
{
    snd_pcm_hw_params_t *params;
    unsigned int srate;
    int err;

    snd_pcm_hw_params_malloc(&params);

    if (sampr > SAMPR_96) {
        buffer_size = MIX_FRAME_SAMPLES * 4 * 4;
        period_size = MIX_FRAME_SAMPLES * 4;
    } else if (sampr > SAMPR_48) {
        buffer_size = MIX_FRAME_SAMPLES * 2 * 4;
        period_size = MIX_FRAME_SAMPLES * 2;
    } else {
        buffer_size = MIX_FRAME_SAMPLES * 4;
        period_size = MIX_FRAME_SAMPLES;
    }

    err = snd_pcm_hw_params_any(pcm, params);
    if (err < 0)
    {
        logf("Broken configuration for playback: %s", snd_strerror(err));
        goto error;
    }
    err = snd_pcm_hw_params_set_access(pcm, params,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0)
    {
        logf("Access type not available for playback: %s", snd_strerror(err));
        goto error;
    }
    err = snd_pcm_hw_params_set_format(pcm, params, format);
    if (err < 0)
    {
        logf("Sample format not available for playback: %s", snd_strerror(err));
        goto error;
    }
    err = snd_pcm_hw_params_set_channels(pcm, params, channels);
    if (err < 0)
    {
        logf("Channel count not available for playback: %s", snd_strerror(err));
        goto error;
    }
    srate = sampr;
#ifdef TRIMPOD_H700
    /* sunxi-snd-codec advertises a wide range. Its "near" constraints can
     * silently renegotiate 48 kHz to 192 kHz when period/buffer sizes are set
     * afterwards, producing 4x playback. This exact tuple is the native H700
     * path and is also what NextUI's ALSA device accepts. */
    err = snd_pcm_hw_params_set_rate(pcm, params, srate, 0);
#else
    err = snd_pcm_hw_params_set_rate_near(pcm, params, &srate, 0);
#endif
    if (err < 0)
    {
        logf("Rate %luHz not available for playback: %s", sampr, snd_strerror(err));
        goto error;
    }
    real_sample_rate = srate;
    /* Upstream's check, and it must stay: a substituted rate is exactly how
     * playback ends up pitched.  Through `default` it never trips. */
    if (real_sample_rate != sampr)
    {
        logf("Rate doesn't match (requested %luHz, got %dHz)", sampr,
             real_sample_rate);
        err = -EINVAL;
        goto error;
    }

#ifdef TRIMPOD_H700
    err = snd_pcm_hw_params_set_buffer_size(pcm, params, buffer_size);
#else
    err = snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_size);
#endif
    if (err < 0)
    {
        logf("Unable to set buffer size %ld: %s", buffer_size, snd_strerror(err));
        goto error;
    }
#ifdef TRIMPOD_H700
    err = snd_pcm_hw_params_set_period_size(pcm, params, period_size, 0);
#else
    err = snd_pcm_hw_params_set_period_size_near(pcm, params, &period_size, NULL);
#endif
    if (err < 0)
    {
        logf("Unable to set period size %ld: %s", period_size, snd_strerror(err));
        goto error;
    }

    if (frames) free(frames);
    frames = calloc(1, period_size * channels * sizeof(int16_t));

    /* Acquiring a Bluetooth transport happens here and takes ~1.8 s. */
    err = snd_pcm_hw_params(pcm, params);
    if (err < 0)
    {
        logf("Unable to set hw params: %s", snd_strerror(err));
        goto error;
    }

    err = 0;
error:
    snd_pcm_hw_params_free(params);
    return err;
}

static int set_swparams(snd_pcm_t *pcm)
{
    snd_pcm_sw_params_t *swparams;
    int err;

    snd_pcm_sw_params_malloc(&swparams);

    err = snd_pcm_sw_params_current(pcm, swparams);
    if (err < 0)
    {
        logf("Unable to determine current swparams: %s", snd_strerror(err));
        goto error;
    }
    err = snd_pcm_sw_params_set_start_threshold(pcm, swparams, buffer_size / 2);
    if (err < 0)
    {
        logf("Unable to set start threshold: %s", snd_strerror(err));
        goto error;
    }
    err = snd_pcm_sw_params_set_avail_min(pcm, swparams, period_size);
    if (err < 0)
    {
        logf("Unable to set avail min: %s", snd_strerror(err));
        goto error;
    }
    err = snd_pcm_sw_params(pcm, swparams);
    if (err < 0)
    {
        logf("Unable to set sw params: %s", snd_strerror(err));
        goto error;
    }

    err = 0;
error:
    snd_pcm_sw_params_free(swparams);
    return err;
}

/* --- device lifecycle ------------------------------------------------------ */

/* Name the device for the current route instead of opening `default`.
 *
 * `default` is a trap here.  audiomon rewrites and deletes ~/.asoundrc as
 * routes change, alsa-lib caches the tree, and the ONLY call that drops that
 * cache -- snd_config_update_free_global() -- unloads the BlueALSA plugin .so
 * out from under the ioplug we are still holding.  That, closing a dead
 * BlueALSA PCM, and closing a dead BlueALSA mixer all segfault; all three were
 * caught in trimpod.log switching the speaker off.
 *
 * All three routes have stable names that need no cache invalidation:
 *   speaker   "Playback" on tg5040; "default" on H700, whose Rockbox mixer is
 *              configured for the codec's native 48 kHz rate.
 *   usb       the card itself, found the way NextUI finds it; see usb_device().
 *   bluetooth the address audiomon just wrote into ~/.asoundrc, read as plain
 *              text rather than through alsa-lib's cached view of it. */
static bool bt_address(char *mac, size_t len)
{
    const char *home = getenv("HOME");
    char line[256];
    FILE *f;

    if (!home)
        return false;

    snprintf(line, sizeof line, "%s/.asoundrc", home);
    f = fopen(line, "r");
    if (!f)
        return false;                   /* route says BT, audiomon disagrees */

    while (fgets(line, sizeof line, f))
    {
        if (sscanf(line, " defaults.bluealsa.device \"%31[^\"]\"", mac) == 1 &&
            strlen(mac) < len && !strpbrk(mac, "'\\\\"))
        {
            fclose(f);
            return true;
        }
    }

    fclose(f);
    return false;
}

/* The Bluetooth device is spelled "bluealsa:DEV=<mac>,PROFILE=a2dp".
 * /usr/share/alsa/alsa.conf.d/20-bluealsa.conf defines pcm.bluealsa as `type
 * plug` over `type bluealsa`, so this is the identical device NextUI's minarch
 * reaches via AUDIODEV=bluealsa. */
static bool route_is_bt(void)
{
    char mac[32];

    return !audiodev && trimpod_audio_sink() == TRIMPOD_SINK_BT &&
           bt_address(mac, sizeof mac);
}

/* USB-C headphones are a USB Audio Class card, so this route is a plain ALSA
 * device -- but through `plug`.  A USB card advertises only the rates it really
 * has (these earphones: 44.1/48/96k) and SUBSTITUTES one rather than failing,
 * which set_hwparams() rejects, so a bare `hw:` route would never open. */
static bool usb_device(char *buf, size_t len)
{
    int card;

    if (trimpod_audio_sink() != TRIMPOD_SINK_USB)
        return false;

    card = trimpod_usb_card();
    if (card < 0)
        return false;               /* route says USB, no card to name */

    snprintf(buf, len, "plughw:%d,0", card);
    return true;
}

/* Start the child that owns the BlueALSA plugin. */
static bool bt_open(void)
{
    char mac[32], cmd[256];

    if (!bt_address(mac, sizeof mac))
        return false;

    period_size = MIX_FRAME_SAMPLES;
    buffer_size = period_size * BT_BUFFER_PERIODS;
    if (frames) free(frames);
    frames = calloc(1, period_size * channels * sizeof(int16_t));
    if (!frames)
        return false;

    /* Bound the child's buffering.  Left to itself aplay takes a 500 ms buffer
     * and reads stdin in 125 ms chunks, so it released us one 125 ms chunk at a
     * time -- and the visualiser ring is filled in copy_frames(), at the moment
     * we hand bytes to the pipe.  The spectrum therefore advanced in jumps and
     * ran ahead of the sound by the whole depth of the pipe plus that buffer.
     * One period per read paces the tap like the ALSA route's snd_pcm_wait().
     *
     * exec so pclose() waits on aplay itself, not an intervening shell. */
    snprintf(cmd, sizeof cmd,
             "exec aplay -q -t raw -f S16_LE -c 2 -r %u "
             "--period-size=%lu --buffer-size=%lu "
             "-D 'bluealsa:DEV=%s,PROFILE=a2dp' -",
             last_sample_rate, (unsigned long)period_size,
             (unsigned long)buffer_size, mac);

    bt_pipe = popen(cmd, "w");
    if (!bt_pipe)
    {
        fprintf(stderr, "pcm: could not start bluetooth helper\n");
        return false;
    }

    setvbuf(bt_pipe, NULL, _IONBF, 0);   /* never hold a period in stdio */

    /* A pipe holds 64 KB by default -- another 372 ms of audio between the
     * visualiser tap and the child.  Two periods is all aplay needs to stay
     * fed, and the write side waits on poll() so the small buffer costs
     * nothing.  Best effort: an old kernel that refuses just keeps 64 KB. */
    fcntl(fileno(bt_pipe), F_SETPIPE_SZ, (int)(2 * period_size * FRAME_BYTES));

    route_lost_sent = false;
    open_route = TRIMPOD_SINK_BT;
    real_sample_rate = last_sample_rate;
    fprintf(stderr, "pcm: bluetooth via child aplay (%s) at %u Hz\n",
            mac, last_sample_rate);
    return true;
}

static bool dev_open(void)
{
    char usb[32];
    const char *name;
    int route = TRIMPOD_SINK_SPEAKER;
    int err;

    if (route_is_bt())
        return bt_open();

    if (audiodev)
        name = audiodev;                /* --audiodev overrides every route */
    else if (usb_device(usb, sizeof usb))
    {
        name = usb;
        route = TRIMPOD_SINK_USB;
    }
    else
#ifdef TRIMPOD_H700
        name = "default";
#else
        name = "Playback";
#endif

    /* Serialises against the volume driver's mixer opens: both rebuild
     * alsa-lib's global config on first use. */
    trimpod_alsa_lock();

    err = snd_pcm_open(&handle, name, SND_PCM_STREAM_PLAYBACK,
                       SND_PCM_NONBLOCK);
    if (err < 0)
    {
        /* Upstream panicf()s here.  A route can legitimately be gone. */
        handle = NULL;
        trimpod_alsa_unlock();
        fprintf(stderr, "pcm: open %s failed (%s)\n", name, snd_strerror(err));
        return false;
    }

    if (set_hwparams(handle, last_sample_rate) < 0 ||
        set_swparams(handle) < 0 ||
        snd_pcm_prepare(handle) < 0)
    {
        snd_pcm_close(handle);          /* never opened a stream; safe to close */
        handle = NULL;
        trimpod_alsa_unlock();
        fprintf(stderr, "pcm: configuring %s failed\n", name);
        return false;
    }

    trimpod_alsa_unlock();
    open_route = route;
    route_lost_sent = false;
    fprintf(stderr, "pcm: opened %s at %u Hz (period %lu, buffer %lu)\n",
            name, real_sample_rate, (unsigned long)period_size,
            (unsigned long)buffer_size);
    return true;
}

/* Orderly teardown: shutdown, a rate change, or a route change.
 *
 * A BlueALSA PCM is NEVER closed, even here.  A route change is usually the
 * news that the speaker just died, and the route poll can beat the write error
 * to it, so "we chose to close it" is no evidence the device still exists.
 * Only the codec, which cannot disappear, is closed properly. */
static void dev_close(void)
{
    snd_pcm_t *h = handle;

    if (bt_pipe)
    {
        FILE *pipe = bt_pipe;

        bt_pipe = NULL;
        pclose(pipe);                   /* EOF; aplay drains and exits */
        fprintf(stderr, "pcm: bluetooth helper closed\n");
        return;
    }

    if (!h)
        return;

    handle = NULL;
    trimpod_alsa_lock();
    snd_pcm_drop(h);                    /* not drain: never wait on the sink */
    snd_pcm_close(h);
    trimpod_alsa_unlock();
    logf("pcm: closed");
}

/* The device reported that it is gone.  CLOSE it -- do not merely drop the
 * pointer.  bluez-alsa's plugin runs its own "pcm-io" thread inside this
 * process; leaving a dead PCM open leaves that thread alive, and it then jumps
 * to a garbage address and kills us:
 *
 *     CPU: 2 PID: 20922 Comm: pcm-io
 *     pc : [<0000000000000011>] lr : [<0000000000000011>]
 *
 * NextUI's minarch closes the device as soon as audiomon rewrites ~/.asoundrc
 * and survives a speaker power-off, so closing is the behaviour that works. */
/* A private route is gone -- a Bluetooth speaker switched off, USB-C headphones
 * pulled out.  PAUSE rather than letting the writer follow the route home:
 * audiomon repoints it at the internal speaker within a second, and moving a
 * private listen to the loudspeaker unasked is the worse failure -- pausing is
 * what VLC and phones do when an output disappears.
 *
 * Posted, not called.  audio_pause() is a blocking queue_send and must run on a
 * Rockbox thread; sim_enter_irq_handler() is how a foreign thread reaches the
 * kernel here, the same way button-sdl.c's SDL event thread posts keypresses.
 * apps/misc.c handles the event.  Once per route: without the flag a route that
 * keeps reopening and dying would post one every REOPEN_DELAY_MS. */
static void route_notify_lost(void)
{
    if (route_lost_sent)
        return;
    route_lost_sent = true;

    sim_enter_irq_handler();
    button_queue_post(SYS_PHONE_UNPLUGGED, 0);
    sim_exit_irq_handler();
}

static void dev_lost(const char *why)
{
    bool was_private = open_route != TRIMPOD_SINK_SPEAKER;

    fprintf(stderr, "pcm: device lost (%s), closing\n", why);

    /* Pause BEFORE closing: closing waits on the Bluetooth child, and a child
     * wedged against a dead speaker must not hold up the one thing the user
     * hears. */
    if (was_private)
        route_notify_lost();

    dev_close();
    trimpod_alsa_forget_sink();
}

/* --- pulling audio from Rockbox (upstream copy_frames) --------------------- */

/* Fill one period, padding with silence when Rockbox has nothing.  Keeping the
 * stream fed rather than stopping it avoids the click a restart makes, and is
 * what the SDL path did while paused. */
static void copy_frames(void)
{
    ssize_t frames_left = period_size;
    bool new_buffer = false;

    sink_lock();

    while (frames_left > 0 && dev_playing)
    {
        if (!pcm_size)
        {
            new_buffer = true;
            if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &pcm_data,
                                                &pcm_size))
                break;
        }

        if (pcm_size % FRAME_BYTES)
        {
            logf("Wrong pcm_size");     /* upstream panicf()s; just resync */
            pcm_size &= ~(size_t)(FRAME_BYTES - 1);
            if (!pcm_size)
                break;
        }

        ssize_t nframes = MIN((ssize_t)pcm_size / FRAME_BYTES, frames_left);
        int16_t *dst = &frames[2 * (period_size - frames_left)];

        /* Volume is applied by the copy; the visualizer wants the source before
         * it, so the spectrum does not shrink with the volume. */
        pcm_copy_buffer(dst, pcm_data, nframes * FRAME_BYTES);
        viz_pcm_push(pcm_data, nframes);

        pcm_data    += nframes * FRAME_BYTES;
        pcm_size    -= nframes * FRAME_BYTES;
        frames_left -= nframes;

        if (new_buffer)
        {
            new_buffer = false;
            pcm_play_dma_status_callback(PCM_DMAST_STARTED);
        }
    }

    sink_unlock();

    if (frames_left > 0)
        memset(&frames[2 * (period_size - frames_left)], 0,
               frames_left * FRAME_BYTES);
}

/* --- the writer thread (replaces upstream's SIGIO handler) ----------------- */

/* Returns false if the device died and was abandoned. */
static bool writer_step(bool *pending)
{
    snd_pcm_sframes_t space, n;
    int err;

    /* Bluetooth: hand the period to the child; its read paces us. */
    if (bt_pipe)
    {
        size_t want = period_size * FRAME_BYTES;
        struct pollfd pfd = { .fd = fileno(bt_pipe), .events = POLLOUT };

        if (!*pending)
        {
            copy_frames();
            *pending = true;
        }

        /* The pipe holds two periods, so writing blind would park us in the
         * kernel for a whole period.  Wait with a timeout instead, mirroring
         * snd_pcm_wait() below, so a route change or shutdown is never stuck
         * behind the child.  poll() also reports a dead reader before the
         * write does. */
        err = poll(&pfd, 1, WAIT_TIMEOUT_MS);
        if (err == 0)
            return true;                /* pipe full; re-check the flags */
        if (err < 0)
        {
            if (errno == EINTR)
                return true;
            dev_lost(strerror(errno));
            return false;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            dev_lost("bluetooth helper gone");
            return false;
        }

        if (fwrite(frames, 1, want, bt_pipe) != want)
        {
            dev_lost("bluetooth helper gone");
            return false;
        }
        *pending = false;
        return true;
    }

    err = snd_pcm_wait(handle, WAIT_TIMEOUT_MS);
    if (err == 0)
        return true;                    /* timeout; re-check the flags */
    if (err < 0)
    {
        if (err == -EPIPE)
        {
            snd_pcm_recover(handle, err, 1);
            return true;
        }
        dev_lost(snd_strerror(err));
        return false;
    }

    space = snd_pcm_avail_update(handle);
    if (space < 0)
    {
        if (space == -EPIPE)
        {
            snd_pcm_recover(handle, space, 1);
            return true;
        }
        dev_lost(snd_strerror(space));
        return false;
    }

    /* Top the device up as far as it will take rather than handing over one
     * period per wakeup, so a scheduling hiccup is made up immediately. */
    while (writer_run && space >= (snd_pcm_sframes_t)period_size)
    {
        if (!*pending)
        {
            copy_frames();
            *pending = true;            /* a filled period is never dropped */
        }

        n = snd_pcm_writei(handle, frames, period_size);
        if (n >= 0)
        {
            *pending = false;
            space -= n;
            continue;
        }
        if (n == -EAGAIN)
            break;                      /* no room after all; go re-wait */
        if (n == -EPIPE || n == -ESTRPIPE)
        {
            snd_pcm_recover(handle, n, 1);
            break;
        }
        dev_lost(snd_strerror(n));
        return false;
    }

    if (snd_pcm_state(handle) == SND_PCM_STATE_PREPARED)
        snd_pcm_start(handle);

    return true;
}

static int writer_fn(void *param)
{
    bool pending = false;
    Uint32 retry_at = 0, route_at = 0;

    (void)param;

    /* The priority SDL gave its audio thread. */
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);

    /* So a crash can be attributed to this thread or to one inside a plugin. */
    fprintf(stderr, "pcm: writer thread tid %ld\n", (long)syscall(SYS_gettid));

    while (writer_run)
    {
        /* Poll the route whether or not a device is open: with the device
         * closed this is the only signal that audiomon has repointed
         * `default`, and therefore that reopening is worth trying. */
        if (SDL_TICKS_PASSED(SDL_GetTicks(), route_at))
        {
            int route = trimpod_audio_sink();
            route_at = SDL_GetTicks() + ROUTE_POLL_MS;
            if (route != cur_route)
            {
                fprintf(stderr, "pcm: route %d -> %d\n", cur_route, route);
                cur_route = route;
                /* Either notice can arrive first -- this poll, or the write
                 * error from a device that has gone -- so leaving a private
                 * route this way is a loss too. */
                if (dev_is_open() && open_route != TRIMPOD_SINK_SPEAKER)
                    dev_lost("route changed");
                else
                    dev_close();
            }
        }

        if (dev_is_open() && req_sample_rate &&
            req_sample_rate != last_sample_rate)
        {
            logf("pcm: rate %u -> %u", last_sample_rate, req_sample_rate);
            dev_close();
        }

        if (!dev_is_open())
        {
            if (SDL_TICKS_PASSED(SDL_GetTicks(), retry_at))
            {
                if (req_sample_rate)
                    last_sample_rate = req_sample_rate;
                if (!dev_open())
                    retry_at = SDL_GetTicks() + REOPEN_DELAY_MS;
            }
            SDL_Delay(20);
            continue;
        }

        if (!writer_step(&pending))
            retry_at = SDL_GetTicks() + REOPEN_DELAY_MS;
    }

    dev_close();
    return 0;
}

/* --- Trimpod visualizer PCM tap -----------------------------------------
 * The spectrum and Milkdrop visualizer (projectM) need the audio waveform to
 * react to the beat.  We mirror the PRE-volume Rockbox S16 stereo source (see
 * copy_frames) into a lock-free ring buffer (single producer = the writer
 * thread, single consumer = the render loop) so the visuals are independent of
 * the volume setting.  Torn reads are harmless for a visualizer. */
#define VIZ_PCM_FRAMES 8192            /* must be a power of two */
static int16_t viz_pcm_ring[VIZ_PCM_FRAMES * 2];
static volatile unsigned viz_pcm_w;    /* total stereo frames ever written */

static void viz_pcm_push(const void *src_v, unsigned frames_n)
{
    const int16_t *src = src_v;
    unsigned w = viz_pcm_w;

    for (unsigned i = 0; i < frames_n; i++)
    {
        unsigned idx = (w & (VIZ_PCM_FRAMES - 1)) * 2;
        viz_pcm_ring[idx]     = src[2 * i];
        viz_pcm_ring[idx + 1] = src[2 * i + 1];
        w++;
    }
    viz_pcm_w = w;
}

/* Copy the most recent up-to max_frames stereo frames into out (LRLR).
 * Returns the number of frames written. Called by trimpod_visualizer.c. */
unsigned pcm_viz_latest(int16_t *out, unsigned max_frames)
{
    unsigned w = viz_pcm_w;
    unsigned n = max_frames < VIZ_PCM_FRAMES ? max_frames : VIZ_PCM_FRAMES;
    if (n > w)
        n = w;
    unsigned start = w - n;
    for (unsigned i = 0; i < n; i++)
    {
        unsigned idx = ((start + i) & (VIZ_PCM_FRAMES - 1)) * 2;
        out[2 * i]     = viz_pcm_ring[idx];
        out[2 * i + 1] = viz_pcm_ring[idx + 1];
    }
    return n;
}

/* --- pcm_sink ops ---------------------------------------------------------- */

/* Publish the rate; the writer reopens at it.  Closing the device from this
 * thread would pull it out from under a writer sitting in snd_pcm_wait(). */
static void sink_set_freq(uint16_t freq)
{
    req_sample_rate = hw_freq_sampr[freq];
}

/* No locking here.  The pcm layer holds it when calling from a Rockbox thread,
 * and calls these re-entrantly from inside copy_frames() on the writer thread,
 * where it is already held. */
static void sink_dma_start(const void *addr, size_t size)
{
    pcm_data = addr;
    pcm_size = size;
    dev_playing = true;
}

/* Upstream snd_pcm_drain()s here.  We must not: this is reached from inside
 * copy_frames() on the writer thread, so draining would block the very thread
 * that empties the buffer. */
static void sink_dma_stop(void)
{
    dev_playing = false;
    pcm_size = 0;
}

static void sink_dma_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&pcm_mtx, &attr);

    /* The Bluetooth helper dying mid-write must surface as an fwrite error,
     * not as a signal that takes the app down with it. */
    signal(SIGPIPE, SIG_IGN);

    last_sample_rate = hw_freq_sampr[HW_FREQ_DEFAULT];
    cur_route = trimpod_audio_sink();

    writer_run = true;
    writer_thread = SDL_CreateThread(writer_fn, "trimpod-pcm", NULL);

    if (!writer_thread)
        panicf("Could not create audio thread");
}

static void sink_dma_postinit(void)
{
}

/* Called from audiohw_close() on the way out.  Every alsa-lib call the writer
 * makes is bounded, so this returns promptly -- which is the point: the SDL
 * path could not be closed at all and hung the "Shutting Down" screen. */
void pcm_alsa_shutdown(void)
{
    if (!writer_thread)
        return;

    writer_run = false;
    SDL_WaitThread(writer_thread, NULL);
    writer_thread = NULL;
}

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
    },
    .ops = {
        .init     = sink_dma_init,
        .postinit = sink_dma_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_dma_start,
        .stop     = sink_dma_stop,
    },
};
