/* Trimpod: SQLite-backed library index -- see trimpod_library.h.
 *
 * The DB is a derived index; the SD-card files are the source of truth.  So on
 * schema change we migrate in place, and only rebuild from scratch when the file
 * is unreadable (never as routine recovery).
 *
 * Scanning uses libc stat() for the per-directory mtime gate.  Rockbox's app
 * file API exposes mtime only through dir_get_info() (i.e. by enumerating the
 * parent), which would defeat the gate; on this hosted target OS and Rockbox
 * paths map 1:1 (app_root_realpath()=="/"), so a direct libc stat() on the same
 * path string is valid -- the same precedent as the extern libc symlink() in
 * trimpod_folders.c.  Directory *enumeration* still uses Rockbox opendir/
 * readdir/dir_get_info (whose mtime is also os_stat-derived, so the two agree),
 * and file opens for tagging use Rockbox open() so the fd matches get_metadata().
 *
 * Single-threaded: reconcile runs synchronously at init, on a source-folder
 * add/remove, and on manual rescan; there is no background writer, so the
 * journal mode is TRUNCATE (no WAL).  The parse loop yields per file so
 * playback keeps running.  The db is worked on in tmpfs and written back to
 * the card after changes (see "tmpfs working copy"). */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <ctype.h>
#include <stdio.h>
#include <sys/stat.h>  /* stat() -- directory-mtime gate (see header note) */

#include "sqlite3.h"

#include "dir.h"       /* opendir/readdir/dir_get_info, ATTR_DIRECTORY */
#include "file.h"      /* open/close/remove, O_RDONLY, MAX_PATH */
#include "pathfuncs.h"
#include "rbpaths.h"   /* ROCKBOX_DIR */
#include "filetypes.h" /* filetype_get_attr, FILE_ATTR_AUDIO */
#include "metadata.h"  /* get_metadata, struct mp3entry */
#include "settings.h"  /* TRIMPOD_MAX_FILES_IN_PLAYLIST */
#include "lang.h"      /* str, LANG_TRIMPOD_SCANNING */
#include "kernel.h"    /* yield */
#include "backlight.h" /* backlight_on */
#include "trimpod_folders.h"
#include "trimpod_ui.h" /* trimpod_fullscreen_message */

#define SCHEMA_VERSION 3

static sqlite3 *db;

/* "Scanning Library NN%" held over the synchronous scan.  Hidden until the walk
 * finds work (a forced rescan shows it up front): 0% from then, the percent
 * parsed through pass 2, 100% through commit + write-back.  Centered on the
 * widest form so the number grows in place. */
#define SCAN_HIDDEN (-1)
static int scan_shown = SCAN_HIDDEN;   /* percent on screen, or hidden */

static void scan_show(int pct)
{
    if (pct == scan_shown) return;
    char widest[64], msg[64];
    snprintf(widest, sizeof widest, "%s 100%%", str(LANG_TRIMPOD_SCANNING));
    snprintf(msg, sizeof msg, "%s %d%%", str(LANG_TRIMPOD_SCANNING), pct);
    trimpod_fullscreen_message(msg, widest);
    backlight_on();
    scan_shown = pct;
}

/* Files the walk found needing a parse.  Parsing is the slow part (~50 ms per
 * file on the card), so it's deferred to a second pass over this list, which
 * also gives the percent its total. */
struct pend_item { char *path; int root_id, category; time_t mtime; off_t size; };
static struct { struct pend_item *v; int n, cap; } pend;

static void pend_add(const char *path, int root_id, int category,
                     time_t mtime, off_t size)
{
    if (pend.n == pend.cap)
    {
        int ncap = pend.cap ? pend.cap * 2 : 64;
        struct pend_item *nv = realloc(pend.v, ncap * sizeof *nv);
        if (!nv) return;                 /* best effort: drop this entry, keep prior */
        pend.v = nv; pend.cap = ncap;
    }
    char *dup = strdup(path);
    if (dup)
        pend.v[pend.n++] = (struct pend_item){ dup, root_id, category, mtime, size };
}
static void pend_free(void)
{
    for (int i = 0; i < pend.n; i++) free(pend.v[i].path);
    free(pend.v); pend.v = NULL; pend.n = pend.cap = 0;
}

