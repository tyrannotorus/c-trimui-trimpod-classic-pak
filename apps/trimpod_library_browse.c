/* Trimpod: tag-based Music Library browse -- see trimpod_library_browse.h.
 *
 * ONE persistent trimpod_page over the SQLite index, navigating levels in place
 * (like the folder browser, not nested blocking screens).  The root-menu facet
 * (the items[] param -> enum trimpod_library_mode) picks the entry level:
 *   Artists -> Albums(artist) ["All Songs" + one row per album] -> Tracks(album)
 *   Albums  -> Tracks(album)          [flat: every album, no "All Songs" row]
 *   Songs   -> (every track)          [one flat, alphabetical track list]
 * A descends a level (or, in Tracks, starts playback at that track and slides to
 * Now Playing); B ascends a level (or leaves at the facet's root level).  On play
 * we stash the navigation position; returning from Now Playing restores the
 * Tracks list on the currently playing song -- so backing out of the WPS lands on
 * the track listing, then tracks back out.  (Rescan Library lives in Settings ->
 * Library.)
 *
 * The enumerators (trimpod_library_artists/_albums/_tracks) hand each row to a
 * callback; we collect them into a growable string list (no fixed cap) that
 * backs a gui_synclist name callback, mirroring the folder picker's listing. */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>   /* intptr_t (the facet param) */
#include <stdlib.h>   /* realloc/free */
#include <string.h>   /* strdup */

#include "kernel.h"    /* HZ (splash timeouts) */
#include "lang.h"
#include "settings.h"
#include "splash.h"
#include "action.h"
#include "gui/list.h"
#include "icon.h"
#include "playlist.h"
#include "misc.h"      /* warn_on_pl_erase, push/pop_current_activity */
#include "pathfuncs.h" /* path_basename (untagged-track fallback) */
#include "root_menu.h" /* GO_TO_WPS / GO_TO_ROOT */
#include "trimpod_page.h"
#include "trimpod_transition.h"
#include "trimpod_folders.h"        /* TP_CAT_MUSIC */
#include "trimpod_library.h"
#include "trimpod_library_browse.h"
#include "trimpod_playlists.h"      /* Hold-A = Add to Playlist */
#include "filetypes.h"              /* FILE_ATTR_AUDIO */

/* ---- growable string list (rows collected from the enumerators) ------- */

struct slist { char **v; int n, cap; };

static void slist_add(struct slist *s, const char *str)
{
    if (s->n == s->cap)
    {
        int ncap = s->cap ? s->cap * 2 : 32;
        char **nv = realloc(s->v, ncap * sizeof *nv);
        if (!nv) return;                 /* best effort: drop this row, keep prior */
        s->v = nv; s->cap = ncap;
    }
    char *dup = strdup(str ? str : "");
    if (dup) s->v[s->n++] = dup;
}
static void slist_free(struct slist *s)
{
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v); s->v = NULL; s->n = s->cap = 0;
}

static const char *disp_artist(const char *a)
{
    return (a && a[0]) ? a : (const char *)str(LANG_TRIMPOD_UNKNOWN_ARTIST);
}

/* ---- enumerator callbacks (append one row per DB row) ----------------- */

static void artist_cb(const char *browse_artist, int track_count, void *ctx)
{
    (void)track_count;
    slist_add((struct slist *)ctx, browse_artist);   /* "" -> Unknown at display */
}
static void album_cb(const char *album, int year, void *ctx)
{
    (void)year;
    if (album && album[0])               /* skip untagged; reachable via All Songs */
        slist_add((struct slist *)ctx, album);
}
/* Tracks level fills two index-aligned lists: the display string (title, or the
 * filename when untagged) and the matching file path -- so a tapped row plays or
 * adds without re-querying the DB (its index is its playlist index). */
struct track_fill { struct slist *rows, *paths; };
static void track_cb(const char *title, const char *path, void *ctx)
{
    struct track_fill *t = ctx;
    if (title && title[0])               /* tagged: show the title */
        slist_add(t->rows, title);
    else                                 /* untagged: fall back to the filename */
    {
        const char *leaf = "";
        if (path && path[0])
            path_basename(path, &leaf);
        slist_add(t->rows, leaf);
    }
    slist_add(t->paths, path ? path : "");
}

