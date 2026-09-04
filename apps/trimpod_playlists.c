/***************************************************************************
 * Trimpod: the Playlists screen.
 *
 * A trimpod_page that lists the playlists in the catalog directory plus a
 * "+ Add Playlist" row, mirroring the Music Folders settings page:
 *   A on "+ Add Playlist" -> the on-screen keyboard; a confirmed name creates
 *     an empty .m3u8 and the list refreshes.
 *   A on a playlist row    -> its track listing (the playlist viewer); A on a
 *     track there starts playback from it.
 *   Hold A on a playlist   -> a Play/Shuffle/Rename/Edit/Delete context menu;
 *     Play/Shuffle start playback at once (track list sits behind Now Playing).
 *   B                      -> leave, back to the main menu.
 *
 * The run loop, header and key whitelist are owned by trimpod_page_run; this
 * page only describes the list and what A/B/Hold-A do.
 ****************************************************************************/
#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>   /* qsort */
#include <string.h>
#include "string-extra.h"
#include "strnatcmp.h"   /* natural alphabetical sort (2 before 10) */
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "settings.h"
#include "lang.h"
#include "splash.h"
#include "action.h"
#include "gui/list.h"
#include "icon.h"
#include "misc.h"        /* push/pop_current_activity, ACTIVITY_PLAYLISTBROWSER */
#include "root_menu.h"
#include "playlist_catalog.h"
#include "playlist.h"          /* playlist_load/get_track_info/delete/save (Edit) */
#include "audio.h"             /* trimpod_resume_if_current */
#include "metadata.h"
#include "filetypes.h"         /* FILE_ATTR_AUDIO (track-set inserts) */
#include "kernel.h"            /* current_tick (shuffle seed) */
#include "system.h"            /* ARRAYLEN */
#include "scratch_buf.h"       /* scratch_buffer_get -- playlist_load buffers */
#include "trimpod_page.h"
#include "trimpod_transition.h"  /* arm the back slide on the B-exit only */
#include "trimpod_ui.h"        /* trimpod_confirm */
#include "trimpod_keyboard.h"  /* the on-screen keyboard */
#include "trimpod_playlists.h"

#define PL_MAX        256
#define PL_NAMEBUF    (PL_MAX * 64)   /* packed NUL-terminated names */

/* one playlists page at a time -> the listing lives in file-scope statics */
static char pl_dir[MAX_PATH];         /* the catalog directory */
static char pl_namebuf[PL_NAMEBUF];   /* packed NUL-terminated playlist names */
static int  pl_off[PL_MAX];           /* offset of each name in pl_namebuf */
static int  pl_count;

/* Remembered selection for the standalone screen, so re-entering it (incl.
 * returning from Now Playing) re-highlights the playlist you were on.  Empty
 * string = the "+ Add Playlist" row.  Session-only. */
static char pl_last_sel[MAX_PATH];
static bool pl_have_last;

/* A pending playlist request handed to the root dispatch, routed via
 * GO_TO_PLAYLIST_VIEWER (playlist_view() in root_menu.c):
 *   view (tap A)          -> open the playlist's track listing in the viewer
 *     (trimpod_playlists_take_pending_view);
 *   play/shuffle (Hold A) -> trimpod_playlists_start_pending() begins playback
 *     so the track list sits *behind* Now Playing (backing out of the WPS
 *     reveals it).  Filename only -- the catalog dir is the stable pl_dir. */
static char pl_pending_file[MAX_PATH];
static bool pl_pending_active;
static bool pl_pending_shuffle;
static bool pl_pending_view;

static bool is_playlist(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8"));
}

/* True if the playlist file at `path` lists no tracks: empty, comments-only
 * ('#' lines), or unreadable.  Tolerates a leading UTF-8 BOM. */
