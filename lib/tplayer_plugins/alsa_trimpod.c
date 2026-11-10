/***************************************************************************
 * Trimpod: an ALSA output for TrimUI's video player that feeds Trimpod.
 *
 * Preloaded into the player (video-target.c).  The player's samples go into
 * the ring in tpva.h, and Trimpod plays them.  The delay reported is the
 * ring's fill plus Trimpod's own latency, so the player times its pictures
 * against what is actually being heard.
 ****************************************************************************/
#define _GNU_SOURCE                 /* RTLD_NEXT, dladdr */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include "tpva.h"

#define MIN_PERIOD_FRAMES 256
#define MIN_RATE          8000
#define MAX_RATE          96000     /* the player's own: Trimpod converts */
#define DRAIN_POLL_US     10000
#define DRAIN_POLLS       100       /* a second */

struct tp
{
    snd_pcm_ioplug_t io;
    struct tpva_ring *ring;
    int wake_fd;
    uint64_t base;                  /* write_pos at prepare: this stream's frame 0 */
};

static uint64_t load(const uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store(uint64_t *p, uint64_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void set_running(struct tp *tp, int running)
{
    __atomic_store_n(&tp->ring->running, running, __ATOMIC_RELEASE);
}

/* Trimpod skips what a seek dropped, so that counts as taken. */
static uint64_t taken(const struct tp *tp)
{
    uint64_t rd = load(&tp->ring->read_pos), fl = load(&tp->ring->flush_pos);

    return rd > fl ? rd : fl;
}

/* NULL writes silence. */
static void ring_write(struct tp *tp, const int16_t *src, snd_pcm_uframes_t frames)
{
    struct tpva_ring *r = tp->ring;
    uint64_t wr = r->write_pos;

    while (frames)
    {
        size_t at = wr % TPVA_FRAMES, n = TPVA_FRAMES - at;

        if (n > frames)
            n = frames;
        if (src)
        {
            memcpy(&r->data[at * TPVA_CHANNELS], src, n * TPVA_FRAME_BYTES);
            src += n * TPVA_CHANNELS;
        }
        else
            memset(&r->data[at * TPVA_CHANNELS], 0, n * TPVA_FRAME_BYTES);
        wr += n;
        frames -= n;
    }
    store(&r->write_pos, wr);
}

static int tp_start(snd_pcm_ioplug_t *io)
{
    set_running(io->private_data, 1);
    return 0;
}

/* Stopped means dropped: what's still in the ring must not play later. */
static int tp_stop(snd_pcm_ioplug_t *io)
{
    struct tp *tp = io->private_data;

    set_running(tp, 0);
    store(&tp->ring->flush_pos, tp->ring->write_pos);
    return 0;
}

static int tp_prepare(snd_pcm_ioplug_t *io)
{
    struct tp *tp = io->private_data;

    tp_stop(io);
    tp->base = tp->ring->write_pos;
    __atomic_store_n(&tp->ring->rate, io->rate, __ATOMIC_RELEASE);
    return 0;
}

static int tp_pause(snd_pcm_ioplug_t *io, int enable)
{
    set_running(io->private_data, !enable);
    return 0;
}

static snd_pcm_sframes_t tp_pointer(snd_pcm_ioplug_t *io)
{
    struct tp *tp = io->private_data;
    uint64_t written = tp->ring->write_pos - tp->base;

    /* snd_pcm_forward() moves the stream on without data: that's silence. */
    if (io->appl_ptr > written && io->appl_ptr - written <= io->buffer_size)
        ring_write(tp, NULL, io->appl_ptr - written);
    return (taken(tp) - tp->base) % io->buffer_size;
}

static snd_pcm_sframes_t tp_transfer(snd_pcm_ioplug_t *io,
                                     const snd_pcm_channel_area_t *areas,
                                     snd_pcm_uframes_t offset,
                                     snd_pcm_uframes_t size)
{
    const char *src = (const char *)areas[0].addr +
                      (areas[0].first + areas[0].step * offset) / 8;

    ring_write(io->private_data, (const int16_t *)src, size);
    return size;
}

static int tp_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delay)
{
    struct tp *tp = io->private_data;
    uint64_t later = (uint64_t)__atomic_load_n(&tp->ring->latency_us, __ATOMIC_RELAXED) *
                     io->rate / 1000000;

    *delay = (snd_pcm_sframes_t)(tp->ring->write_pos - taken(tp) + later);
    return 0;
}

/* Trimpod writes a byte to the wake FIFO for each chunk it plays. */
static int tp_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
                           unsigned int nfds, unsigned short *revents)
{
    struct tp *tp = io->private_data;
    char buf[64];
    uint64_t fill;

    (void)pfd;
    (void)nfds;
    while (read(tp->wake_fd, buf, sizeof buf) > 0)
        ;
    fill = tp->ring->write_pos - taken(tp);
    *revents = io->buffer_size - fill >= io->period_size ? POLLOUT : 0;
    return 0;
}

static int tp_drain(snd_pcm_ioplug_t *io)
{
    struct tp *tp = io->private_data;

    for (int i = 0; i < DRAIN_POLLS && taken(tp) < tp->ring->write_pos; i++)
        usleep(DRAIN_POLL_US);
    return 0;
}

static int tp_close(snd_pcm_ioplug_t *io)
{
    struct tp *tp = io->private_data;

    set_running(tp, 0);
    munmap(tp->ring, sizeof *tp->ring);
    close(tp->wake_fd);
    free(tp);
    return 0;
}