/* Hold-A on a whole album/artist: collect their file paths for the context menu. */
static void path_collect_cb(const char *title, const char *path, void *ctx)
{
    (void)title;
    if (path && path[0])
        slist_add((struct slist *)ctx, path);
}

/* ---- the page: Artists / Albums / Tracks, one gui_synclist ------------ */

enum lib_level { LVL_ARTISTS = 0, LVL_ALBUMS, LVL_TRACKS, LVL_COUNT };

struct lib_page
{
    struct trimpod_page base;
    struct gui_synclist lists;
    enum lib_level level;        /* the level currently shown */
    enum lib_level root_level;   /* B at this level leaves to the root (per mode) */
    char  *artist;               /* browse_artist filter; NULL = no artist (flat
                                  * Albums/Songs), "" = the Unknown artist. owned */
    char  *album;                /* album filter (owned); NULL/"" when all_songs   */
    bool   all_songs;            /* Tracks shows the album wildcard (whole artist,
                                  * or -- with artist NULL -- the whole library) */
    struct slist rows;           /* display strings for the current level */
    struct slist paths;          /* Tracks level: file path per row (play/add
                                  * without re-querying); empty at other levels */
    int    sel_at[LVL_COUNT];    /* per-level highlight, restored on ascent */
    int    result;               /* GO_TO_WPS once a track played, else GO_TO_ROOT */
};

/* Stashed position for the Now Playing round-trip: set when a track plays,
 * consumed on the next entry (only if the same facet re-enters) so B out of the
 * WPS reopens the Tracks list. */
static struct {
    bool  pending;
    enum lib_level root_level;   /* which facet played (matched on re-entry) */
    char *artist;                /* NULL = flat Albums/Songs */
    char *album;                 /* NULL == All Songs */
    int   sel_at[LVL_COUNT];
} lib_resume;

/* The synthetic "All Songs" row exists only when Albums is scoped to one artist
 * (reached from Artists); the flat Albums facet has no artist and no such row. */
static bool albums_has_all_songs(const struct lib_page *p)
{
    return p->level == LVL_ALBUMS && p->artist != NULL;
}

static void set_str(char **dst, const char *s)
{
    free(*dst);
    *dst = strdup(s ? s : "");
}

/* an artist-scoped Albums list prepends the synthetic "All Songs" row */
static int lib_nb_items(const struct lib_page *p)
{
    return p->rows.n + (albums_has_all_songs(p) ? 1 : 0);
}
static const char *lib_get_name(int sel, void *data, char *buf, size_t len)
{
    (void)buf; (void)len;
    struct lib_page *p = data;
    if (albums_has_all_songs(p))
    {
        if (sel == 0)
            return (const char *)str(LANG_TRIMPOD_ALL_SONGS);
        sel--;
    }
    if (sel < 0 || sel >= p->rows.n)
        return "";
    return p->level == LVL_ARTISTS ? disp_artist(p->rows.v[sel]) : p->rows.v[sel];
}

/* (Re)fill rows for the current level, set the title, and restore this level's
 * highlight.  Called on entry and inside every slide's off-screen render pass. */
static void lib_load(struct lib_page *p)
{
    slist_free(&p->rows);
    slist_free(&p->paths);
    const char *title;
    switch (p->level)
    {
        case LVL_ALBUMS:
            trimpod_library_albums(TP_CAT_MUSIC, p->artist, album_cb, &p->rows);
            title = p->artist ? disp_artist(p->artist)          /* one artist */
                              : (const char *)str(LANG_TRIMPOD_ALBUMS); /* flat */
            break;
        case LVL_TRACKS:
        {
            struct track_fill tf = { &p->rows, &p->paths };
            trimpod_library_tracks(TP_CAT_MUSIC, p->artist,
                                   p->all_songs ? NULL : p->album, track_cb, &tf);
            title = !p->all_songs ? p->album                    /* an album */
                  : p->artist     ? (const char *)str(LANG_TRIMPOD_ALL_SONGS)
                  :                 (const char *)str(LANG_TRIMPOD_SONGS); /* whole library */
            break;
        }
        case LVL_ARTISTS:
        default:
            trimpod_library_artists(TP_CAT_MUSIC, artist_cb, &p->rows);
            title = (const char *)str(LANG_TRIMPOD_ARTISTS);
            break;
    }
    gui_synclist_set_title(&p->lists, (char *)title, Icon_Audio);
    int n = lib_nb_items(p);
    gui_synclist_set_nb_items(&p->lists, n);
    int sel = p->sel_at[p->level];
    if (sel < 0 || sel >= n) sel = 0;
    gui_synclist_select_item(&p->lists, sel);
}

