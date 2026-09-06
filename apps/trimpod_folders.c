/***************************************************************************
 * Trimpod: user-managed virtual folders (Music, Podcasts, Audiobooks).
 *
 * Each category is a "virtual folder" that merges the contents of a
 * user-managed set of source folders.  On first run the list is SEEDED with a
 * default folder (/mnt/SDCARD/<Name>, which may or may not exist on the card --
 * a missing folder is simply skipped, never an error), but the user may remove
 * any folder, the seeded default included.  All three categories share one
 * engine, parameterised by a `struct folder_category`:
 *
 *  - Storage:  ROCKBOX_DIR/<store_file>, one absolute path per line.  Seeded
 *    with default_path only when the file does not yet exist; once the file
 *    exists (even empty) the user's choices stand and the default is not forced
 *    back in.
 *  - Settings -> "<Name> Folders": a list of the chosen folders + a "+ Add
 *    Folder" entry, as trimpod_page-derived pages.  A on "+ Add Folder" opens
 *    our native directory-picker page (A descends, Y adds, B backs out).  A on a
 *    folder row asks to remove it; B leaves.
 *  - Root entry: a flattened virtual view of every configured folder's
 *    contents, browsed with our native music browser.  If no folder exists or
 *    they hold no folders/audio (e.g. the user removed them all), a centered
 *    "(empty)" page is shown instead.
 *  - Root "Browse": the same browser over the whole card (PICKER_ROOT).
 *
 * The flattened view is a tmpfs "farm" of symlinks
 * (<farm_path>/<name> -> <configured folder>/<name>).  The hosted filesystem
 * layer follows symlinks (filesystem-app.c dir_get_info), so rockbox_browse()
 * lists, descends into and plays the real files; one link per top-level entry of
 * each folder gives the merged *contents* of all folders rather than the folders
 * themselves.
 ****************************************************************************/
#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>   /* qsort */
#include <string.h>
#include "string-extra.h"
#include "strnatcmp.h"   /* natural alphabetical sort (2 before 10) */
#include "kernel.h"      /* HZ */
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "settings.h"
#include "lang.h"
#include "splash.h"
#include "action.h"
#include "gui/list.h"
#include "tree.h"
#include "filetree.h"   /* ft_load + ft_play_from_context: shared browse/play engine */
#include "root_menu.h"
#include "misc.h"
#include "icon.h"
#include "filetypes.h"   /* filetype_get_attr / FILE_ATTR_AUDIO */
#include "playlist.h"    /* play a track on select */
#include "trimpod_folders.h"
#include "trimpod_ui.h"
#include "trimpod_page.h"
#include "trimpod_transition.h"   /* slide on folder descend/ascend */
#include "trimpod_playlists.h"    /* Hold-A = add highlighted file/folder to a playlist */
#include "trimpod_library.h"      /* Shuffle All's track set */

#define PICKER_ROOT     "/mnt/SDCARD"
#define MAX_FOLDERS     24
#define FPATH_LEN       MAX_PATH
#define LOAD_BUFSZ      (MAX_FOLDERS * FPATH_LEN)

/* libc symlink(); Rockbox does not wrap it and root paths map 1:1 to the OS
 * (app_root_realpath() == "/"), so OS and Rockbox paths are interchangeable. */
extern int symlink(const char *target, const char *linkpath);

/* Last browse position, so re-entering a browse view (incl. returning from
 * Now Playing) restores the folder + highlighted entry.  Session-only. */
struct browse_pos
{
    char last_dir[FPATH_LEN];
    char last_sel[FPATH_LEN];
    bool have_last;
};

/* One virtual-folder category (Music / Podcasts / Audiobooks). */
struct folder_category
{
    const char *store_file;     /* basename under ROCKBOX_DIR (e.g. "folders.txt") */
    const char *default_path;   /* unremovable default (e.g. "/mnt/SDCARD/Music") */
    const char *farm_path;      /* tmpfs merge dir (e.g. "/tmp/trimpod_music") */
    int title_lang;             /* "<Name> Folders" settings-page title */
    int browse_lang;            /* "<Name>" browse-screen title */

    /* runtime state */
    char folders[MAX_FOLDERS][FPATH_LEN];
    int  n_folders;

    struct browse_pos pos;
};