static bool pl_file_is_empty(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return true;

    static const char bom[] = "\xef\xbb\xbf";
    char buf[512];
    ssize_t n;
    int pos = 0;
    bool line_start = true, empty = true;
    while (empty && (n = read(fd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; i++, pos++)
        {
            char c = buf[i];
            if (pos < 3 && c == bom[pos])
                continue;
            if (c == '\n' || c == '\r')
                line_start = true;
            else if (line_start && c != ' ' && c != '\t')
            {
                if (c != '#')
                {
                    empty = false;
                    break;
                }
                line_start = false;         /* comment line: skip to EOL */
            }
        }
    }
    close(fd);
    return empty;
}

/* qsort comparator: natural alphabetical order of the names at two offsets */
static int pl_off_cmp(const void *a, const void *b)
{
    return strnatcasecmp(pl_namebuf + *(const int *)a,
                         pl_namebuf + *(const int *)b);
}

/* (Re)read the catalog directory into the packed name table, sorted. */
static void pl_load(void)
{
    pl_count = 0;
    int used = 0;
    catalog_get_directory(pl_dir, sizeof(pl_dir));   /* also creates it if absent */
    DIR *d = opendir(pl_dir);
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (e->d_name[0] == '.' || !is_playlist(e->d_name))
                continue;
            int len = strlen(e->d_name) + 1;
            if (pl_count >= PL_MAX || used + len > (int)sizeof(pl_namebuf))
                break;                               /* listing full (rare) */
            pl_off[pl_count++] = used;
            memcpy(pl_namebuf + used, e->d_name, len);
            used += len;
        }
        closedir(d);
    }
    qsort(pl_off, pl_count, sizeof pl_off[0], pl_off_cmp);
}

struct playlists_page
{
    struct trimpod_page base;
    struct gui_synclist lists;
    int result;                       /* GO_TO_* handed back to the root dispatch */

    /* "pick" mode (Hold-A = Add to Playlist): the chosen or newly created
     * playlist receives the selection via the stock Rockbox catalog_insert_into().
     * Either a single (pick_sel, pick_attr) -- a file appended, a directory
     * expanded into its tracks -- OR, when pick_paths is set, an explicit set of
     * track paths (a library album/artist has no single path to expand).
     * Normal mode (pick=false) is the standalone Playlists screen. */
    bool         pick;
    const char  *pick_sel;
    int          pick_attr;
    char       **pick_paths;          /* non-NULL -> add these tracks instead */
    int          pick_npaths;
};

/* Copy playlist row `sel`'s filename with its .m3u8/.m3u extension stripped --
 * the display name, reused for the list rows and the delete confirmation. */
static void pl_display_name(int sel, char *out, size_t out_len)
{
    strlcpy(out, pl_namebuf + pl_off[sel], out_len);
    char *dot = strrchr(out, '.');
    if (dot && (!strcasecmp(dot, ".m3u8") || !strcasecmp(dot, ".m3u")))
        *dot = '\0';
}

/* rows: [0, pl_count) playlists, then the "+ Add Playlist" row at pl_count */
static const char *pl_get_name(int sel, void *data, char *buf, size_t buf_len)
{
    (void)data;
    if (sel == pl_count)
        return str(LANG_TRIMPOD_ADD_PLAYLIST_ROW);
    if (sel < 0 || sel > pl_count)
        return "";
    pl_display_name(sel, buf, buf_len);   /* no extension shown */
    return buf;
}

static void playlists_draw(struct trimpod_page *self)
{
    gui_synclist_draw(&((struct playlists_page *)self)->lists);
}

static int playlists_poll(struct trimpod_page *self, int timeout)
{
    int action;
    list_do_action(self->context, timeout,
                   &((struct playlists_page *)self)->lists, &action);
    return action;
}

/* Name a new playlist with the on-screen keyboard and create an empty .m3u8.
 * The keyboard yields a bare name (its glyph set can't produce path separators),
 * so the file is just <catalog>/<name>.m3u8.  Cancel/empty leaves the list as-is. */
static void add_playlist(struct playlists_page *p)
{
    char name[MAX_PATH];
    name[0] = '\0';
    if (trimpod_kbd_input(name, sizeof(name)) != 0 || name[0] == '\0')
        return;

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s.m3u8", pl_dir, name);

    if (file_exists(path) && !trimpod_confirm(str(LANG_TRIMPOD_OVERWRITE_PLAYLIST_Q), name))
        return;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
        close(fd);
    else
        splash(HZ, ID2P(LANG_TRIMPOD_PLAYLIST_CREATE_FAILED));
    pl_load();
    gui_synclist_set_nb_items(&p->lists, pl_count + 1);
}

