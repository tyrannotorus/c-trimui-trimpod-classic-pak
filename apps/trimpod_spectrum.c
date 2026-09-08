/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Trimpod: audio spectrum bar graph for the Now Playing screen.
 *
 * Computes per-band magnitudes with Goertzel filters over the live PCM tap
 * (pcm-alsa.c: pcm_viz_latest) -- no FFT library or allocation needed -- and
 * draws bottom-anchored bars filling the viewport. Rendered in place of the
 * volume bar via the WPS peak-meter element (see skin_display.c / the theme's
 * %pm tag), which already drives a fast partial refresh.
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
#include "trimpod_spectrum.h"

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "screen_access.h"
#include "lcd.h"
#include "viewport.h"
#include "audio.h"        /* paused -> silence */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Audio tap implemented in firmware/target/hosted/sdl/pcm-alsa.c */
extern unsigned pcm_viz_latest(int16_t *out, unsigned max_frames);

#define SPEC_N       512        /* samples analyzed per frame */
#define SPEC_BARS    16
#define SPEC_RATE    44100.0f   /* assumed output rate; only affects band tuning */
#define SPEC_FMIN    60.0f
#define SPEC_FMAX    14000.0f
/* Music is bass-heavy and a single Goertzel bin captures less of each wide
 * upper log-band, so highs barely move without compensation.  Boost amplitude
 * by this many dB per decade of frequency (pink noise falls ~10 dB/decade per
 * bin, so >10 also flattens that). Raise for livelier highs, lower if they peg. */
#define SPEC_TILT_DB 12.0f

static bool  spec_inited;
static float spec_window[SPEC_N];      /* Hann window */
static float spec_coeff[SPEC_BARS];    /* Goertzel coefficient per band */
static float spec_gain[SPEC_BARS];     /* per-band high-freq tilt (amplitude) */
static float spec_level[SPEC_BARS];    /* smoothed bar levels (0..1) */

static void spec_init(void)
{
    for (int n = 0; n < SPEC_N; n++)
        spec_window[n] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / (SPEC_N - 1));

    for (int b = 0; b < SPEC_BARS; b++)
    {
        /* geometric (log) spacing of band centre frequencies */
        float f = SPEC_FMIN * powf(SPEC_FMAX / SPEC_FMIN,
                                   (float)b / (SPEC_BARS - 1));
        float w = 2.0f * (float)M_PI * (f / SPEC_RATE);
        spec_coeff[b] = 2.0f * cosf(w);

        float decades = log10f(f / SPEC_FMIN);
        spec_gain[b] = powf(10.0f, (SPEC_TILT_DB * decades) / 20.0f);
    }
    spec_inited = true;
}

void trimpod_spectrum_draw(struct screen *display, struct viewport *vp)
{
    int16_t pcm[SPEC_N * 2];
    float    x[SPEC_N];

    if (!spec_inited)
        spec_init();

    /* paused: the tap still holds the last audio; feed silence so the bars
     * decay instead of freezing */
    unsigned got = (audio_status() & AUDIO_STATUS_PAUSE) ? 0
                                                        : pcm_viz_latest(pcm, SPEC_N);

    /* mono mix, normalise to ~[-1,1], apply window */
    for (int n = 0; n < SPEC_N; n++)
    {
        float s = 0.0f;
        if ((unsigned)n < got)
            s = (pcm[2 * n] + pcm[2 * n + 1]) * (0.5f / 32768.0f);
        x[n] = s * spec_window[n];
    }

    int W = vp->width;
    int H = vp->height;
    int gap = (W >= 2 * SPEC_BARS) ? 1 : 0;
    int bw = (W - (SPEC_BARS - 1) * gap) / SPEC_BARS;
    if (bw < 1)
        bw = 1;

    display->set_drawmode(DRMODE_SOLID);
    display->clear_viewport();

    for (int b = 0; b < SPEC_BARS; b++)
    {
        /* Goertzel magnitude at this band's centre frequency */
        float coeff = spec_coeff[b];
        float s1 = 0.0f, s2 = 0.0f;
        for (int n = 0; n < SPEC_N; n++)
        {
            float s0 = x[n] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        float mag = (power > 0.0f) ? sqrtf(power) : 0.0f;
        mag /= (SPEC_N * 0.25f);
        mag *= spec_gain[b];        /* tilt up the highs (see SPEC_TILT_DB) */

        /* compress to a 0..1 display level (log-ish), then smooth with fast
         * attack / slow decay so the bars look lively but not jittery */
        float v = log10f(1.0f + mag * 40.0f);
        if (v > 1.0f) v = 1.0f;
        if (v < 0.0f) v = 0.0f;

        if (v >= spec_level[b])
            spec_level[b] = v;                              /* attack */
        else
            spec_level[b] = spec_level[b] * 0.72f + v * 0.28f; /* decay */

        int bh = (int)(spec_level[b] * (H - 1) + 0.5f);
        if (bh < 1 && spec_level[b] > 0.02f)
            bh = 1;

        if (bh > 0)
        {
            int bx = b * (bw + gap);
            display->fillrect(bx, H - bh, bw, bh);   /* bottom-anchored */
        }
    }
}
