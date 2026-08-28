/***************************************************************************
 * Synchronized LRC lyrics for TrimPod(RUS).
 *
 * Lookup order: audio-sidecar .lrc, persistent cache, LRCLIB exact match,
 * LRCLIB fuzzy search.  Network work is delegated to the small wget shipped
 * in the pak, so the Rockbox UI thread never waits for the network.
 *
 * The lyrics lookup and caching flow is based in part on NextUI Music Player
 * by Mohammad Syuhada (Copyright (c) 2025, MIT License):
 * https://github.com/mohammadsyuhada/nextui-music-player
 ***************************************************************************/
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "dir.h"
#include "file.h"
#include "kernel.h"
#include "metadata.h"
#include "rbunicode.h"
#include "system.h"
#include "trimpod_lyrics.h"

#define LYRICS_MAX_LINES       512
#define LYRICS_MAX_FILE        (256 * 1024)
#define LYRICS_CACHE_DIR_NAME  "lyrics"
#ifndef TRIMPOD_H700
#define LYRICS_WGET            "./bin/wget"
#endif
#define LYRICS_USER_AGENT      "TrimPod(RUS)/1.0.7-rus-0.5 " \
                               "(https://github.com/B3L4CQU4/" \
                               "c-trimui-trimpod-classic-pak-RUS)"

enum fetch_stage
{
    FETCH_NONE = 0,
    FETCH_PENDING,
    FETCH_EXACT,
    FETCH_FUZZY,
};

static struct trimpod_lyric_line lyric_lines[LYRICS_MAX_LINES];
static char lyric_data[LYRICS_MAX_FILE + 1];
static char track_path[MAX_PATH];
static char track_artist[256];
static char track_title[256];
static char cache_path[MAX_PATH];
static char legacy_cache_path[MAX_PATH];
static char response_path[MAX_PATH];
static char marker_path[MAX_PATH];
static char status_path[MAX_PATH];
static int lyric_count;
static int fetch_serial;
static int track_duration;
static long fetch_due_tick;
static enum fetch_stage fetch_stage;
static enum trimpod_lyrics_status lyrics_status = TRIMPOD_LYRICS_IDLE;

static unsigned int lyrics_path_hash(const char *path)
{
    unsigned int hash = 5381;
    const unsigned char *p;

    for (p = (const unsigned char *)path; *p; p++)
        hash = ((hash << 5) + hash) + *p;
    return hash;
}

static unsigned int lyrics_hash(const char *artist, const char *title)
{
    unsigned int hash = 5381;
    const unsigned char *p;

    for (p = (const unsigned char *)artist; *p; p++)
        hash = ((hash << 5) + hash) + *p;
    hash = ((hash << 5) + hash) + (unsigned char)'\n';
    for (p = (const unsigned char *)title; *p; p++)
        hash = ((hash << 5) + hash) + *p;
    return hash;
}

static void copy_text(char *dst, size_t size, const char *src)
{
    if (size == 0)
        return;
    snprintf(dst, size, "%s", src ? src : "");
}

static void title_from_path(char *dst, size_t size, const char *path)
{
    const char *base = strrchr(path, '/');
    const char *dot;
    size_t len;

    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= size)
        len = size - 1;
    memcpy(dst, base, len);
    dst[len] = '\0';
}

static void make_sidecar_path(char *dst, size_t size, const char *audio_path,
                              const char *extension)
{
    const char *slash = strrchr(audio_path, '/');
    const char *dot = strrchr(audio_path, '.');
    size_t base_len = strlen(audio_path);

    if (dot && (!slash || dot > slash))
        base_len = (size_t)(dot - audio_path);
    if (base_len >= size)
        base_len = size - 1;
    memcpy(dst, audio_path, base_len);
    dst[base_len] = '\0';
    snprintf(dst + base_len, size - base_len, "%s", extension);
}