/* ---- Edit Playlist: track list, Y removes the highlighted track --------- *
 * Taps the stock playlist API (playlist_load / playlist_delete / playlist_save)
 * on the separate on-disk playlist_info, so it never disturbs playback -- the
 * same path the playlist viewer uses.  Deletions apply in memory and are written
 * back once on exit. */

struct editor_page
{
    struct trimpod_page   base;
    struct gui_synclist   lists;
    struct playlist_info *pl;
    char  path[MAX_PATH];        /* full .m3u8 path, for the save-on-exit */
    bool  dirty;
};

/* track display name = the file's basename */
static void editor_track_name(struct editor_page *p, int sel, char *out, size_t len)
{
    struct playlist_track_info info;
    if (p->pl && playlist_get_track_info(p->pl, sel, &info) == 0)
    {
        const char *base = strrchr(info.filename, PATH_SEPCH);
        strlcpy(out, base ? base + 1 : info.filename, len);
    }
    else
        out[0] = '\0';
}

static const char *editor_get_name(int sel, void *data, char *buf, size_t buf_len)
{
    editor_track_name((struct editor_page *)data, sel, buf, buf_len);
    return buf;
}

/* No legend: B=back is the universal convention and Remove lives in the Hold-A
 * context submenu. */

static void editor_draw(struct trimpod_page *self)
{
    gui_synclist_draw(&((struct editor_page *)self)->lists);
}

static int editor_poll(struct trimpod_page *self, int timeout)
{
    int action;
    list_do_action(self->context, timeout,
                   &((struct editor_page *)self)->lists, &action);
    return action;
}

static enum trimpod_page_result editor_on_action(struct trimpod_page *self, int action)
{
    struct editor_page *p = (struct editor_page *)self;
    int sel = gui_synclist_get_sel_pos(&p->lists);
    int n   = p->pl ? playlist_amount_ex(p->pl) : 0;

    if (action == ACTION_STD_CANCEL)             /* B: leave (saved on exit) */
        return TRIMPOD_PAGE_DONE;

    if (action == ACTION_STD_CONTEXT && sel >= 0 && sel < n)   /* Hold A: context */
    {
        char name[MAX_PATH];
        editor_track_name(p, sel, name, sizeof(name));
        static const char *const opts[] = { ID2P(LANG_TRIMPOD_REMOVE_FROM_PLAYLIST) };
        if (trimpod_context_menu(name, opts, 1) == 0 &&
            trimpod_confirm(str(LANG_TRIMPOD_REMOVE_FROM_PLAYLIST_Q), name) &&
            playlist_delete(p->pl, sel) == 0)
        {
            p->dirty = true;
            int left = playlist_amount_ex(p->pl);
            gui_synclist_set_nb_items(&p->lists, left);
            if (sel >= left && left > 0)
                gui_synclist_select_item(&p->lists, left - 1);
        }
    }
    return TRIMPOD_PAGE_STAY;
}

static const struct trimpod_page_vtable editor_vtable =
{
    .legend    = NULL,
    .draw      = editor_draw,
    .poll      = editor_poll,
    .on_action = editor_on_action,
};

/* only Hold-A (context: remove) and B (back) act; scrolling is consumed in poll */
static const int editor_allowed[] = { ACTION_STD_CONTEXT, ACTION_STD_CANCEL, -1 };

