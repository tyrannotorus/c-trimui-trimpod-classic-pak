/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2011 by Michael Sevakis
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

#ifndef PCM_MIXER_H
#define PCM_MIXER_H

#include <sys/types.h>
#include "pcm.h"
#include "pcm_sink.h"

/** Simple config **/

/* Length of PCM frames (always) */
/* Hosted targets need larger buffers for decent performance due to
   OS locking/scheduling overhead */
#define MIX_FRAME_SAMPLES 1024

/* Otherwise can't DMA from IRAM, IRAM is pointless or worse */
#define DOWNMIX_BUF_IBSS

#define MIXER_CALLBACK_ICODE


/** Definitions **/

/* Channels are preassigned for simplicity */
enum pcm_mixer_channel
{
    PCM_MIXER_CHAN_PLAYBACK = 0,
    PCM_MIXER_CHAN_VOICE,
    PCM_MIXER_CHAN_BEEP,
    PCM_MIXER_CHAN_VIDEO,   /* Trimpod: the video player's sound (video-audio.c) */
    /* Add new channel indexes above this line */
    PCM_MIXER_NUM_CHANNELS,
};

/* Channel playback states */
enum channel_status
{
    CHANNEL_STOPPED = 0,
    CHANNEL_PLAYING,
    CHANNEL_PAUSED,
};

#define MIX_AMP_UNITY    0x00010000
#define MIX_AMP_MUTE     0x00000000


/** Public interfaces **/

/* Start playback on a channel */
struct mixer_play_cbs {
    void (*get_more)(const void **start, size_t *size);
    void (*sampr_changed)(uint32_t sampr);
};

void mixer_channel_play_data(enum pcm_mixer_channel channel,
                             const struct mixer_play_cbs* cbs,
                             const void *start, size_t size);

/* Pause or resume a channel (when started) */
void mixer_channel_play_pause(enum pcm_mixer_channel channel, bool play);

/* Stop playback on a channel */
void mixer_channel_stop(enum pcm_mixer_channel channel);

/* Switch playback sink */
bool mixer_switch_sink(enum pcm_sink_ids sink);

/* Set channel's amplitude factor */
void mixer_channel_set_amplitude(enum pcm_mixer_channel channel,
                                 unsigned int amplitude);

/* Return channel's playback status */
enum channel_status mixer_channel_status(enum pcm_mixer_channel channel);

/* Returns amount data remaining in channel before next callback */
size_t mixer_channel_get_bytes_waiting(enum pcm_mixer_channel channel);

/* Return pointer to channel's playing audio data and the size remaining */
const void * mixer_channel_get_buffer(enum pcm_mixer_channel channel,
                                      int *count);

/* Calculate peak values for channel */
void mixer_channel_calculate_peaks(enum pcm_mixer_channel channel,
                                   struct pcm_peaks *peaks);

/* Adjust channel pointer by a given offset to support movable buffers */
void mixer_adjust_channel_address(enum pcm_mixer_channel channel,
                                  off_t offset);

struct mixer_buffer_cbs {
    /* Called for each buffer, not each mixer chunk */
    void (*next_buffer)(const void *start, size_t size);
    void (*sampr_changed)(uint32_t sampr);
};

/* Set a hook that is called upon getting a new source buffer for a channel */
void mixer_channel_set_buffer_hook(enum pcm_mixer_channel channel,
                                   const struct mixer_buffer_cbs* cbs);

/* Stop ALL channels and PCM and reset state */
void mixer_reset(void);

/* Set output samplerate */
void mixer_set_frequency(unsigned int samplerate);

/* Get output samplerate */
unsigned int mixer_get_frequency(void);

#endif /* PCM_MIXER_H */