/* ---- schema ----------------------------------------------------------- */

static const char *SCHEMA_SQL =
    "CREATE TABLE roots("
    "  id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, category INTEGER NOT NULL);"
    "CREATE TABLE dirs("
    "  path TEXT PRIMARY KEY, parent TEXT,"
    "  root_id INTEGER NOT NULL REFERENCES roots(id) ON DELETE CASCADE,"
    "  mtime INTEGER NOT NULL);"
    "CREATE INDEX idx_dirs_parent ON dirs(parent);"
    "CREATE TABLE tracks("
    "  id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, dir TEXT NOT NULL,"
    "  root_id INTEGER NOT NULL REFERENCES roots(id) ON DELETE CASCADE,"
    "  category INTEGER NOT NULL, mtime INTEGER NOT NULL, size INTEGER NOT NULL,"
    "  title TEXT, artist TEXT, albumartist TEXT, album TEXT, genre TEXT,"
    "  composer TEXT, grouping TEXT, year INTEGER, tracknum INTEGER, discnum INTEGER,"
    "  duration_ms INTEGER, codectype INTEGER, bitrate INTEGER, frequency INTEGER,"
    "  browse_artist TEXT, sort_artist TEXT);"
    "CREATE INDEX idx_tracks_dir ON tracks(dir);"
    "CREATE INDEX idx_tracks_artist ON tracks(category, sort_artist, album, discnum, tracknum);"
    "CREATE INDEX idx_tracks_album  ON tracks(category, album, discnum, tracknum);"
    "CREATE INDEX idx_tracks_browse ON tracks(category, browse_artist, album, discnum, tracknum);"
    "CREATE INDEX idx_tracks_title  ON tracks(category, title, path);"
    "CREATE INDEX idx_tracks_genre  ON tracks(category, genre);";

/* ---- small growable path set (no fixed cap) --------------------------- */

struct pathset { char **v; int n, cap; };

static void ps_init(struct pathset *s) { s->v = NULL; s->n = s->cap = 0; }
static void ps_add(struct pathset *s, const char *p)
{
    if (s->n == s->cap)
    {
        int ncap = s->cap ? s->cap * 2 : 16;
        char **nv = realloc(s->v, ncap * sizeof *nv);
        if (!nv) return;                 /* best effort: drop this entry, keep prior */
        s->v = nv; s->cap = ncap;
    }
    char *dup = strdup(p);
    if (dup) s->v[s->n++] = dup;
}
static bool ps_has(struct pathset *s, const char *p)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->v[i], p) == 0) return true;
    return false;
}
static void ps_free(struct pathset *s)
{
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v); ps_init(s);
}

/* ---- helpers ---------------------------------------------------------- */

static int db_exec(const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL); }

static void bind_str(sqlite3_stmt *st, int i, const char *s)
{
    if (s && s[0]) sqlite3_bind_text(st, i, s, -1, SQLITE_STATIC);
    else           sqlite3_bind_null(st, i);
}

/* iPod sort key: lowercase, leading "the " dropped. */
static void make_sort_artist(const char *ba, char *out, size_t n)
{
    if (!ba) ba = "";
    if (strncasecmp(ba, "the ", 4) == 0) ba += 4;
    size_t i = 0;
    for (; ba[i] && i + 1 < n; i++) out[i] = tolower((unsigned char)ba[i]);
    out[i] = '\0';
}

static int user_version(void)
{
    sqlite3_stmt *st; int v = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, NULL) == SQLITE_OK)
    {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}
static void set_user_version(int v)
{
    char sql[48];
    snprintf(sql, sizeof sql, "PRAGMA user_version=%d;", v);
    db_exec(sql);
}

/* ---- track / dir row ops --------------------------------------------- */

/* The scan reuses ONE prepared statement per query for the whole reconcile
 * (prepare once, then bind/step/reset per row) instead of recompiling per
 * file -- the SQLite-idiomatic hot-loop pattern.  The statements are scoped to
 * a single reconcile: opened at its start, finalized at its end, so none
 * survives a db close/reopen. */