/* Edit playlist `sel`: load it, let the user remove tracks, save if changed. */
static void edit_playlist(int sel)
{
    size_t bufsz;
    char *buf = scratch_buffer_get(&bufsz);
    if (!buf || bufsz < 8192)
    {
        splash(HZ, ID2P(LANG_TRIMPOD_PLAYLIST_EDIT_FAILED));
        return;
    }

    struct editor_page p =
    {
        .base = { .vt = &editor_vtable, .context = CONTEXT_LIST,
                  .allowed = editor_allowed },
    };
    path_append(p.path, pl_dir, pl_namebuf + pl_off[sel], sizeof(p.path));

    int index_sz = (int)playlist_get_index_bufsz(bufsz - 4096);
    p.pl = playlist_load(pl_dir, pl_namebuf + pl_off[sel],
                         buf, index_sz, buf + index_sz, (int)(bufsz - index_sz));
    if (!p.pl)
    {
        splash(HZ, ID2P(LANG_TRIMPOD_PLAYLIST_LOAD_FAILED));
        return;
    }

    char title[MAX_PATH];
    pl_display_name(sel, title, sizeof(title));
    gui_synclist_init(&p.lists, editor_get_name, &p, false, 1, NULL);
    gui_synclist_set_title(&p.lists, title, Icon_Playlist);
    gui_synclist_set_nb_items(&p.lists, playlist_amount_ex(p.pl));
    gui_synclist_select_item(&p.lists, 0);

    push_current_activity(ACTIVITY_PLAYLISTBROWSER);
    trimpod_page_run(&p.base);
    pop_current_activity();

    if (p.dirty)
    {
        if (playlist_amount_ex(p.pl) > 0)
            playlist_save(p.pl, p.path);
        else
        {   /* emptied: playlist_save refuses 0 tracks -- truncate instead */
            int fd = open(p.path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0)
                close(fd);
        }
    }
    playlist_close(p.pl);
}

/* ---- the live play queue -------------------------------------------------
 * Tracks go into the current playlist at `position`: PLAYLIST_INSERT (Play
 * Next: after the current track, successive adds in order), PLAYLIST_INSERT_LAST
 * (Add to Play Queue: the end) or PLAYLIST_REPLACE (a new queue).  With no
 * queue the selection becomes one and starts playing.  Inserts are silent
 * (progress=false): the user stays on the current screen, so the "Inserted N"
 * modal would just flash over it. */
struct queue_op { struct playlist_insert_context ctx; bool fresh; int pos; };

/* The saved queue is only loaded into RAM by Resume Playback, so until then
 * playlist_amount() is 0 although a queue exists; judge by the resume point. */
static bool queue_exists(void)
{
    return playlist_amount() > 0 || global_status.resume_index != -1;
}

bool trimpod_resume_if_current(const char *path)
{
    int status = audio_status();
    struct mp3entry *id3 = audio_current_track();
    if (!(status & AUDIO_STATUS_PLAY) || !id3 || strcmp(id3->path, path) != 0)
        return false;
    if (status & AUDIO_STATUS_PAUSE)
        audio_resume();
    return true;
}

/* False when declined or the control file is unavailable: nothing to close. */
static bool queue_open(struct queue_op *q, int position)
{
    memset(q, 0, sizeof *q);
    bool replace = position == PLAYLIST_REPLACE;
    if (replace && !warn_on_pl_erase())
        return false;
    q->fresh = replace || !queue_exists();
    if (!q->fresh && playlist_amount() <= 0 && playlist_resume() == -1)
        q->fresh = true;                           /* unloadable: start over */
    if (q->fresh && playlist_create(NULL, NULL) == -1)
        return false;
    q->pos = replace ? PLAYLIST_INSERT_LAST : position;   /* new queue: appends */
    if (playlist_insert_context_create(playlist_get_current(), &q->ctx,
                                       q->pos, false, false) < 0)
    {
        playlist_insert_context_release(&q->ctx);  /* a failed create still locks */
        return false;
    }
    return true;
}

/* A queue built from nothing starts playing; returns whether it did. */
static bool queue_close(struct queue_op *q)
{
    playlist_insert_context_release(&q->ctx);
    if (!q->fresh || playlist_amount() <= 0)
        return false;
    global_settings.playlist_shuffle = false;      /* a new queue: file order */
    playlist_start(0, 0, 0);
    return true;
}

/* tracksearch callback: the first audio file ends the walk */
static int first_audio_cb(char *name, void *found)
{
    (void)name;
    *(bool *)found = true;
    return -1;
}