static int read_text_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    off_t size = filesize(fd);
    if (size <= 0 || size > LYRICS_MAX_FILE)
    {
        close(fd);
        return -1;
    }

    ssize_t got = read(fd, lyric_data, (size_t)size);
    close(fd);
    if (got != size)
        return -1;

    lyric_data[got] = '\0';
    return (int)got;
}

static int parse_timestamp(const char *p, const char **end, long *time_ms)
{
    long minutes = 0, seconds = 0, fraction = 0;
    int fraction_digits = 0;

    if (!isdigit((unsigned char)*p))
        return 0;
    while (isdigit((unsigned char)*p))
        minutes = minutes * 10 + (*p++ - '0');
    if (*p++ != ':')
        return 0;
    if (!isdigit((unsigned char)*p))
        return 0;
    while (isdigit((unsigned char)*p))
        seconds = seconds * 10 + (*p++ - '0');
    if (*p == '.' || *p == ':')
    {
        p++;
        while (isdigit((unsigned char)*p) && fraction_digits < 3)
        {
            fraction = fraction * 10 + (*p++ - '0');
            fraction_digits++;
        }
        while (isdigit((unsigned char)*p))
            p++;
    }
    if (*p != ']')
        return 0;

    if (fraction_digits == 1)
        fraction *= 100;
    else if (fraction_digits == 2)
        fraction *= 10;

    *time_ms = minutes * 60000 + seconds * 1000 + fraction;
    *end = p + 1;
    return 1;
}

static void sort_lyrics(void)
{
    int i;
    for (i = 1; i < lyric_count; i++)
    {
        struct trimpod_lyric_line item = lyric_lines[i];
        int j = i - 1;
        while (j >= 0 && lyric_lines[j].time_ms > item.time_ms)
        {
            lyric_lines[j + 1] = lyric_lines[j];
            j--;
        }
        lyric_lines[j + 1] = item;
    }
}

static int parse_lrc(const char *data)
{
    const char *p = data;
    lyric_count = 0;

    if ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb &&
        (unsigned char)p[2] == 0xbf)
        p += 3; /* optional UTF-8 BOM */

    while (*p && lyric_count < LYRICS_MAX_LINES)
    {
        const char *line_end = p;
        const char *text;
        const char *after;
        long time_ms;
        size_t len;

        while (*line_end && *line_end != '\r' && *line_end != '\n')
            line_end++;

        if (*p == '[' && parse_timestamp(p + 1, &after, &time_ms))
        {
            text = after;
            while (text < line_end && (*text == ' ' || *text == '\t'))
                text++;
            len = (size_t)(line_end - text);
            while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'))
                len--;
            if (len > 0)
            {
                if (len >= sizeof(lyric_lines[lyric_count].text))
                {
                    len = sizeof(lyric_lines[lyric_count].text) - 1;
                    /* Never leave an incomplete UTF-8 character at the end. */
                    while (len > 0 &&
                           ((unsigned char)text[len] & 0xc0) == 0x80)
                        len--;
                }
                lyric_lines[lyric_count].time_ms = time_ms;
                memcpy(lyric_lines[lyric_count].text, text, len);
                lyric_lines[lyric_count].text[len] = '\0';
                lyric_count++;
            }
        }

        p = line_end;
        while (*p == '\r' || *p == '\n')
            p++;
    }

    sort_lyrics();
    return lyric_count;
}

static int load_lrc_file(const char *path)
{
    return read_text_file(path) > 0 ? parse_lrc(lyric_data) : 0;
}

static void ensure_cache_paths(void)
{
    const char *home = getenv("HOME");
    char cache_dir[MAX_PATH];

    if (!home || !*home)
        home = "/tmp";
    snprintf(cache_dir, sizeof(cache_dir), "%s/%s", home,
             LYRICS_CACHE_DIR_NAME);
    mkdir(cache_dir);
    /* The path key stays stable while Rockbox updates preview metadata during
     * fast track skipping.  Keep the old artist/title key for migration. */
    snprintf(cache_path, sizeof(cache_path), "%s/p-%08x.lrc", cache_dir,
             lyrics_path_hash(track_path));
    snprintf(legacy_cache_path, sizeof(legacy_cache_path), "%s/%08x.lrc",
             cache_dir,
             lyrics_hash(track_artist, track_title));
}