/* Music keeps the original "folders.txt" name so existing setups are preserved.
 * The farm dirs live in tmpfs (NOT /tmp/trimpod, a vfat bind-mount that cannot
 * hold symlinks). */
static struct folder_category cat_music = {
    .store_file = "folders.txt", .default_path = "/mnt/SDCARD/Music",
    .farm_path = "/tmp/trimpod_music",
    .title_lang = LANG_TRIMPOD_FOLDERS, .browse_lang = LANG_TRIMPOD_MUSIC,
};
static struct folder_category cat_podcast = {
    .store_file = "podcasts.txt", .default_path = "/mnt/SDCARD/Podcasts",
    .farm_path = "/tmp/trimpod_podcasts",
    .title_lang = LANG_TRIMPOD_PODCAST_FOLDERS,
    .browse_lang = LANG_TRIMPOD_PODCASTS,
};
static struct folder_category cat_audiobook = {
    .store_file = "audiobooks.txt", .default_path = "/mnt/SDCARD/Audiobooks",
    .farm_path = "/tmp/trimpod_audiobooks",
    .title_lang = LANG_TRIMPOD_AUDIOBOOK_FOLDERS,
    .browse_lang = LANG_TRIMPOD_AUDIOBOOKS,
};

/* ---- storage ---------------------------------------------------------- */

static void store_path(struct folder_category *cat, char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s", ROCKBOX_DIR, cat->store_file);
}

static void folders_save(struct folder_category *cat)
{
    char path[FPATH_LEN];
    store_path(cat, path, sizeof path);
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd < 0)
        return;
    for (int i = 0; i < cat->n_folders; i++)
    {
        write(fd, cat->folders[i], strlen(cat->folders[i]));
        write(fd, "\n", 1);
    }
    close(fd);
}

static void folders_load(struct folder_category *cat)
{
    cat->n_folders = 0;
    char path[FPATH_LEN];
    store_path(cat, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        /* first run (no file yet): seed with the default folder.  The user may
         * later remove it -- once the file exists we honour it verbatim. */
        strlcpy(cat->folders[0], cat->default_path, FPATH_LEN);
        cat->n_folders = 1;
        return;
    }
    static char rbuf[LOAD_BUFSZ];
    int len = read(fd, rbuf, sizeof(rbuf) - 1);
    close(fd);
    if (len > 0)
    {
        rbuf[len] = '\0';
        char *line = rbuf, *nl;
        while (line && *line && cat->n_folders < MAX_FOLDERS)
        {
            nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (*line == '/')
                strlcpy(cat->folders[cat->n_folders++], line, FPATH_LEN);
            line = nl ? nl + 1 : NULL;
        }
    }
}

/* Persist the list and reconcile the index now, under the Scanning screen. */
static void folders_changed(struct folder_category *cat)
{
    folders_save(cat);
    trimpod_library_reconcile(false);
}

static bool folders_add(struct folder_category *cat, const char *path)
{
    if (cat->n_folders >= MAX_FOLDERS)
        return false;
    for (int i = 0; i < cat->n_folders; i++)
        if (strcmp(cat->folders[i], path) == 0)
            return false; /* already present */
    strlcpy(cat->folders[cat->n_folders++], path, FPATH_LEN);
    folders_changed(cat);
    return true;
}

static void folders_remove(struct folder_category *cat, int idx)
{
    if (idx < 0 || idx >= cat->n_folders)
        return;
    for (int i = idx; i < cat->n_folders - 1; i++)
        strlcpy(cat->folders[i], cat->folders[i + 1], FPATH_LEN);
    cat->n_folders--;
    folders_changed(cat);
}

/* ---- library access (trimpod_library.c) ------------------------------- */

static struct folder_category *cat_by_index(int cat)
{
    switch (cat)
    {
        case TP_CAT_MUSIC:     return &cat_music;
        case TP_CAT_PODCAST:   return &cat_podcast;
        case TP_CAT_AUDIOBOOK: return &cat_audiobook;
        default:               return NULL;
    }
}

void trimpod_folders_load_all(void)
{
    folders_load(&cat_music);
    folders_load(&cat_podcast);
    folders_load(&cat_audiobook);
}

int trimpod_folders_root_count(int cat)
{
    struct folder_category *c = cat_by_index(cat);
    return c ? c->n_folders : 0;
}

const char *trimpod_folders_root_path(int cat, int idx)
{
    struct folder_category *c = cat_by_index(cat);
    if (!c || idx < 0 || idx >= c->n_folders)
        return NULL;
    return c->folders[idx];
}

