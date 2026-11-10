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
#ifndef _TRIMPOD_ALSA_H
#define _TRIMPOD_ALSA_H

/* Shared between the ALSA volume driver (drivers/audio/trimpod-alsa.c) and the
 * PCM sink (target/hosted/sdl/pcm-alsa.c): both need to know where NextUI is
 * currently sending audio, and both talk to alsa-lib from different threads. */

/* NextUI's AUDIO_SINK_* values (workspace/tg5040/libmsettings/msettings.h). */
#define TRIMPOD_SINK_SPEAKER 0
#define TRIMPOD_SINK_BT      1
#define TRIMPOD_SINK_USB     2

/* Active sink as published by NextUI's audiomon; TRIMPOD_SINK_SPEAKER when
 * NextUI is not running. */
int trimpod_audio_sink(void);

/* Card index of the USB audio device, or -1 if there is none. */
int trimpod_usb_card(void);

/* One lock over every alsa-lib entry point in the app.  snd_config_update_free_
 * global() is explicitly not thread-safe and rebuilds the global config tree
 * under any thread that opens a handle, so the volume driver's mixer opens and
 * the PCM sink's device opens must never overlap with it or each other. */
void trimpod_alsa_lock(void);
void trimpod_alsa_unlock(void);

/* Drop the cached mixer control for the off-codec sinks, for use when the
 * device behind one has gone.  The next volume change re-probes; a Bluetooth
 * speaker or a USB DAC that comes back is not the same control. */
void trimpod_alsa_forget_sink(void);

/* The output moved and audiomon set NextUI's volume on it: the next volume
 * set re-applies Trimpod's to every stage, speaker baseline included. */
void trimpod_alsa_route_changed(void);

/* Stop the PCM writer thread and release the device.  Called from
 * audiohw_close() during shutdown. */
void pcm_alsa_shutdown(void);

/* How long audio handed to the output now takes to be heard, as last
 * measured; the video player's sound is timed against it. */
unsigned int pcm_alsa_output_delay_us(void);

#endif /* _TRIMPOD_ALSA_H */