static void save_cache(const char *data)
{
    int fd = open(cache_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    write(fd, data, strlen(data));
    close(fd);
}

static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;

    while (*src && out + 4 < dst_size)
    {
        unsigned char c = (unsigned char)*src++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            dst[out++] = (char)c;
        else
        {
            dst[out++] = '%';
            dst[out++] = hex[c >> 4];
            dst[out++] = hex[c & 15];
        }
    }
    dst[out] = '\0';
}

static void start_fetch(enum fetch_stage stage)
{
    char encoded_artist[768];
    char encoded_title[768];
    char encoded_query[1536];
    char query[520];
    char url[2304];
    char command[3584];

    fetch_stage = stage;
    fetch_serial++;
    snprintf(response_path, sizeof(response_path),
             "/tmp/trimpod-lyrics-%ld-%d.json", (long)getpid(), fetch_serial);
    snprintf(marker_path, sizeof(marker_path),
             "/tmp/trimpod-lyrics-%ld-%d.done", (long)getpid(), fetch_serial);
    snprintf(status_path, sizeof(status_path),
             "/tmp/trimpod-lyrics-%ld-%d.log", (long)getpid(), fetch_serial);
    remove(response_path);
    remove(marker_path);
    remove(status_path);

    url_encode(track_artist, encoded_artist, sizeof(encoded_artist));
    url_encode(track_title, encoded_title, sizeof(encoded_title));
    if (stage == FETCH_EXACT)
    {
        snprintf(url, sizeof(url),
                 "https://lrclib.net/api/get?artist_name=%s&track_name=%s&duration=%d",
                 encoded_artist, encoded_title, track_duration);
    }
    else
    {
        snprintf(query, sizeof(query), "%s %s", track_artist, track_title);
        url_encode(query, encoded_query, sizeof(encoded_query));
        snprintf(url, sizeof(url), "https://lrclib.net/api/search?q=%s",
                 encoded_query);
    }

    /* The paths are generated locally and the URL is percent-encoded.  The
     * marker is written even on an HTTP/network error so polling never hangs. */
#ifdef TRIMPOD_H700
    /* NextUI's H700 port ships its own statically-linked curl in the system
     * PATH because the stock Anbernic userspace has no dependable HTTP client.
     * Use that native runtime instead of the tg5040-linked wget bundled here. */
    snprintf(command, sizeof(command),
             "(curl --silent --show-error --location --fail "
             "--connect-timeout 10 --max-time 15 --retry 1 "
             "--max-filesize 262144 --user-agent '%s' "
             "--dump-header '%s' --output '%s' '%s'; "
             "printf '%%d' $? > '%s') >/dev/null 2>&1 &",
             LYRICS_USER_AGENT, status_path, response_path, url, marker_path);
#else
    snprintf(command, sizeof(command),
             "(%s -T 10 -t 1 --quota=256k --no-check-certificate "
             "--user-agent='%s' --server-response -o '%s' "
             "-O '%s' '%s'; "
             "printf '%%d' $? > '%s') >/dev/null 2>&1 &",
             LYRICS_WGET, LYRICS_USER_AGENT, status_path,
             response_path, url, marker_path);
#endif
    if (system(command) < 0)
    {
        fetch_stage = FETCH_NONE;
        lyrics_status = TRIMPOD_LYRICS_ERROR;
    }
    else
        lyrics_status = TRIMPOD_LYRICS_LOADING;
}

