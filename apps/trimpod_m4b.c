/* Trimpod: .m4b audiobook support.
 *
 * Two independent pieces, both keyed off the .m4b extension:
 *
 * 1. Per-file resume positions.  A small store (path -> elapsed ms + byte
 *    offset) so starting an audiobook always continues where it left off,
 *    independent of the single global resume point in .resume.cfg.  Persisted
 *    as ROCKBOX_DIR "/.m4b_positions" (tmp-write + rename, so the old file
 *    survives a mid-write power loss).  Positions are updated at the same
 *    moments the global resume point is (track change/finish/stop -- and app
 *    shutdown, which stops audio through that same path -- via
 *    playlist_update_resume_info), and an entry is dropped when its book
 *    plays to the end.  Paths are canonicalised
 *    with realpath() so the tmpfs virtual-folder farm and the real SD path
 *    name the same book.
 *
 * 2. Chapter extraction.  m4b chapters come as a QuickTime chapter track
 *    (audio trak -> tref/chap -> text trak; titles are the track's samples)
 *    or as a Nero chpl atom in moov/udta.  Both real-world books on hand use
 *    the QT form and only one carries chpl, so QT is primary and chpl the
 *    fallback.  The result fills the stock cuesheet struct, which buys the
 *    whole existing subtrack machinery: chapter skip and the skin tokens.  Parsed on the audio thread from audio_load_cuesheet(); the
 *    scratch context is static to spare that thread's small stack. */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp */
#include <stdio.h>
#include <inttypes.h>

#include "file.h"
#include "string-extra.h"
#include "rbpaths.h"       /* ROCKBOX_DIR */
#include "rbunicode.h"     /* utf16decode */
#include "metadata.h"      /* struct mp3entry */
#include "cuesheet.h"
#include "trimpod_m4b.h"

/* libc realpath(); Rockbox does not wrap it and root paths map 1:1 to the OS
 * (app_root_realpath() == "/") -- same precedent as symlink() in
 * trimpod_folders.c. */
extern char *realpath(const char *path, char *resolved);

bool trimpod_is_m4b(const char *path)
{
    const char *ext = strrchr(path, '.');
    return ext && !strcasecmp(ext, ".m4b");
}

/* ==== per-file resume positions ======================================== */

#define POS_FILE            ROCKBOX_DIR "/.m4b_positions"
#define POS_TMP             POS_FILE ".tmp"
#define POS_MAX             64
#define POS_DONE_WINDOW_MS  5000   /* stopping this close to the end = finished */

struct m4b_pos
{
    char path[MAX_PATH];
    unsigned long elapsed;   /* ms */
    unsigned long offset;    /* bytes; codec seek hint, as in .resume.cfg */
};

static struct m4b_pos positions[POS_MAX];
static int npos;

/* farm symlink or real path -> one canonical key */
static void pos_canon(const char *in, char *out, size_t outsz)
{
    char *rp = realpath(in, NULL);
    strmemccpy(out, rp ? rp : in, outsz);
    free(rp);
}

static int pos_find(const char *key)
{
    for (int i = 0; i < npos; i++)
        if (!strcmp(positions[i].path, key))
            return i;
    return -1;
}