/* Slide-in plumbing: reload + draw the new level inside the tween's off-screen
 * render pass (as tree.c/pick_slide do), so it slides over the old content. */
static struct lib_page *s_slide_p;
static void lib_slide_render(void *ctx)
{
    (void)ctx;
    lib_load(s_slide_p);
    gui_synclist_draw(&s_slide_p->lists);
}
static void lib_slide(struct lib_page *p, enum trimpod_transition_dir dir)
{
    s_slide_p = p;
    trimpod_transition_animate(dir, lib_slide_render, NULL);
}

static void lib_draw(struct trimpod_page *self)
{
    gui_synclist_draw(&((struct lib_page *)self)->lists);
}
static int lib_poll(struct trimpod_page *self, int timeout)
{
    int action;
    list_do_action(self->context, timeout, &((struct lib_page *)self)->lists, &action);
    return action;
}

/* Stash the position so the return from Now Playing reopens `artist`/`album`'s
 * Tracks list at row `sel` (album NULL == All Songs). */
static void lib_remember(struct lib_page *p, int sel,
                         const char *artist, const char *album)
{
    free(lib_resume.artist);
    free(lib_resume.album);
    lib_resume.root_level = p->root_level;
    lib_resume.artist = artist ? strdup(artist) : NULL;
    lib_resume.album  = album ? strdup(album) : NULL;
    memcpy(lib_resume.sel_at, p->sel_at, sizeof lib_resume.sel_at);
    lib_resume.sel_at[LVL_TRACKS] = sel;
    lib_resume.pending = true;
}

/* Stage `paths` as the queue (trimpod_library_stage_paths: bulk m3u write +
 * index scan) and start it at row `sel`; row order == playlist index.  Past the
 * engine cap, lead with `sel` and start at 0 (as ft_build_playlist does) so it
 * is always the one that plays. */
static bool lib_play(struct lib_page *p, char **paths, int n, int sel,
                     const char *artist, const char *album)
{
    if (!warn_on_pl_erase())
        return false;
    bool exceeds = n > TRIMPOD_MAX_FILES_IN_PLAYLIST;
    if (trimpod_library_stage_paths(paths, n, exceeds ? sel : 0) <= 0)
    {
        splash(HZ, ID2P(LANG_TRIMPOD_NO_MUSIC));
        return false;
    }
    global_settings.playlist_shuffle = false;      /* a new queue: file order */
    playlist_start(exceeds ? 0 : sel, 0, 0);
    lib_remember(p, sel, artist, album);
    return true;
}

/* Tap on the Tracks list: p->paths is index-aligned with the rows (filled when
 * the level loaded), so no DB query runs here.  The track already playing is
 * shown, not restarted. */
static bool play_track(struct lib_page *p, int sel)
{
    if (sel < 0 || sel >= p->paths.n)
        return false;
    const char *album = p->all_songs ? NULL : (p->album ? p->album : "");
    if (trimpod_resume_if_current(p->paths.v[sel]))
    {
        lib_remember(p, sel, p->artist, album);
        return true;
    }
    return lib_play(p, p->paths.v, p->paths.n, sel, p->artist, album);
}

/* Hold-A on an artist/album row: the context menu for `title` over its track
 * set; on Play the set becomes the queue (lib_play).  Frees `paths`. */
static bool ctx_play_set(struct lib_page *p, const char *title,
                         struct slist *paths, const char *artist, const char *album)
{
    bool started = trimpod_music_context_tracks(title, paths->v, paths->n) &&
                   lib_play(p, paths->v, paths->n, 0, artist, album);
    slist_free(paths);
    return started;
}