static void queue_fetch(void)
{
    /* Fast-skip preview metadata can change several times in a fraction of a
     * second.  Wait briefly so intermediate tracks never reach LRCLIB. */
    fetch_stage = FETCH_PENDING;
    fetch_due_tick = current_tick + HZ / 2;
    lyrics_status = TRIMPOD_LYRICS_LOADING;
}

static int read_fetch_result(void)
{
    char result[16];
    int fd = open(marker_path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t got = read(fd, result, sizeof(result) - 1);
    close(fd);
    if (got <= 0)
        return -1;
    result[got] = '\0';
    return atoi(result);
}

static int fetch_was_rate_limited(void)
{
    int limited = 0;

    if (read_text_file(status_path) > 0)
        limited = strstr(lyric_data, " 429 ") != NULL;

    remove(status_path);
    return limited;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Locate the first non-null syncedLyrics value and decode it in-place at the
 * beginning of lyric_data.  This tiny parser deliberately ignores all other
 * JSON fields and works for both LRCLIB's object and search-array responses. */
static char *decode_synced_lyrics(void)
{
    const char *key = "\"syncedLyrics\"";
    char *scan = lyric_data;

    while ((scan = strstr(scan, key)) != NULL)
    {
        char *src = scan + strlen(key);
        char *dst = lyric_data;

        while (*src && *src != ':') src++;
        if (*src == ':') src++;
        while (*src && isspace((unsigned char)*src)) src++;
        if (*src != '"')
        {
            scan += strlen(key);
            continue;
        }
        src++;

        while (*src && *src != '"')
        {
            if (*src != '\\')
            {
                *dst++ = *src++;
                continue;
            }

            src++;
            switch (*src)
            {
                case 'n': *dst++ = '\n'; src++; break;
                case 'r': *dst++ = '\r'; src++; break;
                case 't': *dst++ = '\t'; src++; break;
                case 'b': *dst++ = '\b'; src++; break;
                case 'f': *dst++ = '\f'; src++; break;
                case '"': *dst++ = '"'; src++; break;
                case '\\': *dst++ = '\\'; src++; break;
                case '/': *dst++ = '/'; src++; break;
                case 'u':
                {
                    unsigned long code = 0;
                    int i;
                    src++;
                    for (i = 0; i < 4; i++)
                    {
                        int value = hex_value(src[i]);
                        if (value < 0)
                            break;
                        code = (code << 4) | (unsigned long)value;
                    }
                    if (i == 4)
                    {
                        dst = (char *)utf8encode(code, (unsigned char *)dst);
                        src += 4;
                    }
                    break;
                }
                default:
                    if (*src) *dst++ = *src++;
                    break;
            }
        }
        *dst = '\0';
        return lyric_data[0] ? lyric_data : NULL;
    }
    return NULL;
}

void trimpod_lyrics_request(const struct mp3entry *id3)
{
    char sidecar[MAX_PATH];
    char artist[256];
    char title[256];
    int duration;

    if (!id3 || !id3->path[0])
    {
        trimpod_lyrics_clear();
        return;
    }

    copy_text(artist, sizeof(artist), id3->artist);
    if (id3->title && id3->title[0])
        copy_text(title, sizeof(title), id3->title);
    else
        title_from_path(title, sizeof(title), id3->path);
    duration = (int)((id3->length + 500) / 1000);

    if (!strcmp(track_path, id3->path) &&
        lyrics_status != TRIMPOD_LYRICS_IDLE)
    {
        /* Loaded lyrics always win.  Otherwise restart only if Rockbox has
         * replaced fast-skip preview metadata with different final metadata. */
        if (lyrics_status == TRIMPOD_LYRICS_READY ||
            (!strcmp(track_artist, artist) && !strcmp(track_title, title) &&
             track_duration == duration))
            return;
    }

    trimpod_lyrics_clear();
    copy_text(track_path, sizeof(track_path), id3->path);
    copy_text(track_artist, sizeof(track_artist), artist);
    copy_text(track_title, sizeof(track_title), title);
    track_duration = duration;
    ensure_cache_paths();

    make_sidecar_path(sidecar, sizeof(sidecar), id3->path, ".lrc");
    if (load_lrc_file(sidecar) > 0)
    {
        lyrics_status = TRIMPOD_LYRICS_READY;
        return;
    }
    make_sidecar_path(sidecar, sizeof(sidecar), id3->path, ".LRC");
    if (load_lrc_file(sidecar) > 0)
    {
        lyrics_status = TRIMPOD_LYRICS_READY;
        return;
    }
    if (load_lrc_file(cache_path) > 0)
    {
        lyrics_status = TRIMPOD_LYRICS_READY;
        return;
    }
    if (load_lrc_file(legacy_cache_path) > 0)
    {
        /* Preserve compatibility with lyrics cached before path-based keys. */
        save_cache(lyric_data);
        lyrics_status = TRIMPOD_LYRICS_READY;
        return;
    }

    if (!track_artist[0] && !track_title[0])
        lyrics_status = TRIMPOD_LYRICS_NOT_FOUND;
    else
        queue_fetch();
}

void trimpod_lyrics_poll(void)
{
    char *lrc;
    enum fetch_stage completed_stage;
    int fetch_result;
    int rate_limited;

    if (lyrics_status != TRIMPOD_LYRICS_LOADING)
        return;

    if (fetch_stage == FETCH_PENDING)
    {
        if (!TIME_BEFORE(current_tick, fetch_due_tick))
            start_fetch(FETCH_EXACT);
        return;
    }

    if (!file_exists(marker_path))
        return;

    completed_stage = fetch_stage;
    fetch_stage = FETCH_NONE;
    fetch_result = read_fetch_result();
    remove(marker_path);
    rate_limited = fetch_was_rate_limited();

    if (!rate_limited && fetch_result == 0 &&
        read_text_file(response_path) > 0)
    {
        remove(response_path);
        lrc = decode_synced_lyrics();
        if (lrc && parse_lrc(lrc) > 0)
        {
            save_cache(lrc);
            lyrics_status = TRIMPOD_LYRICS_READY;
            return;
        }
    }
    else
        remove(response_path);

    if (rate_limited)
        lyrics_status = TRIMPOD_LYRICS_ERROR;
    else if (completed_stage == FETCH_EXACT)
        start_fetch(FETCH_FUZZY);
    else if (fetch_result != 0)
        lyrics_status = TRIMPOD_LYRICS_ERROR;
    else
        lyrics_status = TRIMPOD_LYRICS_NOT_FOUND;
}

void trimpod_lyrics_clear(void)
{
    if (marker_path[0]) remove(marker_path);
    if (response_path[0]) remove(response_path);
    if (status_path[0]) remove(status_path);
    marker_path[0] = '\0';
    response_path[0] = '\0';
    status_path[0] = '\0';
    track_path[0] = '\0';
    lyric_count = 0;
    fetch_due_tick = 0;
    fetch_stage = FETCH_NONE;
    lyrics_status = TRIMPOD_LYRICS_IDLE;
}

enum trimpod_lyrics_status trimpod_lyrics_get_status(void)
{
    return lyrics_status;
}

int trimpod_lyrics_count(void)
{
    return lyric_count;
}

const struct trimpod_lyric_line *trimpod_lyrics_line(int index)
{
    if (index < 0 || index >= lyric_count)
        return NULL;
    return &lyric_lines[index];
}

int trimpod_lyrics_current(unsigned long elapsed_ms)
{
    int low = 0, high = lyric_count - 1, result = 0;
    if (lyric_count <= 0)
        return -1;

    while (low <= high)
    {
        int middle = low + (high - low) / 2;
        if ((unsigned long)lyric_lines[middle].time_ms <= elapsed_ms)
        {
            result = middle;
            low = middle + 1;
        }
        else
            high = middle - 1;
    }
    return result;
}