bool trimpod_queue_add(const char *path, int attr, int position)
{
    struct queue_op q;
    bool replace = position == PLAYLIST_REPLACE;
    if ((attr & ATTR_DIRECTORY) && (replace || !queue_exists()))
    {
        /* About to build a queue from it: a folder with no audio anywhere
         * under it is a no-op, not a new empty queue. */
        bool found = false;
        playlist_directory_tracksearch(path, true, first_audio_cb, &found);
        if (!found)
        {
            if (replace)
                splash(HZ, ID2P(LANG_TRIMPOD_NO_MUSIC));
            return false;
        }
    }
    if (!queue_open(&q, position))
        return false;
    if (attr & ATTR_DIRECTORY)
        playlist_insert_directory(NULL, path, q.pos, false, true, &q.ctx);
    else
        playlist_insert_context_add(&q.ctx, path);
    return queue_close(&q);
}

static void queue_add_tracks(char **paths, int count, int position)
{
    struct queue_op q;
    if (!queue_open(&q, position))
        return;
    for (int i = 0; i < count; i++)
    {
        yield();                                   /* keep the codec fed */
        if (playlist_insert_context_add(&q.ctx, paths[i]) < 0)
            break;
    }
    queue_close(&q);
}

static void queue_add_playlist(const char *path)
{
    struct queue_op q;
    if (!queue_open(&q, false))
        return;
    playlist_entries_iterate(path, &q.ctx, NULL);
    queue_close(&q);
}

/* The per-playlist Hold-A context menu, titled with the playlist's name.
 * trimpod_context_menu returns the row index or -1 on cancel. */
enum { PL_ACT_PLAY = 0, PL_ACT_SHUFFLE, PL_ACT_QUEUE,
       PL_ACT_RENAME, PL_ACT_EDIT, PL_ACT_DELETE };
static const char *const playlist_action_opts[] =
    { ID2P(LANG_PLAY), ID2P(LANG_SHUFFLE), ID2P(LANG_TRIMPOD_ADD_TO_PLAY_QUEUE),
      ID2P(LANG_RENAME), ID2P(LANG_EDIT), ID2P(LANG_DELETE) };

/* Open the context menu for playlist `sel`; returns TRIMPOD_PAGE_DONE when a
 * chosen action leaves the page (play -> Now Playing), else STAY. */
static enum trimpod_page_result playlist_action(struct playlists_page *p, int sel)
{
    char path[MAX_PATH], name[MAX_PATH];
    path_append(path, pl_dir, pl_namebuf + pl_off[sel], sizeof(path));
    pl_display_name(sel, name, sizeof(name));

    int act = trimpod_context_menu(name, playlist_action_opts,
                                   ARRAYLEN(playlist_action_opts));
    switch (act)
    {
        case PL_ACT_PLAY:
        case PL_ACT_SHUFFLE:
        {
            /* Record the pick and hand off to the root dispatch: playback starts
             * immediately (from the top, or shuffled) and slides to Now Playing,
             * with the playlist's file list shown *behind* it -- backing out of
             * the WPS reveals it.  Routed via GO_TO_PLAYLIST_VIEWER; the play
             * happens in trimpod_playlists_start_pending() (root_menu.c). */
            if (!warn_on_pl_erase())            /* about to replace the playlist */
                break;                          /* declined: stay on the list */
            strlcpy(pl_pending_file, pl_namebuf + pl_off[sel], sizeof pl_pending_file);
            pl_pending_shuffle = (act == PL_ACT_SHUFFLE);
            pl_pending_active = true;
            p->result = GO_TO_PLAYLIST_VIEWER;
            return TRIMPOD_PAGE_DONE;
        }
        case PL_ACT_QUEUE:
            queue_add_playlist(path);           /* the saved file is never edited */
            break;
        case PL_ACT_RENAME:
        {
            /* Pre-fill the keyboard with the current name, then rename the
             * .m3u8 file -- the same operation fileop.c's rename_file does. */
            if (trimpod_kbd_input(name, sizeof(name)) == 0 && name[0])
            {
                char newpath[MAX_PATH];
                snprintf(newpath, sizeof(newpath), "%s/%s.m3u8", pl_dir, name);
                if (strcmp(newpath, path) == 0)
                    break;                              /* unchanged */
                if (file_exists(newpath))
                    splash(HZ, ID2P(LANG_TRIMPOD_NAME_EXISTS));
                else if (rename(path, newpath) == 0)
                {
                    pl_load();
                    gui_synclist_set_nb_items(&p->lists, pl_count + 1);
                }
                else
                    splash(HZ, ID2P(LANG_TRIMPOD_RENAME_FAILED));
            }
            break;
        }
        case PL_ACT_EDIT:
            edit_playlist(sel);
            break;
        case PL_ACT_DELETE:
        {
            if (trimpod_confirm(str(LANG_TRIMPOD_DELETE_PLAYLIST_Q), name))
            {
                remove(path);
                pl_load();
                gui_synclist_set_nb_items(&p->lists, pl_count + 1);
                if (sel > pl_count)
                    gui_synclist_select_item(&p->lists, pl_count);
            }
            break;
        }
        default:                                /* -1: cancelled */
            break;
    }
    return TRIMPOD_PAGE_STAY;
}