static void pos_save_file(void)
{
    int fd = open(POS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    for (int i = 0; i < npos; i++)
        fdprintf(fd, "%lu %lu %s\n", positions[i].elapsed,
                 positions[i].offset, positions[i].path);
    close(fd);
    rename(POS_TMP, POS_FILE);
}

/* insert/update at the front (the file doubles as an LRU list) */
static void pos_upsert(const char *key, unsigned long elapsed,
                       unsigned long offset)
{
    struct m4b_pos e;
    int i = pos_find(key);
    if (i < 0)
    {
        memset(&e, 0, sizeof e);
        strmemccpy(e.path, key, sizeof e.path);
        if (npos < POS_MAX)
            npos++;
        i = npos - 1;             /* evict the oldest when full */
    }
    else
        e = positions[i];
    e.elapsed = elapsed;
    e.offset = offset;
    memmove(positions + 1, positions, i * sizeof positions[0]);
    positions[0] = e;
}

static bool pos_remove(const char *key)
{
    int i = pos_find(key);
    if (i < 0)
        return false;
    memmove(positions + i, positions + i + 1,
            (npos - i - 1) * sizeof positions[0]);
    npos--;
    return true;
}

void trimpod_m4b_resume_init(void)
{
    int fd = open(POS_FILE, O_RDONLY);
    if (fd < 0)
        return;
    off_t size = filesize(fd);
    if (size <= 0 || size > (off_t)(POS_MAX * sizeof(struct m4b_pos)))
    {
        close(fd);
        return;
    }
    char *buf = malloc(size + 1);
    if (buf && read(fd, buf, size) == (ssize_t)size)
    {
        buf[size] = '\0';
        char *line = buf;
        while (line && *line && npos < POS_MAX)
        {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl++ = '\0';
            char *sp1 = strchr(line, ' ');
            char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
            if (sp2 && sp2[1])
            {
                struct m4b_pos *e = &positions[npos];
                e->elapsed = strtoul(line, NULL, 10);
                e->offset = strtoul(sp1 + 1, NULL, 10);
                strmemccpy(e->path, sp2 + 1, sizeof e->path);
                npos++;
            }
            line = nl;
        }
    }
    free(buf);
    close(fd);
}

bool trimpod_m4b_resume_get(const char *path,
                            unsigned long *elapsed, unsigned long *offset)
{
    if (!trimpod_is_m4b(path))
        return false;
    char key[MAX_PATH];
    pos_canon(path, key, sizeof key);
    int i = pos_find(key);
    if (i < 0)
        return false;
    *elapsed = positions[i].elapsed;
    *offset = positions[i].offset;
    return true;
}

void trimpod_m4b_resume_note(const struct mp3entry *id3)
{
    if (!id3 || !id3->path[0] || !trimpod_is_m4b(id3->path))
        return;
    char key[MAX_PATH];
    pos_canon(id3->path, key, sizeof key);

    bool dirty;
    if (id3->length > 0 && id3->elapsed + POS_DONE_WINDOW_MS >= id3->length)
        dirty = pos_remove(key);   /* finished: next start is from the top */
    else
    {
        pos_upsert(key, id3->elapsed, id3->offset);
        dirty = true;
    }
    if (dirty)
        pos_save_file();
}

/* ==== chapter extraction =============================================== */

#define M4B_ID(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

#define ID_moov M4B_ID('m','o','o','v')
#define ID_trak M4B_ID('t','r','a','k')
#define ID_mdia M4B_ID('m','d','i','a')
#define ID_minf M4B_ID('m','i','n','f')
#define ID_stbl M4B_ID('s','t','b','l')
#define ID_udta M4B_ID('u','d','t','a')
#define ID_tref M4B_ID('t','r','e','f')
#define ID_tkhd M4B_ID('t','k','h','d')
#define ID_mdhd M4B_ID('m','d','h','d')
#define ID_stts M4B_ID('s','t','t','s')
#define ID_stsc M4B_ID('s','t','s','c')
#define ID_stsz M4B_ID('s','t','s','z')
#define ID_stco M4B_ID('s','t','c','o')
#define ID_co64 M4B_ID('c','o','6','4')
#define ID_chap M4B_ID('c','h','a','p')
#define ID_chpl M4B_ID('c','h','p','l')

#define M4B_MAX_TRAKS 6
#define M4B_MAX_REFS  4

struct m4b_trak
{
    uint32_t id;
    uint32_t timescale;
    uint64_t stts_off, stsc_off, stsz_off, stco_off;   /* atom body offsets */
    bool co64;
};

/* Static: only touched by the audio thread (audio_load_cuesheet), whose stack
 * is small. */
static struct m4b_ctx
{
    struct m4b_trak traks[M4B_MAX_TRAKS];
    int ntraks;
    uint32_t chap_ids[M4B_MAX_REFS];
    int nrefs;
    uint64_t chpl_off;
    /* per-chapter scratch for the title samples */
    uint64_t sample_off[MAX_TRACKS];
    uint32_t sample_size[MAX_TRACKS];
    uint64_t chunk_off[MAX_TRACKS];
    uint32_t stsc_first[MAX_TRACKS], stsc_spc[MAX_TRACKS];
    unsigned char raw[512];
} ctx;

static bool rd(int fd, void *buf, size_t n)
{
    return read(fd, buf, n) == (ssize_t)n;
}

static bool rd_u16(int fd, uint16_t *v)
{
    unsigned char b[2];
    if (!rd(fd, b, 2))
        return false;
    *v = (b[0] << 8) | b[1];
    return true;
}

static bool rd_u32(int fd, uint32_t *v)
{
    unsigned char b[4];
    if (!rd(fd, b, 4))
        return false;
    *v = M4B_ID(b[0], b[1], b[2], b[3]);
    return true;
}

static bool rd_u64(int fd, uint64_t *v)
{
    uint32_t hi, lo;
    if (!rd_u32(fd, &hi) || !rd_u32(fd, &lo))
        return false;
    *v = ((uint64_t)hi << 32) | lo;
    return true;
}

/* Walk the atoms in [pos, end), recording what the chapter loaders need.
 * Tolerant: on anything malformed it just stops that level. */
static void m4b_walk(int fd, uint64_t pos, uint64_t end, int depth)
{
    struct m4b_trak *t =
        ctx.ntraks > 0 ? &ctx.traks[ctx.ntraks - 1] : NULL;

    while (pos + 8 <= end && depth < 6)
    {
        lseek(fd, pos, SEEK_SET);
        uint32_t sz32, type;
        if (!rd_u32(fd, &sz32) || !rd_u32(fd, &type))
            return;
        uint64_t hdr = 8, size = sz32;
        if (sz32 == 1)
        {
            if (!rd_u64(fd, &size))
                return;
            hdr = 16;
        }
        else if (sz32 == 0)
            size = end - pos;
        if (size < hdr || pos + size > end)
            return;
        uint64_t body = pos + hdr;

        switch (type)
        {
        case ID_moov: case ID_mdia: case ID_minf:
        case ID_stbl: case ID_udta: case ID_tref:
            m4b_walk(fd, body, pos + size, depth + 1);
            break;

        case ID_trak:
            if (ctx.ntraks < M4B_MAX_TRAKS)
            {
                ctx.ntraks++;
                m4b_walk(fd, body, pos + size, depth + 1);
            }
            break;

        case ID_tkhd:
            if (t)
            {
                unsigned char ver;
                if (rd(fd, &ver, 1))
                {
                    /* ver/flags(4) + ctime/mtime (4+4 or 8+8) -> track id */
                    lseek(fd, body + (ver ? 20 : 12), SEEK_SET);
                    rd_u32(fd, &t->id);
                }
            }
            break;

        case ID_mdhd:
            if (t)
            {
                unsigned char ver;
                if (rd(fd, &ver, 1))
                {
                    lseek(fd, body + (ver ? 20 : 12), SEEK_SET);
                    rd_u32(fd, &t->timescale);
                }
            }
            break;

        case ID_stts: if (t) t->stts_off = body; break;
        case ID_stsc: if (t) t->stsc_off = body; break;
        case ID_stsz: if (t) t->stsz_off = body; break;
        case ID_stco: if (t) { t->stco_off = body; t->co64 = false; } break;
        case ID_co64: if (t) { t->stco_off = body; t->co64 = true; } break;

        case ID_chap:   /* inside tref: the referenced chapter track ids */
            for (uint64_t p = body; p + 4 <= pos + size
                                    && ctx.nrefs < M4B_MAX_REFS; p += 4)
            {
                uint32_t ref;
                lseek(fd, p, SEEK_SET);
                if (!rd_u32(fd, &ref))
                    break;
                ctx.chap_ids[ctx.nrefs++] = ref;
            }
            break;

        case ID_chpl:
            ctx.chpl_off = body;
            break;
        }

        pos += size;
    }
}

static struct m4b_trak *find_trak(uint32_t id)
{
    for (int i = 0; i < ctx.ntraks; i++)
        if (ctx.traks[i].id == id)
            return &ctx.traks[i];
    return NULL;
}

/* Chaptering tools often leave XML-escaped text in titles ("Quinn&apos;s
 * God"); decode the five predefined entities in place. */
static void unescape_entities(char *s)
{
    static const struct { const char *ent; char ch; } tab[] = {
        { "&amp;", '&' }, { "&apos;", '\'' }, { "&quot;", '"' },
        { "&lt;", '<' }, { "&gt;", '>' },
    };
    char *w = s;
    while (*s)
    {
        char c = *s;
        if (c == '&')
            for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
            {
                size_t n = strlen(tab[i].ent);
                if (!strncmp(s, tab[i].ent, n))
                {
                    c = tab[i].ch;
                    s += n - 1;
                    break;
                }
            }
        *w++ = c;
        s++;
    }
    *w = '\0';
}

/* Decode one title sample (u16be byte count, then UTF-8 or BOM'd UTF-16). */
static void read_title(int fd, uint64_t off, uint32_t sample_size,
                       char *out, size_t outsz, int chapnum)
{
    uint16_t len = 0;
    out[0] = '\0';
    lseek(fd, off, SEEK_SET);
    if (rd_u16(fd, &len) && len > 0)
    {
        if (sample_size >= 2 && len > sample_size - 2)
            len = sample_size - 2;
        if (len > sizeof(ctx.raw))
            len = sizeof(ctx.raw);
        if (rd(fd, ctx.raw, len))
        {
            if (len >= 2 && ((ctx.raw[0] == 0xFE && ctx.raw[1] == 0xFF) ||
                             (ctx.raw[0] == 0xFF && ctx.raw[1] == 0xFE)))
            {
                bool le = (ctx.raw[0] == 0xFF);
                unsigned char *p = utf16decode(ctx.raw + 2,
                                               (unsigned char *)out,
                                               (len - 2) / 2, outsz - 1, le);
                *p = '\0';
            }
            else
            {
                size_t n = len < outsz - 1 ? len : outsz - 1;
                memcpy(out, ctx.raw, n);
                out[n] = '\0';
            }
            unescape_entities(out);
        }
    }
    if (!out[0])
        snprintf(out, outsz, "Chapter %d", chapnum);
}

/* QuickTime chapter track -> cue.  Returns the chapter count (0 = no good). */
static int load_qt_chapters(int fd, const struct m4b_trak *t,
                            struct cuesheet *cue)
{
    if (!t || !t->timescale || !t->stts_off || !t->stsc_off ||
        !t->stsz_off || !t->stco_off)
        return 0;

    /* chapter start times: running sum of the stts sample durations */
    uint32_t v, nent;
    lseek(fd, t->stts_off, SEEK_SET);
    if (!rd_u32(fd, &v) || !rd_u32(fd, &nent))
        return 0;
    int n = 0;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < nent && n < MAX_TRACKS; i++)
    {
        uint32_t cnt, delta;
        if (!rd_u32(fd, &cnt) || !rd_u32(fd, &delta))
            return 0;
        for (uint32_t j = 0; j < cnt && n < MAX_TRACKS; j++)
        {
            cue->tracks[n++].offset = acc * 1000 / t->timescale;
            acc += delta;
        }
    }
    if (n < 2)
        return 0;

    /* sample sizes */
    uint32_t uniform, cnt;
    lseek(fd, t->stsz_off, SEEK_SET);
    if (!rd_u32(fd, &v) || !rd_u32(fd, &uniform) || !rd_u32(fd, &cnt))
        return 0;
    if ((int)cnt < n)
        n = cnt;
    for (int i = 0; i < n; i++)
        if (uniform)
            ctx.sample_size[i] = uniform;
        else if (!rd_u32(fd, &ctx.sample_size[i]))
            return 0;

    /* chunk offsets */
    uint32_t nchunks;
    lseek(fd, t->stco_off, SEEK_SET);
    if (!rd_u32(fd, &v) || !rd_u32(fd, &nchunks))
        return 0;
    if (nchunks > MAX_TRACKS)
        nchunks = MAX_TRACKS;     /* n <= 99 samples never need more chunks */
    for (uint32_t i = 0; i < nchunks; i++)
    {
        if (t->co64)
        {
            if (!rd_u64(fd, &ctx.chunk_off[i]))
                return 0;
        }
        else
        {
            uint32_t o;
            if (!rd_u32(fd, &o))
                return 0;
            ctx.chunk_off[i] = o;
        }
    }

    /* sample-to-chunk map */
    uint32_t nstsc;
    lseek(fd, t->stsc_off, SEEK_SET);
    if (!rd_u32(fd, &v) || !rd_u32(fd, &nstsc))
        return 0;
    if (nstsc > MAX_TRACKS)
        nstsc = MAX_TRACKS;
    for (uint32_t i = 0; i < nstsc; i++)
    {
        uint32_t desc;
        if (!rd_u32(fd, &ctx.stsc_first[i]) ||
            !rd_u32(fd, &ctx.stsc_spc[i]) || !rd_u32(fd, &desc))
            return 0;
    }
    if (nstsc == 0)
        return 0;

    /* absolute file offset of each title sample */
    int sample = 0;
    uint32_t entry = 0;
    for (uint32_t c = 1; c <= nchunks && sample < n; c++)
    {
        while (entry + 1 < nstsc && c >= ctx.stsc_first[entry + 1])
            entry++;
        uint64_t off = ctx.chunk_off[c - 1];
        for (uint32_t s = 0; s < ctx.stsc_spc[entry] && sample < n; s++)
        {
            ctx.sample_off[sample] = off;
            off += ctx.sample_size[sample];
            sample++;
        }
    }
    n = sample;
    if (n < 2)
        return 0;

    for (int i = 0; i < n; i++)
        read_title(fd, ctx.sample_off[i], ctx.sample_size[i],
                   cue->tracks[i].title, sizeof cue->tracks[i].title, i + 1);
    return n;
}