static const char TRACK_UPSERT_SQL[] =
    "INSERT INTO tracks(path,dir,root_id,category,mtime,size,title,artist,"
    "albumartist,album,genre,composer,grouping,year,tracknum,discnum,"
    "duration_ms,codectype,bitrate,frequency,browse_artist,sort_artist)"
    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22)"
    " ON CONFLICT(path) DO UPDATE SET dir=?2,root_id=?3,category=?4,mtime=?5,"
    "size=?6,title=?7,artist=?8,albumartist=?9,album=?10,genre=?11,composer=?12,"
    "grouping=?13,year=?14,tracknum=?15,discnum=?16,duration_ms=?17,codectype=?18,"
    "bitrate=?19,frequency=?20,browse_artist=?21,sort_artist=?22;";

struct scan_stmts
{
    sqlite3_stmt *track_sel;    /* SELECT mtime,size FROM tracks WHERE path=? */
    sqlite3_stmt *track_ins;    /* the upsert above                          */
    sqlite3_stmt *track_del;    /* DELETE FROM tracks WHERE path=?            */
    sqlite3_stmt *dir_sel;      /* SELECT mtime FROM dirs WHERE path=?        */
    sqlite3_stmt *dir_ins;      /* upsert into dirs                          */
    sqlite3_stmt *dir_del;      /* DELETE FROM dirs WHERE path=?              */
    sqlite3_stmt *kids_sel;     /* SELECT path FROM dirs WHERE parent=?       */
    sqlite3_stmt *dtracks_sel;  /* SELECT path FROM tracks WHERE dir=?        */
};

static bool prep(sqlite3_stmt **st, const char *sql)
{
    return sqlite3_prepare_v2(db, sql, -1, st, NULL) == SQLITE_OK;
}
/* Returns false if any statement failed to compile (partial set is safe to
 * close: sqlite3_finalize(NULL) is a no-op). */
static bool scan_stmts_open(struct scan_stmts *s)
{
    memset(s, 0, sizeof *s);
    return prep(&s->track_sel,   "SELECT mtime,size FROM tracks WHERE path=?;")
        && prep(&s->track_ins,   TRACK_UPSERT_SQL)
        && prep(&s->track_del,   "DELETE FROM tracks WHERE path=?;")
        && prep(&s->dir_sel,     "SELECT mtime FROM dirs WHERE path=?;")
        && prep(&s->dir_ins,
                "INSERT INTO dirs(path,parent,root_id,mtime) VALUES(?1,?2,?3,?4)"
                " ON CONFLICT(path) DO UPDATE SET parent=?2,root_id=?3,mtime=?4;")
        && prep(&s->dir_del,     "DELETE FROM dirs WHERE path=?;")
        && prep(&s->kids_sel,    "SELECT path FROM dirs WHERE parent=?;")
        && prep(&s->dtracks_sel, "SELECT path FROM tracks WHERE dir=?;");
}
static void scan_stmts_close(struct scan_stmts *s)
{
    sqlite3_finalize(s->track_sel);  sqlite3_finalize(s->track_ins);
    sqlite3_finalize(s->track_del);  sqlite3_finalize(s->dir_sel);
    sqlite3_finalize(s->dir_ins);    sqlite3_finalize(s->dir_del);
    sqlite3_finalize(s->kids_sel);   sqlite3_finalize(s->dtracks_sel);
}

/* Each helper binds, steps, then sqlite3_reset()s so the statement is clean for
 * the next row.  We rebind every parameter each call, so no clear_bindings is
 * needed.  Each use fully drains + resets before returning, so the same
 * statement is safely reused down the recursive scan (single-threaded). */
static bool track_unchanged(sqlite3_stmt *st, const char *path,
                            time_t mtime, off_t size)
{
    bool same = false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        same = sqlite3_column_int64(st, 0) == (sqlite3_int64)mtime &&
               sqlite3_column_int64(st, 1) == (sqlite3_int64)size;
    sqlite3_reset(st);
    return same;
}