/* ---- pick mode: add the pending file/dir to a chosen / new playlist ----- */

/* Insert the pending selection into an existing playlist `sel`: a single file/dir
 * (Rockbox expands a directory into its tracks), or an explicit set of track
 * paths (a library album/artist).  The screen closes afterwards. */
static enum trimpod_page_result pick_into_existing(struct playlists_page *p, int sel)
{
    char path[MAX_PATH];
    path_append(path, pl_dir, pl_namebuf + pl_off[sel], sizeof(path));
    if (p->pick_paths)
        for (int i = 0; i < p->pick_npaths; i++)
            catalog_insert_into(path, false, p->pick_paths[i], FILE_ATTR_AUDIO);
    else
        catalog_insert_into(path, false, p->pick_sel, p->pick_attr);
    splash(HZ, ID2P(LANG_TRIMPOD_ADDED_TO_PLAYLIST));
    return TRIMPOD_PAGE_DONE;
}

/* Standalone screen only: remember row `sel` so the next entry re-highlights it
 * (empty string = the "+ Add Playlist" row). */
static void pl_save_pos(struct playlists_page *p, int sel)
{
    if (p->pick)
        return;
    if (sel >= 0 && sel < pl_count)
        strlcpy(pl_last_sel, pl_namebuf + pl_off[sel], sizeof(pl_last_sel));
    else
        pl_last_sel[0] = '\0';
    pl_have_last = true;
}

static enum trimpod_page_result playlists_on_action(struct trimpod_page *self,
                                                    int action)
{
    struct playlists_page *p = (struct playlists_page *)self;
    int sel = gui_synclist_get_sel_pos(&p->lists);

    if (action == ACTION_STD_CANCEL)            /* B: leave */
    {
        pl_save_pos(p, sel);
        trimpod_transition_arm_back();          /* returning to the parent */
        return TRIMPOD_PAGE_DONE;
    }

    if (action == ACTION_STD_OK)                /* A */
    {
        if (sel == pl_count)                    /* "+ Add Playlist": create empty */
            add_playlist(p);                    /* (both modes) then stay, so a
                                                 * pick-mode user can now select it */
        else if (sel >= 0 && sel < pl_count)    /* a playlist row */
        {
            if (p->pick)
            {
                enum trimpod_page_result r = pick_into_existing(p, sel);
                if (r == TRIMPOD_PAGE_DONE)
                    pl_save_pos(p, sel);
                return r;
            }
            /* tap A: open this playlist's track listing (playlist_view()
             * consumes the pending view and runs the viewer on it).  The
             * viewer refuses an empty playlist, so teach the add gesture
             * instead of bouncing. */
            char path[MAX_PATH];
            path_append(path, pl_dir, pl_namebuf + pl_off[sel], sizeof(path));
            if (pl_file_is_empty(path))
            {
                char name[MAX_PATH];
                pl_display_name(sel, name, sizeof(name));
                static const char *const rows[] =
                    { ID2P(LANG_TRIMPOD_PLAYLIST_EMPTY),
                      ID2P(LANG_TRIMPOD_PLAYLIST_EMPTY_HINT1),
                      ID2P(LANG_TRIMPOD_PLAYLIST_EMPTY_HINT2) };
                trimpod_message_page(name, rows, 3);
                return TRIMPOD_PAGE_STAY;
            }
            strlcpy(pl_pending_file, pl_namebuf + pl_off[sel],
                    sizeof pl_pending_file);
            pl_pending_view = true;
            p->result = GO_TO_PLAYLIST_VIEWER;
            pl_save_pos(p, sel);
            return TRIMPOD_PAGE_DONE;
        }
    }
    else if (action == ACTION_STD_CONTEXT       /* Hold A: playlist actions */
             && !p->pick && sel >= 0 && sel < pl_count)
    {
        enum trimpod_page_result r = playlist_action(p, sel);
        if (r == TRIMPOD_PAGE_DONE)              /* leaving (play -> WPS) */
            pl_save_pos(p, sel);
        return r;
    }
    return TRIMPOD_PAGE_STAY;                    /* the runner redraws each frame */
}

