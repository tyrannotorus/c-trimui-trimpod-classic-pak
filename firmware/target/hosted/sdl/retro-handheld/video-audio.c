/***************************************************************************
 * Trimpod: the video player's sound, played on Trimpod's own output.
 *
 * The plugin preloaded into the player (lib/tplayer_plugins/alsa_trimpod.c)
 * writes its samples into the ring in tpva.h.  This end plays the ring on a
 * mixer channel, so the sound follows every route change as music does, and
 * publishes how long a sample takes from the ring to the ear, which the plugin
 * adds to the delay the player times its pictures against.
 ****************************************************************************/
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "pcm_mixer.h"
#include "trimpod_alsa.h"
#include "tpva.h"
#include "video-audio.h"

/* A chunk handed to the mixer; also the granularity of the latency. */
#define CHUNK_FRAMES 512

static struct tpva_ring *ring;
static int wake_fd = -1;
static unsigned int out_rate;
static int16_t chunk[CHUNK_FRAMES * TPVA_CHANNELS];

/* The stream's rate is the player's own; this converts it to the mixer's by
 * linear interpolation (an exact copy when they match).  pos is the next
 * output frame's place in the input, in 1/2^32 frames past read_pos. */
static uint64_t pos;

static uint64_t load(const uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static int16_t sample_at(uint64_t frame, int ch)
{
    return ring->data[(frame % TPVA_FRAMES) * TPVA_CHANNELS + ch];
}

static size_t take(unsigned int in_rate)
{
    uint64_t rd = ring->read_pos, fl = load(&ring->flush_pos);
    uint64_t avail, step = ((uint64_t)in_rate << 32) / out_rate;
    size_t n = 0;

    if (rd < fl)                        /* a seek dropped what was waiting */
    {
        rd = fl;
        pos = 0;
    }
    avail = load(&ring->write_pos) - rd;

    /* Each output frame needs the input frame at pos and the one after it. */
    while (n < CHUNK_FRAMES && (pos >> 32) + 1 < avail)
    {
        uint64_t at = rd + (pos >> 32);
        int32_t frac = (int32_t)((pos & 0xffffffffu) >> 16);   /* Q16 */

        for (int ch = 0; ch < TPVA_CHANNELS; ch++)
        {
            int32_t a = sample_at(at, ch), b = sample_at(at + 1, ch);

            chunk[n * TPVA_CHANNELS + ch] = (int16_t)(a + (((int64_t)(b - a) * frac) >> 16));
        }
        pos += step;
        n++;
    }
    __atomic_store_n(&ring->read_pos, rd + (pos >> 32), __ATOMIC_RELEASE);
    pos &= 0xffffffffu;
    return n;
}

/* The mixer's callback, mostly on the PCM writer's thread: nothing here may
 * block. */
static void get_more(const void **start, size_t *size)
{
    unsigned int in_rate = __atomic_load_n(&ring->rate, __ATOMIC_ACQUIRE);
    size_t n = 0;

    if (in_rate && __atomic_load_n(&ring->running, __ATOMIC_ACQUIRE))
        n = take(in_rate);
    if (!n)
    {
        n = CHUNK_FRAMES;               /* silence keeps the output open */
        memset(chunk, 0, sizeof chunk);
    }
    __atomic_store_n(&ring->latency_us, pcm_alsa_output_delay_us() +
                     (uint32_t)((uint64_t)(MIX_FRAME_SAMPLES + n) * 1000000 / out_rate),
                     __ATOMIC_RELAXED);
    ssize_t woke = write(wake_fd, "", 1);  /* full: earlier wake-ups unread */
    (void)woke;
    *start = chunk;
    *size = n * TPVA_FRAME_BYTES;
}

static const struct mixer_play_cbs play_cbs = { .get_more = get_more };

bool video_audio_open(void)
{
    int fd;

    unlink(TPVA_WAKE);
    if (mkfifo(TPVA_WAKE, 0600) < 0)
        return false;
    wake_fd = open(TPVA_WAKE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    fd = open(TPVA_SHM, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (wake_fd < 0 || fd < 0 || ftruncate(fd, sizeof *ring) < 0)
    {
        if (fd >= 0)
            close(fd);
        video_audio_close();
        return false;
    }
    ring = mmap(NULL, sizeof *ring, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ring == MAP_FAILED)
    {
        ring = NULL;
        video_audio_close();
        return false;
    }
    ring->version = TPVA_VERSION;
    __atomic_store_n(&ring->magic, TPVA_MAGIC, __ATOMIC_RELEASE);  /* last: it's ready */

    pos = 0;
    out_rate = mixer_get_frequency();
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_VIDEO, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_VIDEO, &play_cbs, NULL, 0);
    return true;
}

void video_audio_close(void)
{
    if (ring)
    {
        mixer_channel_stop(PCM_MIXER_CHAN_VIDEO);   /* waits out get_more */
        munmap(ring, sizeof *ring);
        ring = NULL;
    }
    if (wake_fd >= 0)
    {
        close(wake_fd);
        wake_fd = -1;
    }
    unlink(TPVA_SHM);
    unlink(TPVA_WAKE);
}