static bool parse_and_upsert(sqlite3_stmt *st, const char *path, const char *dir,
                             int root_id, int category, time_t mtime, off_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    static struct mp3entry id3;          /* large; reused (single-threaded scan) */
    bool ok = get_metadata(&id3, fd, path);
    close(fd);
    if (!ok) return false;

    const char *ba = (id3.albumartist && id3.albumartist[0]) ? id3.albumartist
                   : (id3.artist ? id3.artist : "");
    char sortbuf[128];
    make_sort_artist(ba, sortbuf, sizeof sortbuf);

    sqlite3_bind_text (st, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 2, dir,  -1, SQLITE_STATIC);
    sqlite3_bind_int  (st, 3, root_id);
    sqlite3_bind_int  (st, 4, category);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)mtime);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)size);
    bind_str(st,  7, id3.title);
    bind_str(st,  8, id3.artist);
    bind_str(st,  9, id3.albumartist);
    bind_str(st, 10, id3.album);
    bind_str(st, 11, id3.genre_string);
    bind_str(st, 12, id3.composer);
    bind_str(st, 13, id3.grouping);
    sqlite3_bind_int(st, 14, id3.year);
    sqlite3_bind_int(st, 15, id3.tracknum);
    sqlite3_bind_int(st, 16, id3.discnum);
    sqlite3_bind_int64(st, 17, (sqlite3_int64)id3.length);
    sqlite3_bind_int(st, 18, (int)id3.codectype);
    sqlite3_bind_int(st, 19, (int)id3.bitrate);
    sqlite3_bind_int64(st, 20, (sqlite3_int64)id3.frequency);
    sqlite3_bind_text(st, 21, ba, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 22, sortbuf, -1, SQLITE_TRANSIENT);
    bool done = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_reset(st);
    return done;
}

static void upsert_dir(sqlite3_stmt *st, const char *path, const char *parent,
                       int root_id, time_t mtime)
{
    sqlite3_bind_text (st, 1, path, -1, SQLITE_STATIC);
    bind_str          (st, 2, parent);
    sqlite3_bind_int  (st, 3, root_id);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)mtime);
    sqlite3_step(st);
    sqlite3_reset(st);
}

static bool dir_stored_mtime(sqlite3_stmt *st, const char *path, time_t *out)
{
    bool found = false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
    { *out = (time_t)sqlite3_column_int64(st, 0); found = true; }
    sqlite3_reset(st);
    return found;
}

/* Collect child paths (col 0) from a reused single-text-param query. */
static void collect(sqlite3_stmt *st, const char *param, struct pathset *out)
{
    sqlite3_bind_text(st, 1, param, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW)
        ps_add(out, (const char *)sqlite3_column_text(st, 0));
    sqlite3_reset(st);
}

static void delete_where(sqlite3_stmt *st, const char *path)
{
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_reset(st);
}

/* ---- recursive stat-gated scan --------------------------------------- */

static int scan_dir(struct scan_stmts *s, const char *path, const char *parent,
                    int root_id, int category, bool force)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;                        /* vanished; the parent pass prunes its rows */
    time_t cur = st.st_mtime;

    /* Gate: unchanged dir -> its direct entries are unchanged; skip the readdir
     * and just recurse known subdirs (a deep change bumps only that subdir). */
    if (!force)
    {
        time_t stored;
        if (dir_stored_mtime(s->dir_sel, path, &stored) && stored == cur)
        {
            struct pathset kids; ps_init(&kids);
            collect(s->kids_sel, path, &kids);
            int touched = 0;
            for (int i = 0; i < kids.n; i++)
                touched += scan_dir(s, kids.v[i], path, root_id, category, force);
            ps_free(&kids);
            return touched;
        }
    }

    /* New or changed: enumerate and reconcile this directory's direct children. */
    DIR *d = opendir(path);
    if (!d) return 0;
    int touched = 0;
    struct pathset seen_files, seen_dirs;
    ps_init(&seen_files); ps_init(&seen_dirs);
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.') continue;
        char child[MAX_PATH];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= sizeof child)
            continue;
        struct dirinfo info = dir_get_info(d, e);
        if (info.attribute & ATTR_DIRECTORY)
        {
            ps_add(&seen_dirs, child);
            touched += scan_dir(s, child, path, root_id, category, force);
        }
        else if ((filetype_get_attr(e->d_name) & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
        {
            ps_add(&seen_files, child);
            if (!track_unchanged(s->track_sel, child, info.mtime, info.size))
            {
                scan_show(0);                /* first real work: show the screen */
                pend_add(child, root_id, category, info.mtime, info.size);
            }
        }
    }
    closedir(d);

    /* Prune direct children that disappeared. */
    struct pathset rows; ps_init(&rows);
    collect(s->dtracks_sel, path, &rows);
    for (int i = 0; i < rows.n; i++)
        if (!ps_has(&seen_files, rows.v[i]))
        { delete_where(s->track_del, rows.v[i]); touched++; }
    ps_free(&rows);

    ps_init(&rows);
    collect(s->kids_sel, path, &rows);
    for (int i = 0; i < rows.n; i++)
        if (!ps_has(&seen_dirs, rows.v[i]))
            delete_where(s->dir_del, rows.v[i]);   /* cascades tracks */
    ps_free(&rows);

    upsert_dir(s->dir_ins, path, parent, root_id, cur);
    ps_free(&seen_files); ps_free(&seen_dirs);
    return touched;
}