static const struct trimpod_page_vtable playlists_vtable =
{
    .legend    = NULL,                  /* normal header (no legend) */
    .draw      = playlists_draw,
    .poll      = playlists_poll,
    .on_action = playlists_on_action,
};

/* A (open/add), Hold-A (playlist actions) and B (back) act; list scrolling is
 * consumed in poll */
static const int playlists_allowed[] = { ACTION_STD_OK, ACTION_STD_CONTEXT,
                                         ACTION_STD_CANCEL, -1 };

/* Shared driver: (re)read the catalog, run the page with the given title. */
static int run_playlists(struct playlists_page *p, const char *title)
{
    pl_load();
    gui_synclist_init(&p->lists, pl_get_name, NULL, false, 1, NULL);
    gui_synclist_set_title(&p->lists, (char *)title, Icon_Playlist);
    gui_synclist_set_nb_items(&p->lists, pl_count + 1);

    /* Standalone screen: restore the remembered selection (incl. on return from
     * Now Playing); pick mode always starts at the top. */
    int sel = 0;
    if (!p->pick && pl_have_last)
    {
        if (pl_last_sel[0])
        {
            for (int i = 0; i < pl_count; i++)
                if (strcmp(pl_namebuf + pl_off[i], pl_last_sel) == 0)
                {
                    sel = i;
                    break;
                }
        }
        else
            sel = pl_count;          /* the "+ Add Playlist" row */
    }
    gui_synclist_select_item(&p->lists, sel);

    push_current_activity(ACTIVITY_PLAYLISTBROWSER);
    trimpod_page_run(&p->base);
    pop_current_activity();
    return p->base.home ? GO_TO_ROOT : p->result;
}

/* Consume a pending Play/Shuffle Playlist request (set by the action menu) and
 * start playback, mirroring the root Shuffle action: physically shuffle the
 * track order when requested, then play from the top; global_settings.playlist_
 * shuffle is left untouched.  Returns 1 when playback started (caller -> Now
 * Playing), 0 when nothing was pending (caller -> show the file list), or -1 on
 * an empty/unloadable playlist (caller -> back to the Playlists list). */
/* Consume a pending view request (tap A on a playlist row): fill `path` with
 * the playlist's full path and return true.  playlist_view() then opens the
 * stock viewer on it -- A on a track there starts playback from that track. */
bool trimpod_playlists_take_pending_view(char *path, size_t len)
{
    if (!pl_pending_view)
        return false;
    pl_pending_view = false;
    path_append(path, pl_dir, pl_pending_file, len);
    return true;
}

int trimpod_playlists_start_pending(void)
{
    if (!pl_pending_active)
        return 0;
    pl_pending_active = false;

    if (playlist_create(pl_dir, pl_pending_file) == -1 || playlist_amount() <= 0)
    {
        splash(HZ, ID2P(LANG_TRIMPOD_PLAYLIST_EMPTY));
        return -1;
    }
    /* Reflect the chosen mode in the shuffle setting so Now Playing shows it
     * correctly (On for Shuffle Playlist, Off for Play Playlist). */
    global_settings.playlist_shuffle = pl_pending_shuffle;
    if (pl_pending_shuffle)
        playlist_shuffle(current_tick, -1);
    playlist_start(0, 0, 0);
    return 1;
}