static enum trimpod_page_result lib_on_action(struct trimpod_page *self, int action)
{
    struct lib_page *p = (struct lib_page *)self;
    int sel = gui_synclist_get_sel_pos(&p->lists);

    if (action == ACTION_STD_CANCEL)                /* B: up a level, or leave */
    {
        if (p->level == p->root_level)              /* at the facet root -> leave */
        {
            trimpod_transition_arm_back();
            return TRIMPOD_PAGE_DONE;
        }
        p->level--;
        lib_slide(p, TRIMPOD_TRANS_BACK);
        return TRIMPOD_PAGE_STAY;
    }

    if (action == ACTION_STD_OK)                    /* A: descend, or play */
    {
        switch (p->level)
        {
            case LVL_ARTISTS:
                if (sel < 0 || sel >= p->rows.n)
                    break;
                p->sel_at[LVL_ARTISTS] = sel;
                set_str(&p->artist, p->rows.v[sel]);
                p->level = LVL_ALBUMS;
                p->sel_at[LVL_ALBUMS] = 0;
                lib_slide(p, TRIMPOD_TRANS_FORWARD);
                break;

            case LVL_ALBUMS:
            {
                bool has_all = albums_has_all_songs(p);
                bool all = has_all && sel == 0;     /* row 0 == "All Songs" */
                int ai = sel - (has_all ? 1 : 0);
                if (!all && (ai < 0 || ai >= p->rows.n))
                    break;
                p->sel_at[LVL_ALBUMS] = sel;
                p->all_songs = all;
                set_str(&p->album, all ? "" : p->rows.v[ai]);
                p->level = LVL_TRACKS;
                p->sel_at[LVL_TRACKS] = 0;
                lib_slide(p, TRIMPOD_TRANS_FORWARD);
                break;
            }

            case LVL_TRACKS:
                if (sel >= 0 && sel < p->rows.n && play_track(p, sel))
                {
                    p->result = GO_TO_WPS;          /* no arm_back: WPS slides in */
                    return TRIMPOD_PAGE_DONE;
                }
                break;

            default:
                break;
        }
    }

    if (action == ACTION_STD_CONTEXT)               /* Hold A: music context menu */
    {
        switch (p->level)
        {
            case LVL_ARTISTS:                       /* whole artist */
            {
                if (sel < 0 || sel >= p->rows.n)
                    break;
                const char *artist = p->rows.v[sel];
                struct slist paths = {0};
                trimpod_library_tracks(TP_CAT_MUSIC, artist, NULL,
                                       path_collect_cb, &paths);
                p->sel_at[LVL_ARTISTS] = sel;       /* B out of the WPS lands here */
                p->sel_at[LVL_ALBUMS] = 0;
                if (ctx_play_set(p, disp_artist(artist), &paths, artist, NULL))
                {
                    p->result = GO_TO_WPS;
                    return TRIMPOD_PAGE_DONE;
                }
                break;
            }
            case LVL_ALBUMS:                        /* whole album (or All Songs) */
            {
                bool has_all = albums_has_all_songs(p);
                bool all = has_all && sel == 0;
                int ai = sel - (has_all ? 1 : 0);
                if (!all && (ai < 0 || ai >= p->rows.n))
                    break;
                const char *album = all ? NULL : p->rows.v[ai];
                struct slist paths = {0};
                trimpod_library_tracks(TP_CAT_MUSIC, p->artist, album,
                                       path_collect_cb, &paths);
                p->sel_at[LVL_ALBUMS] = sel;        /* B out of the WPS lands here */
                if (ctx_play_set(p, all ? (const char *)str(LANG_TRIMPOD_ALL_SONGS)
                                        : album, &paths, p->artist, album))
                {
                    p->result = GO_TO_WPS;
                    return TRIMPOD_PAGE_DONE;
                }
                break;
            }
            case LVL_TRACKS:                        /* the highlighted track */
            {
                if (sel < 0 || sel >= p->paths.n || !p->paths.v[sel][0])
                    break;
                if (trimpod_music_context(p->rows.v[sel], p->paths.v[sel],
                                          FILE_ATTR_AUDIO) && play_track(p, sel))
                {
                    p->result = GO_TO_WPS;          /* no arm_back: WPS slides in */
                    return TRIMPOD_PAGE_DONE;
                }
                break;
            }
            default:
                break;
        }
        return TRIMPOD_PAGE_STAY;
    }
    return TRIMPOD_PAGE_STAY;
}