/* ---- roots sync ------------------------------------------------------- */

static bool root_is_configured(const char *path)
{
    for (int c = 0; c < TP_CAT_COUNT; c++)
        for (int i = 0, n = trimpod_folders_root_count(c); i < n; i++)
            if (strcmp(trimpod_folders_root_path(c, i), path) == 0) return true;
    return false;
}

static int root_id_for(const char *path)
{
    sqlite3_stmt *st; int id = 0;
    if (sqlite3_prepare_v2(db, "SELECT id FROM roots WHERE path=?;",
                           -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return id;
}

static void sync_roots(void)
{
    /* upsert every configured root */
    for (int c = 0; c < TP_CAT_COUNT; c++)
        for (int i = 0, n = trimpod_folders_root_count(c); i < n; i++)
        {
            const char *p = trimpod_folders_root_path(c, i);
            sqlite3_stmt *st;
            if (sqlite3_prepare_v2(db,
                    "INSERT INTO roots(path,category) VALUES(?1,?2)"
                    " ON CONFLICT(path) DO UPDATE SET category=?2 WHERE category!=?2;",
                    -1, &st, NULL) == SQLITE_OK)
            {
                sqlite3_bind_text(st, 1, p, -1, SQLITE_STATIC);
                sqlite3_bind_int (st, 2, c);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
        }
    /* drop roots no longer configured (cascades their dirs + tracks) */
    struct pathset gone; ps_init(&gone);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT path FROM roots;", -1, &st, NULL) == SQLITE_OK)
    {
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            const char *p = (const char *)sqlite3_column_text(st, 0);
            if (!root_is_configured(p)) ps_add(&gone, p);
        }
        sqlite3_finalize(st);
    }
    if (gone.n)
    {
        sqlite3_stmt *del;
        if (sqlite3_prepare_v2(db, "DELETE FROM roots WHERE path=?;",
                               -1, &del, NULL) == SQLITE_OK)
        {
            for (int i = 0; i < gone.n; i++)
                delete_where(del, gone.v[i]);
            sqlite3_finalize(del);
        }
    }
    ps_free(&gone);
}

/* ---- tmpfs working copy ------------------------------------------------ *
 * The card is a `sync` vfat mount: every 4 KB page SQLite writes costs ~18 ms,
 * and a library change dirties most pages twice (journal + db), so committing
 * there took ~15 s.  The index is worked on in tmpfs instead and copied back
 * to the card in one sequential pass after a reconcile that changed it. */
#define DB_SD    ROCKBOX_DIR "/library.db"
#define DB_RAM   "/tmp/trimpod-library.db"
#define COPY_BUF (256 * 1024)

static bool db_in_ram;

static bool copy_file(const char *src, const char *dst)
{
    bool ok = false;
    int in = open(src, O_RDONLY);
    if (in < 0) return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    char *buf = malloc(COPY_BUF);
    if (out >= 0 && buf)
    {
        ssize_t n;
        while ((n = read(in, buf, COPY_BUF)) > 0)
            if (write(out, buf, n) != n) break;
        ok = n == 0;
    }
    free(buf);
    if (out >= 0) close(out);
    close(in);
    return ok;
}

/* tmp + rename: a kill mid-write leaves the card's old copy intact. */
static void db_writeback(void)
{
    if (db_in_ram && copy_file(DB_RAM, DB_SD ".new"))
        rename(DB_SD ".new", DB_SD);
}

/* ---- public API ------------------------------------------------------- */

int trimpod_library_reconcile(bool force_full)
{
    if (!db) return 0;
    trimpod_folders_load_all();
    scan_shown = SCAN_HIDDEN;
    if (force_full) scan_show(0);            /* a full walk is always worth showing */
    db_exec("BEGIN;");
    int changes_before = sqlite3_total_changes(db);
    sync_roots();
    int touched = 0;
    struct scan_stmts s;
    if (scan_stmts_open(&s))
    {
        /* Pass 1: stat-gated walk -- prune, upsert dirs, collect files to parse. */
        for (int c = 0; c < TP_CAT_COUNT; c++)
            for (int i = 0, n = trimpod_folders_root_count(c); i < n; i++)
            {
                const char *r = trimpod_folders_root_path(c, i);
                int id = root_id_for(r);
                if (id > 0) touched += scan_dir(&s, r, NULL, id, c, force_full);
            }
        /* Pass 2: parse them, with the percent; yield so the codec keeps up. */
        for (int i = 0; i < pend.n; i++)
        {
            scan_show(i * 100 / pend.n);
            yield();
            struct pend_item *it = &pend.v[i];
            char dir[MAX_PATH];
            const char *sl = strrchr(it->path, '/');
            snprintf(dir, sizeof dir, "%.*s", (int)(sl ? sl - it->path : 0), it->path);
            if (parse_and_upsert(s.track_ins, it->path, dir, it->root_id,
                                 it->category, it->mtime, it->size))
                touched++;
        }
    }
    /* Hold the screen through commit + write-back (seconds on the card); a pure
     * removal has shown nothing yet. */
    bool changed = sqlite3_total_changes(db) != changes_before;
    if (changed) scan_show(100);
    pend_free();
    scan_stmts_close(&s);
    db_exec("COMMIT;");
    if (changed) db_writeback();
    return touched;
}

/* ---- Shuffle All's track set ------------------------------------------ */

void trimpod_library_music_paths(bool (*cb)(const char *path, void *ctx),
                                 void *ctx)
{
    if (!db || !cb) return;
    /* Title order is what Shuffle Off restores.  The LIMIT is the queue cap, so
     * an over-cap library yields a random sample, not its alphabetical head. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT path FROM (SELECT path, title FROM tracks WHERE category=?"
            " ORDER BY RANDOM() LIMIT ?) ORDER BY title, path;", -1, &st, NULL)
            != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, TP_CAT_MUSIC);
    sqlite3_bind_int(st, 2, TRIMPOD_MAX_FILES_IN_PLAYLIST);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        const char *p = (const char *)sqlite3_column_text(st, 0);
        if (p && !cb(p, ctx))
            break;
    }
    sqlite3_finalize(st);
}

void trimpod_library_artists(int category,
        void (*cb)(const char *, int, void *), void *ctx)
{
    if (!db || !cb) return;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT browse_artist, COUNT(*) FROM tracks WHERE category=?"
            " GROUP BY sort_artist ORDER BY sort_artist;", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, category);
    while (sqlite3_step(st) == SQLITE_ROW)
        cb((const char *)sqlite3_column_text(st, 0), sqlite3_column_int(st, 1), ctx);
    sqlite3_finalize(st);
}

void trimpod_library_albums(int category, const char *ba,
        void (*cb)(const char *, int, void *), void *ctx)
{
    if (!db || !cb) return;
    /* One artist -> chronological (year, then title); ba==NULL -> every album in
     * the library, alphabetical (the flat "Albums" facet). */
    char sql[160];
    snprintf(sql, sizeof sql,
        "SELECT album, MAX(year) FROM tracks WHERE category=?%s"
        " GROUP BY album ORDER BY %s;",
        ba ? " AND browse_artist=?" : "",
        ba ? "MAX(year), album" : "album");
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, category);
    if (ba) sqlite3_bind_text(st, 2, ba, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW)
        cb((const char *)sqlite3_column_text(st, 0), sqlite3_column_int(st, 1), ctx);
    sqlite3_finalize(st);
}

void trimpod_library_tracks(int category, const char *ba, const char *album,
        void (*cb)(const char *title, const char *path, void *ctx), void *ctx)
{
    if (!db || !cb) return;
    /* Deterministic order: the browse queues rows as listed, so a row index is
     * its playlist index.  album==NULL -> the artist's whole library; ba==NULL
     * too -> the "Songs" facet. */
    const char *order = album ? " ORDER BY discnum,tracknum"
                      : ba    ? " ORDER BY path"
                      :         " ORDER BY title, path";
    char sql[256];
    snprintf(sql, sizeof sql,
        "SELECT title, path FROM tracks WHERE category=?%s%s%s;",
        ba ? " AND browse_artist=?" : "",
        album ? " AND album=?" : "",
        order);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    int bi = 1;
    sqlite3_bind_int (st, bi++, category);
    if (ba)    sqlite3_bind_text(st, bi++, ba,    -1, SQLITE_STATIC);
    if (album) sqlite3_bind_text(st, bi++, album, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW)
        cb((const char *)sqlite3_column_text(st, 0),
           (const char *)sqlite3_column_text(st, 1), ctx);
    sqlite3_finalize(st);
}

/* ---- open / migrate / init ------------------------------------------- */

/* Ordered in-place migrations from an older on-disk schema up to SCHEMA_VERSION.
 * `from` is the stored version; each guard brings it one step forward.  Steps
 * are idempotent (CREATE ... IF NOT EXISTS) so a re-run is harmless. */
static bool db_migrate(int from)
{
    if (from < 2)                                /* v2: index the artist-filtered queries */
        if (db_exec("CREATE INDEX IF NOT EXISTS idx_tracks_browse"
                    " ON tracks(category, browse_artist, album, discnum, tracknum);")
                != SQLITE_OK)
            return false;
    if (from < 3)                                /* v3: index the Songs facet (title order) */
        if (db_exec("CREATE INDEX IF NOT EXISTS idx_tracks_title"
                    " ON tracks(category, title, path);")
                != SQLITE_OK)
            return false;
    return true;
}

static bool db_setup(void)
{
    db_exec("PRAGMA foreign_keys=ON;");
    db_exec("PRAGMA journal_mode=TRUNCATE;");   /* single writer; exFAT-safe, no WAL */
    db_exec("PRAGMA synchronous=NORMAL;");
    int ver = user_version();
    if (ver == 0)                                /* fresh */
    {
        if (db_exec(SCHEMA_SQL) != SQLITE_OK) return false;
        set_user_version(SCHEMA_VERSION);
    }
    else if (ver < SCHEMA_VERSION)               /* migrate in place */
    {
        if (!db_migrate(ver)) return false;
        set_user_version(SCHEMA_VERSION);
    }
    else if (ver > SCHEMA_VERSION)               /* newer/foreign db -> rebuild */
        return false;
    return true;
}

void trimpod_library_init(void)
{
    /* Work on a tmpfs copy of the card's index (see db_writeback); fall back to
     * the card itself if the copy can't be made. */
    remove(DB_RAM);                          /* never inherit a stale copy */
    db_in_ram = copy_file(DB_SD, DB_RAM) || !file_exists(DB_SD);
    const char *dbpath = db_in_ram ? DB_RAM : DB_SD;

    if (sqlite3_open(dbpath, &db) != SQLITE_OK || !db_setup())
    {
        /* Unreadable / incompatible: the SD files are truth, so rebuild once. */
        if (db) sqlite3_close(db);
        db = NULL;
        remove(dbpath);
        if (db_in_ram) remove(DB_SD);
        if (sqlite3_open(dbpath, &db) != SQLITE_OK || !db_setup())
        {
            if (db) { sqlite3_close(db); db = NULL; }
            return;                              /* app still runs without the index */
        }
    }
    trimpod_library_reconcile(false);
}

void trimpod_library_close(void)
{
    if (db) { sqlite3_close(db); db = NULL; }
    if (db_in_ram) remove(DB_RAM);
}