/* Nero chpl atom -> cue (fallback).  Layout matches the reader in
 * lib/rbcodec/metadata/mp4.c: 8 bytes of version/flags+reserved, u8 count,
 * then per chapter a u64be timestamp in 100 ns units, u8 length, title. */
static int load_chpl_chapters(int fd, struct cuesheet *cue)
{
    unsigned char count, len;
    lseek(fd, ctx.chpl_off + 8, SEEK_SET);
    if (!rd(fd, &count, 1))
        return 0;
    int n = count < MAX_TRACKS ? count : MAX_TRACKS;
    for (int i = 0; i < n; i++)
    {
        uint64_t ts;
        if (!rd_u64(fd, &ts) || !rd(fd, &len, 1))
            return 0;
        cue->tracks[i].offset = ts / 10000;
        char *title = cue->tracks[i].title;
        size_t max = sizeof cue->tracks[i].title - 1;
        size_t want = len < max ? len : max;
        if (!rd(fd, title, want))
            return 0;
        title[want] = '\0';
        if (want < len)
            lseek(fd, len - want, SEEK_CUR);
        unescape_entities(title);
        if (!title[0])
            snprintf(title, max + 1, "Chapter %d", i + 1);
    }
    return n;
}

bool trimpod_m4b_load_chapters(const char *path, struct cuesheet *cue)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    memset(&ctx, 0, sizeof ctx);
    m4b_walk(fd, 0, filesize(fd), 0);

    memset(cue, 0, sizeof *cue);
    strmemccpy(cue->path, path, sizeof cue->path);
    strmemccpy(cue->file, path, sizeof cue->file);
    cue->curr_track = cue->tracks;

    int n = 0;
    for (int i = 0; i < ctx.nrefs && n < 2; i++)
        n = load_qt_chapters(fd, find_trak(ctx.chap_ids[i]), cue);
    if (n < 2 && ctx.chpl_off)
        n = load_chpl_chapters(fd, cue);
    close(fd);

    /* two real, ordered chapters or it isn't worth activating the engine */
    if (n < 2)
        return false;
    for (int i = 1; i < n; i++)
        if (cue->tracks[i].offset < cue->tracks[i - 1].offset)
            return false;
    cue->track_count = n;
    return true;
}