static const snd_pcm_ioplug_callback_t tp_callback = {
    .start        = tp_start,
    .stop         = tp_stop,
    .pointer      = tp_pointer,
    .transfer     = tp_transfer,
    .close        = tp_close,
    .prepare      = tp_prepare,
    .drain        = tp_drain,
    .pause        = tp_pause,
    .poll_revents = tp_poll_revents,
    .delay        = tp_delay,
};

static int set_constraints(snd_pcm_ioplug_t *io)
{
    static const unsigned int access[] = { SND_PCM_ACCESS_RW_INTERLEAVED };
    static const unsigned int format[] = { SND_PCM_FORMAT_S16_LE };
    const unsigned int ring_bytes = TPVA_FRAMES * TPVA_FRAME_BYTES;
    int err;

    if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, 1, access)) < 0 ||
        (err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, 1, format)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
                                               TPVA_CHANNELS, TPVA_CHANNELS)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
                                               MIN_RATE, MAX_RATE)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
                                               2 * MIN_PERIOD_FRAMES * TPVA_FRAME_BYTES,
                                               ring_bytes)) < 0 ||
        (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
                                               MIN_PERIOD_FRAMES * TPVA_FRAME_BYTES,
                                               ring_bytes / 2)) < 0)
        return err;
    return snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 2,
                                           TPVA_FRAMES / MIN_PERIOD_FRAMES);
}

SND_PCM_PLUGIN_DEFINE_FUNC(trimpod)
{
    struct tp *tp;
    int fd, err = -ENODEV;

    (void)root;
    (void)conf;
    if (stream != SND_PCM_STREAM_PLAYBACK)
        return -EINVAL;
    tp = calloc(1, sizeof *tp);
    if (!tp)
        return -ENOMEM;
    tp->wake_fd = -1;

    /* Trimpod made the ring before starting the player. */
    fd = open(TPVA_SHM, O_RDWR | O_CLOEXEC);
    if (fd >= 0)
    {
        tp->ring = mmap(NULL, sizeof *tp->ring, PROT_READ | PROT_WRITE, MAP_SHARED,
                        fd, 0);
        close(fd);
    }
    if (fd < 0 || tp->ring == MAP_FAILED)
        goto fail_free;
    if (tp->ring->magic != TPVA_MAGIC || tp->ring->version != TPVA_VERSION)
        goto fail_unmap;
    tp->wake_fd = open(TPVA_WAKE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (tp->wake_fd < 0)
        goto fail_unmap;

    tp->io.version = SND_PCM_IOPLUG_VERSION;
    tp->io.name = "Trimpod";
    tp->io.callback = &tp_callback;
    tp->io.private_data = tp;
    tp->io.poll_fd = tp->wake_fd;
    tp->io.poll_events = POLLIN;
    tp->base = tp->ring->write_pos;
    store(&tp->ring->flush_pos, tp->base);

    err = snd_pcm_ioplug_create(&tp->io, name, stream, mode);
    if (err < 0)
        goto fail_close;
    err = set_constraints(&tp->io);
    if (err < 0)
    {
        snd_pcm_ioplug_delete(&tp->io);     /* runs tp_close */
        return err;
    }
    *pcmp = tp->io.pcm;
    return 0;

fail_close:
    close(tp->wake_fd);
fail_unmap:
    munmap(tp->ring, sizeof *tp->ring);
fail_free:
    free(tp);
    return err;
}

SND_PCM_PLUGIN_SYMBOL(trimpod);

/* The player opens "default", and this system's alsa-lib opens the codec for
 * that name whatever the configuration says.  So while Trimpod's ring exists,
 * the name is taken over here: a plug, for format and channels, over the type
 * above, defined in memory.  (Its rate converter rejects the player's fixed
 * period sizes, so the rate stays the player's.) */
#define TP_PCM      "trimpod_video"
#define TP_PCM_CONF "pcm_type.trimpod { lib \"%s\" }\n" \
                    "pcm." TP_PCM " { type plug slave.pcm { type trimpod } }\n"

int snd_pcm_open(snd_pcm_t **pcmp, const char *name, snd_pcm_stream_t stream,
                 int mode)
{
    static int (*real_open)(snd_pcm_t **, const char *, snd_pcm_stream_t, int);
    char text[512];
    snd_config_t *top;
    snd_input_t *in;
    Dl_info self;
    int err;

    if (!real_open)
        real_open = (int (*)(snd_pcm_t **, const char *, snd_pcm_stream_t, int))
                    dlsym(RTLD_NEXT, "snd_pcm_open");
    if (stream != SND_PCM_STREAM_PLAYBACK || strcmp(name, "default") ||
        access(TPVA_SHM, F_OK) || !dladdr((void *)snd_pcm_open, &self))
        return real_open(pcmp, name, stream, mode);

    snprintf(text, sizeof text, TP_PCM_CONF, self.dli_fname);
    if ((err = snd_config_update()) < 0 || (err = snd_config_copy(&top, snd_config)) < 0)
        return err;
    if ((err = snd_input_buffer_open(&in, text, -1)) == 0)
    {
        err = snd_config_load(top, in);
        snd_input_close(in);
    }
    if (err == 0)
        err = snd_pcm_open_lconf(pcmp, TP_PCM, stream, mode, top);
    snd_config_delete(top);
    return err;
}
