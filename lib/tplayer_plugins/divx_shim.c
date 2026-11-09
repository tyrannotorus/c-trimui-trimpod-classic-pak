/* DivX 4 and 5 video for TrimUI's stock video player (tplayerslave).
 *
 * The hardware MPEG-4 decoder handles these streams, but the firmware doesn't
 * register it for those formats.  Trimpod loads this library into the player with
 * LD_PRELOAD to add the registrations. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>

/* libvideoengine's format IDs */
#define VIDEO_FORMAT_DIVX4 0x108
#define VIDEO_FORMAT_DIVX5 0x109

typedef void *(*decoder_create_fn)(void *, void *, void *, void *);
typedef int (*decoder_register_fn)(int, const char *, decoder_create_fn, int);

static void *create_divx_decoder(void *a, void *b, void *c, void *d)
{
    static decoder_create_fn create;

    if (!create)
    {
        void *lib = dlopen("libawmpeg4normal.so", RTLD_NOW | RTLD_GLOBAL);

        if (lib)
            create = (decoder_create_fn)dlsym(lib, "CreateMpeg4NormalDecoder");
    }
    return create ? create(a, b, c, d) : NULL;
}

__attribute__((constructor))
static void register_divx_formats(void)
{
    void *engine = dlopen("libvideoengine.so", RTLD_NOW | RTLD_GLOBAL);
    decoder_register_fn reg;

    if (!engine)
        return;
    reg = (decoder_register_fn)dlsym(engine, "VDecoderRegister");
    if (!reg)
        return;
    reg(VIDEO_FORMAT_DIVX4, "divx4", create_divx_decoder, 0);
    reg(VIDEO_FORMAT_DIVX5, "divx5", create_divx_decoder, 0);
}
