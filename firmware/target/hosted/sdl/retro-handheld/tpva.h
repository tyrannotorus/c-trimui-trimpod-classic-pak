/***************************************************************************
 * Trimpod: the video player's sound, handed to Trimpod.
 *
 * libasound_module_pcm_trimpod.so (lib/tplayer_plugins/alsa_trimpod.c), preloaded
 * into the player, writes its samples into this ring, and video-audio.c plays
 * them on Trimpod's output, which follows every route change -- so a headphone
 * change never touches the player.  One producer (the plugin) and one consumer
 * (Trimpod); the positions count frames since Trimpod created the ring.
 ****************************************************************************/
#ifndef TPVA_H
#define TPVA_H

#include <stdint.h>

#define TPVA_SHM        "/dev/shm/trimpod-video-audio"
#define TPVA_WAKE       "/dev/shm/trimpod-video-audio.wake"  /* FIFO: a byte per chunk */
#define TPVA_MAGIC      0x41565054u                          /* "TPVA" */
#define TPVA_VERSION    1
#define TPVA_CHANNELS   2                                    /* S16_LE, interleaved */
#define TPVA_FRAME_BYTES (TPVA_CHANNELS * 2)
#define TPVA_FRAMES     16384    /* the most the player may buffer; it picks less */

struct tpva_ring
{
    uint32_t magic;
    uint32_t version;
    uint32_t rate;          /* plugin: the stream's rate, the player's own */
    uint32_t latency_us;    /* Trimpod: from leaving the ring to being heard */
    uint32_t running;       /* the plugin's stream is started and not paused */
    uint32_t pad;
    uint64_t write_pos;     /* plugin */
    uint64_t read_pos;      /* Trimpod */
    uint64_t flush_pos;     /* plugin: what's before it was dropped (a seek) */
    int16_t  data[TPVA_FRAMES * TPVA_CHANNELS];
};

#endif /* TPVA_H */