/* ---- "+ Add Folder" picker: a native directory-picker page --------------
 * A trimpod_page that lists the sub-directories of the current directory:
 *   A = descend into the highlighted folder
 *   Y = add the highlighted folder (or the current folder if it has none)
 *   B = up one level, or leave the picker at the start root
 * No Rockbox file browser involved -- it's a first-class page like the others. */

#define PICK_MAX_DIRS   512
#define PICK_NAMEBUF    (24 * 1024)

/* one picker at a time -> the listing lives in file-scope statics, off-stack */
static char pick_namebuf[PICK_NAMEBUF];   /* packed NUL-terminated names */
static int  pick_off[PICK_MAX_DIRS];      /* offset of each name in pick_namebuf */
static bool pick_isdir[PICK_MAX_DIRS];    /* is entry i a folder (vs a track)? */
static int  pick_count;
static char pick_reselect[FPATH_LEN];     /* if set, pick_load highlights this
                                           * entry by name (used on ascend to
                                           * restore the folder we came from) */

struct picker_page
{
    struct trimpod_page base;
    struct gui_synclist lists;
    char   curdir[FPATH_LEN];
    char   root[FPATH_LEN];
    /* folder-picker mode (out!=NULL): Hold A picks a folder into out[] */
    char  *out;
    size_t out_len;
    bool   picked;
    /* music-browse mode (music=true): rows are folders+tracks, A plays a track,
     * exits with `result` for the root dispatch (GO_TO_WPS / GO_TO_ROOT) */
    bool   music;
    int    result;
    struct browse_pos *pos;        /* music mode: where the position persists */
    struct tree_context *mctx;     /* music mode: the stock browse engine state */
};

/* ---- mode-aware list accessors --------------------------------------------
 * Music mode is backed by the stock tree_context (one browse + playlist engine,
 * shared with the file browser); the "+ Add Folder" picker keeps its own simple
 * pick_*[] directory listing.  These hide which source a row comes from. */
static int item_count(struct picker_page *p)
{
    return p->music ? p->mctx->filesindir : pick_count;
}
static const char *item_name(struct picker_page *p, int sel)
{
    if (p->music)
    {
        struct entry *e = tree_get_entry_at(p->mctx, sel);
        return e ? e->name : "";
    }
    if (sel < 0 || sel >= pick_count)
        return "";
    return pick_namebuf + pick_off[sel];
}
static bool item_isdir(struct picker_page *p, int sel)
{
    if (p->music)
    {
        struct entry *e = tree_get_entry_at(p->mctx, sel);
        return e && (e->attr & ATTR_DIRECTORY);
    }
    return (sel >= 0 && sel < pick_count) && pick_isdir[sel];
}

/* Music mode: load p->curdir into the tree_context via the stock engine
 * (folders-first natural-alpha, same order the file browser uses).  dirfilter /
 * sort are configured once in folder_browse. */
static void music_load(struct picker_page *p)
{
    struct tree_context *c = p->mctx;
    strlcpy(c->currdir, p->curdir, sizeof(c->currdir));
    c->selected_item = 0;
    ft_load(c, NULL);
    gui_synclist_set_nb_items(&p->lists, c->filesindir);

    int sel = 0;
    if (pick_reselect[0])
    {
        for (int i = 0; i < c->filesindir; i++)
        {
            struct entry *e = tree_get_entry_at(c, i);
            if (e && strcmp(e->name, pick_reselect) == 0) { sel = i; break; }
        }
        pick_reselect[0] = '\0';
    }
    gui_synclist_select_item(&p->lists, sel);
}

/* Remember the current folder + highlighted entry so the next entry into this
 * view (incl. returning from Now Playing) lands back here. */
static void browse_save_pos(struct picker_page *p)
{
    if (!p->music || !p->pos)
        return;
    strlcpy(p->pos->last_dir, p->curdir, sizeof(p->pos->last_dir));
    int sel = gui_synclist_get_sel_pos(&p->lists);
    if (sel >= 0 && sel < item_count(p))
        strlcpy(p->pos->last_sel, item_name(p, sel), sizeof(p->pos->last_sel));
    else
        p->pos->last_sel[0] = '\0';
    p->pos->have_last = true;
}