int trimpod_playlists_screen(void *param)
{
    (void)param;
    struct playlists_page p =
    {
        /* enter_honor_back: returning from the viewer / Now Playing (which
         * armed a back) slides this screen in L->R; a fresh open from the root
         * enters forward.  no_arm_back: the exit slide is armed explicitly
         * (only on B), so tap-A slides forward into the track listing. */
        .base = { .vt = &playlists_vtable, .context = CONTEXT_LIST,
                  .allowed = playlists_allowed, .enter_honor_back = true,
                  .no_arm_back = true },
        /* Back to the root menu via GO_TO_ROOT (NOT GO_TO_PREVIOUS): the root
         * dispatch keeps last_screen == our entry, so the menu re-highlights the
         * "Playlists" row on return -- the same contract Settings uses. */
        .result = GO_TO_ROOT,
    };
    return run_playlists(&p, str(LANG_PLAYLISTS));
}

void trimpod_playlists_pick(const char *sel, int sel_attr)
{
    struct playlists_page p =
    {
        .base = { .vt = &playlists_vtable, .context = CONTEXT_LIST,
                  .allowed = playlists_allowed },
        .result = GO_TO_ROOT,
        .pick = true, .pick_sel = sel, .pick_attr = sel_attr,
    };
    run_playlists(&p, str(LANG_TRIMPOD_ADD_TO_PLAYLIST));
}

/* Add an explicit set of track paths (a library album/artist, which has no
 * single directory to expand) to a chosen / new playlist. */
static void trimpod_playlists_pick_tracks(char **paths, int count)
{
    struct playlists_page p =
    {
        .base = { .vt = &playlists_vtable, .context = CONTEXT_LIST,
                  .allowed = playlists_allowed },
        .result = GO_TO_ROOT,
        .pick = true, .pick_paths = paths, .pick_npaths = count,
    };
    run_playlists(&p, str(LANG_TRIMPOD_ADD_TO_PLAYLIST));
}

/* ---- shared Hold-A music context menu ------------------------------------
 * Play / Play Next / Add to Play Queue / Add to Playlist for the highlighted
 * row.  The adds are handled here; Play is returned to the caller, which owns
 * how its rows start. */
enum { MUSIC_CTX_PLAY = 0, MUSIC_CTX_PLAY_NEXT, MUSIC_CTX_ADD_QUEUE,
       MUSIC_CTX_ADD_PL };
static const char *const music_ctx_opts[] =
    { ID2P(LANG_PLAY), ID2P(LANG_PLAY_NEXT), ID2P(LANG_TRIMPOD_ADD_TO_PLAY_QUEUE),
      ID2P(LANG_TRIMPOD_ADD_TO_PLAYLIST) };

bool trimpod_music_context(const char *title, const char *path, int attr)
{
    switch (trimpod_context_menu(title, music_ctx_opts, ARRAYLEN(music_ctx_opts)))
    {
        case MUSIC_CTX_PLAY:      return true;
        case MUSIC_CTX_PLAY_NEXT: trimpod_queue_add(path, attr, PLAYLIST_INSERT); break;
        case MUSIC_CTX_ADD_QUEUE: trimpod_queue_add(path, attr, PLAYLIST_INSERT_LAST); break;
        case MUSIC_CTX_ADD_PL:    trimpod_playlists_pick(path, attr); break;
    }
    return false;
}

bool trimpod_music_context_tracks(const char *title, char **paths, int count)
{
    if (count <= 0)
        return false;
    switch (trimpod_context_menu(title, music_ctx_opts, ARRAYLEN(music_ctx_opts)))
    {
        case MUSIC_CTX_PLAY:      return true;
        case MUSIC_CTX_PLAY_NEXT: queue_add_tracks(paths, count, PLAYLIST_INSERT); break;
        case MUSIC_CTX_ADD_QUEUE: queue_add_tracks(paths, count, PLAYLIST_INSERT_LAST); break;
        case MUSIC_CTX_ADD_PL:    trimpod_playlists_pick_tracks(paths, count); break;
    }
    return false;
}
