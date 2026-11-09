/* AC-3 audio for TrimUI's stock video player (tplayerslave).
 *
 * The player's libadecoder loads "libpassthrua.so" for AC-3, which the firmware
 * doesn't ship.  Trimpod puts this one on the player's LD_LIBRARY_PATH.  The
 * structures below name only the fields used here. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "config-a52.h"
#include "a52.h"

#define AC3_MAX_FRAME_BYTES 3840
#define AC3_FRAME_SAMPLES   (6 * 256)
#define AC3_HEADER_BYTES    7
#define OUTPUT_CHANNELS     2
#define OUTPUT_BITS         16

/* Unity gain lands on 28 bits of precision, as in the Rockbox a52 codec. */
#define A52_LEVEL           (1 << 24)
#define SAMPLE_SHIFT        (28 - (OUTPUT_BITS - 1))

/* decode() results libadecoder acts on */
#define DEC_OK              0
#define DEC_NEED_DATA       5    /* the reader won't wait: retry later */
#define DEC_NO_DATA         (-2)

struct file_read
{
    char pad[80];
    int (*show)(void *buf, int size, struct file_read *fr);   /* peek */
    int (*skip)(int size, struct file_read *fr);              /* consume */
};

struct stream_info
{
    int pad0;
    int time_ms;       /* +4 */
    int sample_rate;   /* +8 */
    int pad12;
    int channels;      /* +16 */
    int pad20[2];
    int samples;       /* +28: per channel, since the last seek */
    int pad32[2];
    int bitrate_kbps;  /* +40 */
    int pad44[3];
    int output_bits;   /* +56 */
};
_Static_assert(offsetof(struct stream_info, output_bits) == 56, "stream_info");

/* Offsets into libadecoder's command block */
#define CMD_PRIVATE_0    0
#define CMD_PRIVATE_1    8
#define CMD_OUTPUT_BITS  72
#define CMD_NO_WAIT      92

struct audio_decoder
{
    struct file_read *fr;      /* +0: libadecoder sets these three after init */
    struct stream_info *info;  /* +8 */
    char *cmd;                 /* +16 */
    long pad24;
    int (*init)(struct audio_decoder *);                     /* +32 */
    int (*decode)(struct audio_decoder *, void *, int *);    /* +40 */
    int (*exit)(struct audio_decoder *);                     /* +48 */
    int (*prepare)(struct audio_decoder *, int, int, void *);/* +56: AC-3 needs it */
};
_Static_assert(sizeof(struct audio_decoder) == 0x40, "audio_decoder");

static a52_state_t *state;
static uint8_t frame[AC3_MAX_FRAME_BYTES];

static int starved(const struct audio_decoder *d)
{
    return d->cmd[CMD_NO_WAIT] == 1 ? DEC_NEED_DATA : DEC_NO_DATA;
}

static int16_t to_s16(sample_t sample)
{
    int32_t s = sample >> SAMPLE_SHIFT;

    if (s > INT16_MAX)
        return INT16_MAX;
    if (s < INT16_MIN)
        return INT16_MIN;
    return (int16_t)s;
}

static int ac3_decode(struct audio_decoder *d, void *out, int *out_bytes)
{
    struct file_read *fr = d->fr;
    struct stream_info *info = d->info;
    int16_t *pcm = out;
    int flags, rate, bitrate, length;
    level_t level = A52_LEVEL;

    *out_bytes = 0;

    for (;;)
    {
        if (fr->show(frame, AC3_HEADER_BYTES, fr) < AC3_HEADER_BYTES)
            return starved(d);
        length = a52_syncinfo(frame, &flags, &rate, &bitrate);
        if (length)
            break;
        fr->skip(1, fr);                /* not on a frame boundary: resync */
    }
    if (fr->show(frame, length, fr) < length)
        return starved(d);
    fr->skip(length, fr);

    flags = A52_STEREO | A52_ADJUST_LEVEL;
    if (a52_frame(state, frame, &flags, &level, 0))
        return DEC_OK;                  /* damaged frame: output nothing */
    a52_dynrng(state, NULL, NULL);

    for (int block = 0; block < 6; block++)
    {
        const sample_t *samples;

        if (a52_block(state))
            return DEC_OK;
        samples = a52_samples(state);
        for (int i = 0; i < 256; i++)
        {
            *pcm++ = to_s16(samples[i]);
            *pcm++ = to_s16(samples[256 + i]);
        }
    }

    info->sample_rate = rate;
    info->channels = OUTPUT_CHANNELS;
    info->output_bits = OUTPUT_BITS;
    info->bitrate_kbps = bitrate / 1000;
    info->samples += AC3_FRAME_SAMPLES;
    info->time_ms = (int)((int64_t)info->samples * 1000 / rate);
    *out_bytes = AC3_FRAME_SAMPLES * OUTPUT_CHANNELS * (OUTPUT_BITS / 8);
    return DEC_OK;
}

static int ac3_init(struct audio_decoder *d)
{
    state = a52_init(0);
    if (!state)
        return -1;
    *(void **)(d->cmd + CMD_PRIVATE_0) = d;
    *(void **)(d->cmd + CMD_PRIVATE_1) = d;
    *(uint16_t *)(d->cmd + CMD_OUTPUT_BITS) = OUTPUT_BITS;
    return 0;
}

static int ac3_exit(struct audio_decoder *d)
{
    (void)d;
    return 0;
}

static int ac3_prepare(struct audio_decoder *d, int a, int b, void *c)
{
    (void)d; (void)a; (void)b; (void)c;
    return 0;
}

struct audio_decoder *passthruaDecInit(void)
{
    struct audio_decoder *d = calloc(1, sizeof *d);

    if (d)
    {
        d->init = ac3_init;
        d->decode = ac3_decode;
        d->exit = ac3_exit;
        d->prepare = ac3_prepare;
    }
    return d;
}

int passthruaDecExit(struct audio_decoder *d)
{
    free(d);
    return 0;
}