/* qsort comparator: natural alphabetical order of the names at two offsets */
static int pick_off_cmp(const void *a, const void *b)
{
    return strnatcasecmp(pick_namebuf + *(const int *)a,
                         pick_namebuf + *(const int *)b);
}

/* Read curdir into the packed name table.  Picker mode: sub-directories only.
 * Music mode: folders first (pass 0) then audio tracks (pass 1).  Each group is
 * sorted alphabetically. */
static void pick_load(struct picker_page *p)
{
    pick_count = 0;
    int used = 0, nfolders = 0;
    int passes = p->music ? 2 : 1;
    for (int pass = 0; pass < passes; pass++)
    {
        DIR *d = opendir(p->curdir);
        if (!d)
            break;
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (e->d_name[0] == '.')          /* skip ., .., hidden */
                continue;
            struct dirinfo info = dir_get_info(d, e);
            bool is_dir = (info.attribute & ATTR_DIRECTORY);
            if (!p->music)
            {
                if (!is_dir)                  /* picker: directories only */
                    continue;
            }
            else if (pass == 0)               /* music pass 0: folders */
            {
                if (!is_dir)
                    continue;
            }
            else                              /* music pass 1: audio tracks */
            {
                if (is_dir ||
                    (filetype_get_attr(e->d_name) & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
                    continue;
            }
            int len = strlen(e->d_name) + 1;
            if (pick_count >= PICK_MAX_DIRS ||
                used + len > (int)sizeof(pick_namebuf))
                break;                        /* listing full (rare) */
            pick_off[pick_count] = used;
            pick_isdir[pick_count] = is_dir;
            pick_count++;
            memcpy(pick_namebuf + used, e->d_name, len);
            used += len;
        }
        closedir(d);
        if (pass == 0)
            nfolders = pick_count;            /* folders occupy [0, nfolders) */
    }
    /* sort folders, then tracks, alphabetically (each group on its own) */
    qsort(pick_off, nfolders, sizeof pick_off[0], pick_off_cmp);
    qsort(pick_off + nfolders, pick_count - nfolders, sizeof pick_off[0],
          pick_off_cmp);
    gui_synclist_set_nb_items(&p->lists, pick_count);

    /* Default to the top, unless an ascend asked us to re-highlight the folder
     * we just came out of (matched by name). */
    int sel = 0;
    if (pick_reselect[0])
    {
        for (int i = 0; i < pick_count; i++)
            if (strcmp(pick_namebuf + pick_off[i], pick_reselect) == 0)
            {
                sel = i;
                break;
            }
        pick_reselect[0] = '\0';
    }
    gui_synclist_select_item(&p->lists, sel);
}

/* render the (reloaded) current dir off-screen for a slide transition */
static struct picker_page *s_reload_p;
static void pick_reload_render(void *ctx)
{
    (void)ctx;
    if (s_reload_p->music)
        music_load(s_reload_p);
    else
        pick_load(s_reload_p);
    gui_synclist_draw(&s_reload_p->lists);
}
static void pick_slide(struct picker_page *p, enum trimpod_transition_dir dir)
{
    s_reload_p = p;
    trimpod_transition_animate(dir, pick_reload_render, NULL);
}

static const char *pick_get_name(int sel, void *data, char *buf, size_t buf_len)
{
    (void)buf; (void)buf_len;
    return item_name((struct picker_page *)data, sel);
}

static const char *picker_legend(struct trimpod_page *self)
{
    /* music: no legend (A plays / opens, Hold-A adds to a playlist, B backs out).
     * folder-picker (Settings): A opens and B backs (universal), but Hold-A to add
     * is the one non-obvious gesture, so publicize just that. */
    return ((struct picker_page *)self)->music ? NULL
                                               : str(LANG_TRIMPOD_HOLD_A_TO_ADD);
}

static void picker_draw(struct trimpod_page *self)
{
    gui_synclist_draw(&((struct picker_page *)self)->lists);
}

static int picker_poll(struct trimpod_page *self, int timeout)
{
    int action;
    list_do_action(self->context, timeout,
                   &((struct picker_page *)self)->lists, &action);
    return action;
}

static enum trimpod_page_result picker_on_action(struct trimpod_page *self,
                                                 int action)
{
    struct picker_page *p = (struct picker_page *)self;
    int sel = gui_synclist_get_sel_pos(&p->lists);
    bool have_sel = (item_count(p) > 0 && sel >= 0 && sel < item_count(p));

    switch (action)
    {
        case ACTION_STD_OK:          /* A: open a folder, or (music) play a track */
            if (!have_sel)
                return TRIMPOD_PAGE_STAY;
            if (p->music && !item_isdir(p, sel))   /* a track -> play, go to WPS */
            {
                browse_save_pos(p);   /* return from Now Playing lands back here */
                /* Shared browse/play engine: builds the playlist from this same
                 * tree_context and starts it (shuffle/bookmark/resume handled). */
                if (ft_play_from_context(p->mctx, sel))
                {
                    p->result = GO_TO_WPS;
                    return TRIMPOD_PAGE_DONE;
                }
                return TRIMPOD_PAGE_STAY;   /* play cancelled at a prompt */
            }
            {                                      /* a folder -> descend (slide) */
                char next[FPATH_LEN];
                path_append(next, p->curdir, item_name(p, sel), sizeof(next));
                strlcpy(p->curdir, next, sizeof(p->curdir));
                pick_slide(p, TRIMPOD_TRANS_FORWARD);
            }
            return TRIMPOD_PAGE_STAY;

        case ACTION_STD_CONTEXT:     /* Hold A: music context menu / pick folder */
            if (p->music)
            {
                if (have_sel)
                {
                    char path[FPATH_LEN];
                    path_append(path, p->curdir, item_name(p, sel), sizeof(path));
                    bool isdir = item_isdir(p, sel);
                    if (trimpod_music_context(item_name(p, sel), path,
                                              isdir ? ATTR_DIRECTORY : FILE_ATTR_AUDIO))
                    {
                        /* Play: a track exactly as tapping it; a folder plays
                         * everything under it (recursive) as the new queue. */
                        browse_save_pos(p);
                        if (isdir ? trimpod_queue_add(path, ATTR_DIRECTORY, PLAYLIST_REPLACE)
                                  : ft_play_from_context(p->mctx, sel))
                        {
                            p->result = GO_TO_WPS;
                            return TRIMPOD_PAGE_DONE;
                        }
                    }
                }
                return TRIMPOD_PAGE_STAY;
            }
            /* folder-picker: pick the highlighted folder (or curdir if empty) */
            if (have_sel)
                path_append(p->out, p->curdir, item_name(p, sel), p->out_len);
            else
                strlcpy(p->out, p->curdir, p->out_len);
            p->picked = true;
            return TRIMPOD_PAGE_DONE;

        case ACTION_STD_CANCEL:      /* B: up one level, or leave at the root */
            if (strcmp(p->curdir, p->root) == 0)
            {
                if (p->music)                      /* slide the main menu back in */
                {
                    browse_save_pos(p);   /* remember the root highlight too */
                    trimpod_transition_arm_back();
                }
                return TRIMPOD_PAGE_DONE;
            }
            {
                /* Remember the folder we're leaving so the parent re-highlights
                 * it (not row 0) once it reloads. */
                const char *leaf;
                path_basename(p->curdir, &leaf);
                strlcpy(pick_reselect, leaf, sizeof(pick_reselect));

                const char *par;
                size_t n = path_dirname(p->curdir, &par);
                char parent[FPATH_LEN];
                if (n >= sizeof(parent))
                    n = sizeof(parent) - 1;
                memcpy(parent, par, n);
                parent[n] = '\0';
                strlcpy(p->curdir, parent, sizeof(p->curdir));
                pick_slide(p, TRIMPOD_TRANS_BACK);
            }
            return TRIMPOD_PAGE_STAY;
    }
    return TRIMPOD_PAGE_STAY;
}

static const struct trimpod_page_vtable picker_vtable =
{
    .legend    = picker_legend,
    .draw      = picker_draw,
    .poll      = picker_poll,
    .on_action = picker_on_action,
};

/* A (descend/play), B (up/leave), Hold-A (music: context menu / picker: add folder) */
static const int picker_allowed[] =
    { ACTION_STD_OK, ACTION_STD_CANCEL, ACTION_STD_CONTEXT, -1 };

/* Returns true and fills out[] with the chosen folder if the user picked one. */
static bool folder_pick(char *out, size_t out_len)
{
    struct picker_page p =
    {
        .base = { .vt = &picker_vtable, .context = CONTEXT_LIST,
                  .allowed = picker_allowed },
        .out = out, .out_len = out_len, .picked = false,
    };
    strlcpy(p.root, dir_exists(PICKER_ROOT) ? PICKER_ROOT : "/", sizeof(p.root));
    strlcpy(p.curdir, p.root, sizeof(p.curdir));

    gui_synclist_init(&p.lists, pick_get_name, &p, false, 1, NULL);
    gui_synclist_set_title(&p.lists, str(LANG_TRIMPOD_ADD_FOLDER), Icon_file_view_menu);
    pick_load(&p);

    push_current_activity(ACTIVITY_FILEBROWSER);
    trimpod_page_run(&p.base);
    pop_current_activity();

    return p.picked;
}

/* ---- Settings -> "<Name> Folders" (manage list) ----------------------- */

static const char *mgmt_get_name(int sel, void *data, char *buf, size_t buf_len)
{
    struct folder_category *cat = data;
    if (sel == cat->n_folders)
        return str(LANG_TRIMPOD_ADD_FOLDER_ROW);
    snprintf(buf, buf_len, "%s", cat->folders[sel]);
    return buf;
}

/* The folders list, derived from trimpod_page.  The run loop owns the loop,
 * header and key whitelist; this page describes the list and what A/B do.
 * A on "+ Add Folder" opens the picker; A on a folder row confirms removal;
 * B leaves. */
struct folders_page
{
    struct trimpod_page base;
    struct gui_synclist lists;
    struct folder_category *cat;
};

static void folders_page_draw(struct trimpod_page *self)
{
    struct folders_page *p = (struct folders_page *)self;
    gui_synclist_draw(&p->lists);
}

static int folders_page_poll(struct trimpod_page *self, int timeout)
{
    struct folders_page *p = (struct folders_page *)self;
    int action;
    list_do_action(self->context, timeout, &p->lists, &action);
    return action;
}

static enum trimpod_page_result folders_page_on_action(struct trimpod_page *self,
                                                       int action)
{
    struct folders_page *p = (struct folders_page *)self;
    struct folder_category *cat = p->cat;
    int sel = gui_synclist_get_sel_pos(&p->lists);

    if (action == ACTION_STD_CANCEL)            /* B: leave */
        return TRIMPOD_PAGE_DONE;

    if (action == ACTION_STD_OK)                /* A */
    {
        if (sel == cat->n_folders)              /* "+ Add Folder" */
        {
            char picked[FPATH_LEN];
            if (folder_pick(picked, sizeof(picked)))
            {
                if (!folders_add(cat, picked))
                    splash(HZ, ID2P(LANG_TRIMPOD_FOLDER_ADD_FAILED));
                gui_synclist_set_nb_items(&p->lists, cat->n_folders + 1);
            }
        }
        else if (sel >= 0 && sel < cat->n_folders)   /* a folder row */
        {
            if (trimpod_confirm(str(LANG_TRIMPOD_REMOVE_FOLDER_Q), cat->folders[sel]))
            {
                folders_remove(cat, sel);
                gui_synclist_set_nb_items(&p->lists, cat->n_folders + 1);
                if (sel > cat->n_folders)
                    gui_synclist_select_item(&p->lists, cat->n_folders);
            }
        }
    }
    return TRIMPOD_PAGE_STAY;                    /* the runner redraws each frame */
}

static const struct trimpod_page_vtable folders_vtable =
{
    .legend    = NULL,                  /* normal header (no legend) */
    .draw      = folders_page_draw,
    .poll      = folders_page_poll,
    .on_action = folders_page_on_action,
};

/* only A (add/remove) and B (back) act; list scrolling is consumed in poll */
static const int folders_allowed[] = { ACTION_STD_OK, ACTION_STD_CANCEL, -1 };

static int folders_settings(struct folder_category *cat)
{
    folders_load(cat);

    struct folders_page p =
    {
        .base = { .vt = &folders_vtable, .context = CONTEXT_LIST,
                  .allowed = folders_allowed },
        .cat = cat,
    };
    gui_synclist_init(&p.lists, mgmt_get_name, cat, false, 1, NULL);
    gui_synclist_set_title(&p.lists, (char *)str(cat->title_lang),
                           Icon_file_view_menu);
    gui_synclist_set_nb_items(&p.lists, cat->n_folders + 1);
    gui_synclist_select_item(&p.lists, 0);

    trimpod_page_run(&p.base);
    return 0;
}

/* ---- root virtual-folder entry (flattened view) ----------------------- */

/* Remove every entry currently in the farm dir (each is a symlink). */
static void farm_clear(struct folder_category *cat)
{
    DIR *d = opendir(cat->farm_path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' ||
             (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;
        char path[FPATH_LEN];
        path_append(path, cat->farm_path, e->d_name, sizeof(path));
        remove(path);
    }
    closedir(d);
}

/* (Re)build the symlink farm. Returns the number of links created.
 * First folder wins on name collisions (symlink() fails with EEXIST).
 * Folders that don't exist are silently skipped. */
static int farm_build(struct folder_category *cat)
{
    mkdir(cat->farm_path);
    farm_clear(cat);

    int created = 0;
    for (int i = 0; i < cat->n_folders; i++)
    {
        DIR *d = opendir(cat->folders[i]);
        if (!d)
            continue;                  /* missing/unreadable folder: skip */
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (e->d_name[0] == '.')   /* skip ., .., hidden */
                continue;
            char target[FPATH_LEN], link[FPATH_LEN];
            path_append(target, cat->folders[i], e->d_name, sizeof(target));
            path_append(link, cat->farm_path, e->d_name, sizeof(link));
            if (symlink(target, link) == 0)
                created++;
        }
        closedir(d);
    }
    return created;
}

/* Resolve a category's flattened root the same way for browsing and shuffle:
 * a single existing source folder is used directly; several are merged into the
 * symlink farm.  Returns NULL if no source folder currently exists.  The folder
 * list must already be loaded; the returned pointer is owned by `cat`. */
static const char *folder_resolve_root(struct folder_category *cat)
{
    /* Only existing source folders matter; a missing folder (the seeded default
     * included) is fine -- it's simply not part of the view. */
    int n_exist = 0, only = -1;
    for (int i = 0; i < cat->n_folders; i++)
        if (dir_exists(cat->folders[i])) { n_exist++; only = i; }

    if (n_exist == 1)
        return cat->folders[only];          /* single source: use it directly */
    if (n_exist > 1 && farm_build(cat) > 0)
        return cat->farm_path;              /* several: a merged symlink farm */
    return NULL;
}

/* Browse `root` with the stock browse engine, rendered by our picker: folders
 * descend, tracks play via ft_play_from_context, B at the root leaves.  The
 * global tree_context is borrowed and restored.  Returns the page result, or
 * -1 when the root holds no folders/audio (nothing was shown). */
static int browse_run(const char *root, int title_lang, struct browse_pos *pos)
{
    struct tree_context *c = tree_get_context();
    static int music_filter = SHOW_MUSIC;
    int  *saved_filter = c->dirfilter;
    int   saved_sort   = c->sort_dir;
    bool  saved_brows  = c->is_browsing;
    struct browse_context *saved_browse = c->browse;
    char  saved_last[sizeof(global_status.browse_last_folder)];
    memcpy(saved_last, global_status.browse_last_folder, sizeof(saved_last));

    c->dirfilter   = &music_filter;     /* dirs + audio, folders-first */
    c->sort_dir    = SORT_ALPHA;
    c->is_browsing = false;
    c->browse      = NULL;
    c->out_of_tree = 0;                 /* ensure ft_load actually loads */

    struct picker_page p =
    {
        /* enter_honor_back: returning from Now Playing (which armed a back)
         * slides the browser in L->R; a fresh open from the root still
         * enters forward.  no_arm_back: the exit slide is armed explicitly
         * (only on B at the root), not automatically. */
        .base = { .vt = &picker_vtable, .context = CONTEXT_LIST,
                  .allowed = picker_allowed, .no_arm_back = true,
                  .enter_honor_back = true },
        .music = true, .result = GO_TO_ROOT,   /* root re-highlights our entry */
        .pos = pos, .mctx = c,
    };
    strlcpy(p.root, root, sizeof(p.root));

    /* Restore the last browse position (incl. on return from Now Playing):
     * the saved folder if it still exists and sits under this root, else the
     * root.  pick_reselect re-highlights the saved entry once the list loads. */
    if (pos->have_last && dir_exists(pos->last_dir) &&
        strncmp(pos->last_dir, root, strlen(root)) == 0)
    {
        strlcpy(p.curdir, pos->last_dir, sizeof(p.curdir));
        strlcpy(pick_reselect, pos->last_sel, sizeof(pick_reselect));
    }
    else
        strlcpy(p.curdir, root, sizeof(p.curdir));

    gui_synclist_init(&p.lists, pick_get_name, &p, false, 1, NULL);
    gui_synclist_set_title(&p.lists, (char *)str(title_lang), Icon_Audio);
    music_load(&p);

    /* If a restored folder turned up empty (its files were since removed),
     * fall back to the root rather than showing the "(empty)" page. */
    if (item_count(&p) == 0 && strcmp(p.curdir, root) != 0)
    {
        strlcpy(p.curdir, root, sizeof(p.curdir));
        pick_reselect[0] = '\0';
        music_load(&p);
    }

    bool browsed = (item_count(&p) > 0);
    if (browsed)                        /* has folders/audio: browse it */
    {
        push_current_activity(ACTIVITY_FILEBROWSER);
        trimpod_page_run(&p.base);
        pop_current_activity();
    }

    /* Restore the tree context untouched. */
    c->dirfilter   = saved_filter;
    c->sort_dir    = saved_sort;
    c->is_browsing = saved_brows;
    c->browse      = saved_browse;
    memcpy(global_status.browse_last_folder, saved_last, sizeof(saved_last));

    if (!browsed)
        return -1;
    return p.base.home ? GO_TO_ROOT : p.result;
}

static int folder_browse(struct folder_category *cat)
{
    folders_load(cat);

    const char *root = folder_resolve_root(cat);
    if (root)
    {
        int r = browse_run(root, cat->browse_lang, &cat->pos);
        if (r >= 0)
            return r;
    }

    /* No usable source folder yet: instead of a dead-end "(empty)", drop the user
     * into THIS category's folder list (the default folder + "+ Add Folder") so
     * they can set it up (most won't know to visit Settings -> Audio Folders).
     * Called directly here, so backing out of the list returns to the main menu
     * -- not the Settings -> Audio Folders menu. */
    folders_settings(cat);
    return GO_TO_ROOT;
}

/* ---- public entry points (one thin wrapper per category) -------------- */

int trimpod_music_settings(void)     { return folders_settings(&cat_music); }
int trimpod_podcast_settings(void)   { return folders_settings(&cat_podcast); }
int trimpod_audiobook_settings(void) { return folders_settings(&cat_audiobook); }

int trimpod_music_browse(void *param)     { (void)param; return folder_browse(&cat_music); }
int trimpod_podcast_browse(void *param)   { (void)param; return folder_browse(&cat_podcast); }
int trimpod_audiobook_browse(void *param) { (void)param; return folder_browse(&cat_audiobook); }

/* root "Browse": the whole card in the same browser as the categories */
int trimpod_files_browse(void *param)
{
    (void)param;
    static struct browse_pos pos;
    int r = browse_run(dir_exists(PICKER_ROOT) ? PICKER_ROOT : "/",
                       LANG_TRIMPOD_BROWSE, &pos);
    if (r >= 0)
        return r;
    splash(HZ, ID2P(LANG_TRIMPOD_NO_MUSIC));
    trimpod_transition_suppress_next();   /* only a splash: don't re-slide */
    return GO_TO_ROOT;
}

/* ---- root "Shuffle All": shuffle + play the whole Music library -----------
 * Every Music track from the index (no filesystem walk), shuffled, straight to
 * Now Playing.  Music only -- podcasts/audiobooks are never shuffled. */
int trimpod_shuffle_all(void *param)
{
    (void)param;
    folders_load(&cat_music);

    if (!folder_resolve_root(&cat_music))
    {
        /* No music configured yet: drop into the Music folder setup, exactly as
         * browsing an empty Music entry does, rather than a dead-end splash. */
        folders_settings(&cat_music);
        return GO_TO_ROOT;
    }

    struct trimpod_queue q;
    if (!trimpod_queue_begin(&q, PLAYLIST_REPLACE))   /* declined the erase */
        return GO_TO_ROOT;
    trimpod_library_music_paths(trimpod_queue_add_path, &q);
    if (!trimpod_queue_end(&q, 0, true))              /* nothing indexed */
    {
        splash(HZ, ID2P(LANG_TRIMPOD_NO_MUSIC));
        trimpod_transition_suppress_next();   /* only a splash: don't re-slide the menu */
        return GO_TO_ROOT;
    }
    return GO_TO_WPS;
}