static const struct trimpod_page_vtable lib_vtable =
{
    .legend = NULL, .draw = lib_draw,               /* no button legend */
    .poll = lib_poll, .on_action = lib_on_action,
};

/* A (descend/play), B (up/leave), Hold-A (context menu); scrolling in poll */
static const int lib_allowed[] =
    { ACTION_STD_OK, ACTION_STD_CANCEL, ACTION_STD_CONTEXT, -1 };

/* trimpod_library_browse -- the root-menu Artists / Albums / Songs entries (the
 * facet is the items[] param).  A re-entrant, root-dispatched page: a fresh open
 * starts at the facet's root level; returning from Now Playing restores the
 * Tracks list on the currently playing song (only when the same facet re-opens). */
int trimpod_library_browse(void *param)
{
    enum trimpod_library_mode mode = (enum trimpod_library_mode)(intptr_t)param;

    struct lib_page p =
    {
        /* re-entrant page: honor a pending back on enter (return from Now
         * Playing) and don't auto-arm a back on exit -- the next screen slides
         * itself in.  Matches folder_browse. */
        .base = { .vt = &lib_vtable, .context = CONTEXT_LIST,
                  .allowed = lib_allowed, .no_arm_back = true,
                  .enter_honor_back = true },
        .result = GO_TO_ROOT,
    };
    switch (mode)                                    /* facet -> starting level */
    {
        case TP_LIB_ALBUMS:                          /* flat Albums -> Tracks */
            p.level = p.root_level = LVL_ALBUMS;
            break;
        case TP_LIB_SONGS:                           /* every song */
            p.level = p.root_level = LVL_TRACKS;
            p.all_songs = true;                      /* artist + album wildcard */
            break;
        case TP_LIB_ARTISTS:
        default:
            p.level = p.root_level = LVL_ARTISTS;
            break;
    }

    /* Returning from Now Playing into the same facet: reopen the Tracks list. */
    if (lib_resume.pending && lib_resume.root_level == p.root_level)
    {
        lib_resume.pending = false;
        p.level = LVL_TRACKS;
        if (lib_resume.artist)
            set_str(&p.artist, lib_resume.artist);
        p.all_songs = (lib_resume.album == NULL);
        if (lib_resume.album)
            set_str(&p.album, lib_resume.album);
        memcpy(p.sel_at, lib_resume.sel_at, sizeof p.sel_at);
        if (playlist_amount() > 0)                   /* land on the playing song */
        {
            int idx = playlist_get_display_index() - 1;
            if (idx >= 0)
                p.sel_at[LVL_TRACKS] = idx;
        }
    }
    else if (lib_resume.pending)                     /* a different facet -> drop it */
        lib_resume.pending = false;

    gui_synclist_init(&p.lists, lib_get_name, &p, false, 1, NULL);
    lib_load(&p);

    /* A restored Tracks list that came up empty (library changed since play)
     * falls back to this facet's root level; an empty root list means there is
     * nothing to browse. */
    if (p.level != p.root_level && p.rows.n == 0)
    {
        p.level = p.root_level;
        lib_load(&p);
    }
    if (p.rows.n == 0)
    {
        /* Never rendered -- only a splash over the menu we came from.  Suppress
         * the menu's re-entry slide so it doesn't slide back to itself. */
        splash(HZ, ID2P(LANG_TRIMPOD_NO_MUSIC));
        slist_free(&p.rows);
        slist_free(&p.paths);
        free(p.artist);
        free(p.album);
        trimpod_transition_suppress_next();
        return GO_TO_ROOT;
    }

    push_current_activity(ACTIVITY_FILEBROWSER);
    trimpod_page_run(&p.base);
    pop_current_activity();

    slist_free(&p.rows);
    slist_free(&p.paths);
    free(p.artist);
    free(p.album);
    return p.base.home ? GO_TO_ROOT : p.result;
}
