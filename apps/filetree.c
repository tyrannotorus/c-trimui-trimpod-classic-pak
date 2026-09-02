/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Björn Stenberg
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdlib.h>
#include <file.h>
#include <dir.h>
#include <string.h>
#include <kernel.h>
#include <lcd.h>
#include <debug.h>
#include <font.h>
#include <limits.h>
#include "bookmark.h"
#include "tree.h"
#include "root_menu.h"
#include "pathfuncs.h"
#include "viewport.h"
#include "core_alloc.h"
#include "settings.h"
#include "filetypes.h"
#include "playlist.h"
#include "lang.h"
#include "language.h"
#include "screens.h"
#include "rolo.h"
#include "splash.h"
#include "cuesheet.h"
#include "filetree.h"
#include "misc.h"
#include "audio.h"
#include "metadata.h"
#include "trimpod_m4b.h"
#include "strnatcmp.h"
#include "keyboard.h"


#include "wps.h"

static struct compare_data
{
    int sort_dir; /* qsort key for sorting directories */
    int sort_file; /*       ...for sorting files       */
    int(*_compar)(const char*, const char*, size_t);
} cmp_data;

/* dummy wrapper to match the (a,b,size_t) comparator signature */
static int strnatcasecmp_n(const char *a, const char *b, size_t n)
{
    (void)n;
     return strnatcasecmp(a, b);
}

int ft_build_playlist(struct tree_context* c, int start_index)
{
    int i;
    int start=start_index;
    int res;
    struct playlist_info *playlist = playlist_get_current();

    tree_lock_cache(c);
    struct entry *entries = tree_get_entries(c);
    bool exceeds_pl = false;
    if (c->filesindir > playlist->max_playlist_size)
    {
        exceeds_pl = true;
        start_index = 0;
    }
    struct playlist_insert_context pl_context;

    res = playlist_insert_context_create(playlist, &pl_context,
                                        PLAYLIST_REPLACE, false, false);
    if (res >= 0)
    {
        cpu_boost(true);
        for(i = 0;i < c->filesindir;i++)
        {
            int item = i;
            if (exceeds_pl)
                item = (i + start) % c->filesindir;
            if((entries[item].attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
            {
                res = playlist_insert_context_add(&pl_context, entries[item].name);
                if (res < 0)
                    break;
            }
            else if (!exceeds_pl)
            {
                /* Adjust the start index when se skip non-MP3 entries */
                if(i < start)
                    start_index--;
            }
        }
        cpu_boost(false);
    }

    playlist_insert_context_release(&pl_context);

    tree_unlock_cache(c);
    return start_index;
}

/* Start playback of a playlist, confirming first if the current one is
 * modified.  Returns false if playback wasn't started. */
bool ft_play_playlist(char* dirname, char* filename)
{
    if (!warn_on_pl_erase())
        return false;

    if (playlist_create(dirname, filename) != -1)
    {
        global_settings.playlist_shuffle = false;   /* normal play: file order */
        playlist_start(0, 0, 0);
        return true;
    }

    return false;
}

/* support function for qsort() */
static int compare(const void* p1, const void* p2)
{
    struct entry* e1 = (struct entry*)p1;
    struct entry* e2 = (struct entry*)p2;
    int criteria;

    if (cmp_data.sort_dir == SORT_AS_FILE)
    {   /* treat as two files */
        criteria = cmp_data.sort_file;
    }
    else if (e1->attr & ATTR_DIRECTORY && e2->attr & ATTR_DIRECTORY)
    {   /* two directories */
        criteria = cmp_data.sort_dir;


    }
    else if (!(e1->attr & ATTR_DIRECTORY) && !(e2->attr & ATTR_DIRECTORY))
    {   /* two files */
        criteria = cmp_data.sort_file;
    }
    else /* dir and file, dir goes first */
        return (e2->attr & ATTR_DIRECTORY) - (e1->attr & ATTR_DIRECTORY);

    switch(criteria)
    {
        case SORT_TYPE:
        case SORT_TYPE_REVERSED:
        {
            int t1 = e1->attr & FILE_ATTR_MASK;
            int t2 = e2->attr & FILE_ATTR_MASK;

            if (!t1) /* unknown type */
                t1 = INT_MAX; /* gets a high number, to sort after known */
            if (!t2) /* unknown type */
                t2 = INT_MAX; /* gets a high number, to sort after known */

            if (t1 != t2) /* if different */
                return (t1 - t2) * (criteria == SORT_TYPE_REVERSED ? -1 : 1);
            /* else alphabetical sorting */
            return cmp_data._compar(e1->name, e2->name, MAX_PATH);
        }

        case SORT_DATE:
        case SORT_DATE_REVERSED:
        {
            if (e1->time_write != e2->time_write)
                return (e1->time_write - e2->time_write)
                       * (criteria == SORT_DATE_REVERSED ? -1 : 1);
            /* else fall through to alphabetical sorting */
        }
        case SORT_ALPHA:
        case SORT_ALPHA_REVERSED:
        {
            return cmp_data._compar(e1->name, e2->name, MAX_PATH) *
                (criteria == SORT_ALPHA_REVERSED ? -1 : 1);
        }

    }
    return 0; /* never reached */
}

/* load and sort directory into the tree's cache. returns NULL on failure. */
int ft_load(struct tree_context* c, const char* tempdir)
{
    if (c->out_of_tree > 0) /* something else is loaded */
        return 0;

    int files_in_dir = 0;
    int name_buffer_used = 0;
    struct dirent *entry;
    bool (*callback_show_item)(char *, int, struct tree_context *) = NULL;
    DIR *dir;

    if (!c->is_browsing)
        c->browse = NULL;

    if (tempdir)
        dir = opendir(tempdir);
    else
    {
        dir = opendir(c->currdir);
        callback_show_item = c->browse? c->browse->callback_show_item: NULL;
    }
    if(!dir)
        return -1; /* not a directory */

    c->dirsindir = 0;
    c->dirfull = false;

    tree_lock_cache(c);
    while ((entry = readdir(dir))) {
        int len;
        struct dirinfo info;
        struct entry* dptr = tree_get_entry_at(c, files_in_dir);
        if (!dptr)
        {
            c->dirfull = true;
            break;
        }

        info = dir_get_info(dir, entry);
        len = strlen((char *)entry->d_name);

        /* Skip FAT volume ID */
        if (info.attribute & ATTR_VOLUME_ID) {
            continue;
        }

        dptr->attr = info.attribute;
        int dir_attr = (dptr->attr & ATTR_DIRECTORY);
        /* skip directories . and .. */
        if (dir_attr && is_dotdir_name(entry->d_name))
            continue;

        /* filter out dotfiles and hidden files */
        if (*c->dirfilter != SHOW_ALL &&
            ((entry->d_name[0]=='.') ||
            (info.attribute & ATTR_HIDDEN))) {
            continue;
        }

        if (*c->dirfilter == SHOW_PLUGINS && (dptr->attr & ATTR_DIRECTORY) &&
            (dptr->attr &
            (ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID | ATTR_VOLUME)) != 0) {
            continue; /* skip non plugin folders */
        }

        /* check for known file types */
        if ( !(dir_attr) )
            dptr->attr |= filetype_get_attr((char *)entry->d_name);

        int file_attr = (dptr->attr & FILE_ATTR_MASK);

#define CHK_FT(show,attr) (*c->dirfilter == (show) && file_attr != (attr))
        /* filter out non-visible files */
        if ((!(dir_attr) && (CHK_FT(SHOW_PLAYLIST, FILE_ATTR_M3U) ||
            (CHK_FT(SHOW_MUSIC, FILE_ATTR_AUDIO) && file_attr != FILE_ATTR_M3U) ||
            (*c->dirfilter == SHOW_SUPPORTED && !filetype_supported(dptr->attr)))) ||
            CHK_FT(SHOW_WPS,  FILE_ATTR_WPS)  ||
            CHK_FT(SHOW_FONT, FILE_ATTR_FONT) ||
            CHK_FT(SHOW_SBS,  FILE_ATTR_SBS)  ||
            CHK_FT(SHOW_M3U, FILE_ATTR_M3U) ||
            CHK_FT(SHOW_CFG, FILE_ATTR_CFG) ||
            CHK_FT(SHOW_LNG, FILE_ATTR_LNG) ||
            CHK_FT(SHOW_MOD, FILE_ATTR_MOD) ||
           /* show first level directories */
           ((!(dir_attr) || c->dirlevel > 0)       &&
            CHK_FT(SHOW_PLUGINS, FILE_ATTR_ROCK)   &&
                       file_attr != FILE_ATTR_LUA  &&
                       file_attr != FILE_ATTR_OPX) ||
            (callback_show_item && !callback_show_item(entry->d_name, dptr->attr, c)))
        {
            continue;
        }
#undef CHK_FT

        if (len > c->cache.name_buffer_size - name_buffer_used - 1) {
            /* Tell the world that we ran out of buffer space */
            c->dirfull = true;
            break;
        }

        ++files_in_dir;

        dptr->name = core_get_data(c->cache.name_buffer_handle)+name_buffer_used;
        dptr->time_write = info.mtime;
        strcpy(dptr->name, (char *)entry->d_name);
        name_buffer_used += len + 1;

        if (dir_attr) /* count the remaining dirs */
            c->dirsindir++;
    }
    c->filesindir = files_in_dir;
    c->dirlength = files_in_dir;
    closedir(dir);

    /* Trimpod: fixed folders-first, natural-alpha, case-insensitive sort
     * (the File View sort settings were removed). */
    cmp_data.sort_dir = (*c->dirfilter == SHOW_PLUGINS) ? SORT_AS_FILE : TP_SORT_DIR;
    cmp_data.sort_file = TP_SORT_DIR;
    cmp_data._compar = strnatcasecmp_n;

    qsort(tree_get_entries(c), files_in_dir, sizeof(struct entry), compare);

    tree_unlock_cache(c);

    if (TP_KEEP_DIRECTORY && get_current_activity() == ACTIVITY_FILEBROWSER)
    {
        path_append(global_status.browse_last_folder, c->currdir, PA_SEP_HARD,
                    sizeof(global_status.browse_last_folder));
    }

    return 0;
}
static void ft_load_font(char *file)
{
    int current_font_id;
    enum screen_type screen = SCREEN_MAIN;
#if NB_SCREENS > 1
    MENUITEM_STRINGLIST(menu, ID2P(LANG_CUSTOM_FONT), NULL,
                        ID2P(LANG_MAIN_SCREEN), ID2P(LANG_REMOTE_SCREEN))
    switch (do_menu(&menu, NULL, NULL, false))
    {
        case 0: /* main lcd */
            screen = SCREEN_MAIN;
            set_file(file, (char *)global_settings.font_file);
            break;
        case 1: /* remote */
            screen = SCREEN_REMOTE;
            set_file(file, (char *)global_settings.remote_font_file);
            break;
    }
#else
    set_file(file, (char *)global_settings.font_file);
#endif
    splash(0, ID2P(LANG_WAIT));
    current_font_id = screens[screen].getuifont();
    if (current_font_id >= 0)
        font_unload(current_font_id);
    screens[screen].setuifont(
        font_load_ex(file,0,TRIMPOD_GLYPHS_TO_CACHE));
    viewportmanager_theme_changed(THEME_UI_VIEWPORT);
}

static void ft_apply_skin_file(char *buf, char *file)
{
    splash(0, ID2P(LANG_WAIT));
    set_file(buf, file);
    settings_apply_skins();
}

static const char *strip_slash(const char *path, const char *def)
{
    if (path)
    {
        while (*path == PATH_SEPCH)
            path++; /* we don't want this treated as an absolute path */
        return path;
    }
    return def;
}

int ft_assemble_path(char *buf, size_t bufsz, const char* currdir, const char* filename)
{
    size_t len;
    const char *cd = strip_slash(currdir, "");
    filename = strip_slash(filename, "");
    /* remove slashes and NULL strings to make logic below simpler */

    /* Other devices might need a specific drive/dir prepended but its usually '/' */
    if (*cd != '\0') /* Not in / */
    {
        len = path_append(buf, root_realpath(), cd, bufsz);/* /currdir */
        if(len < bufsz)
            len += path_append(buf + len, PA_SEP_HARD, filename, bufsz - len);
    } /* buf => /currdir/filename */
    else /* In / */
    {
        len = path_append(buf, root_realpath(), filename, bufsz);
    }  /* buf => /filename */

    if (len > bufsz)
        splash(HZ, ID2P(LANG_PLAYLIST_DIRECTORY_ACCESS_ERROR));
    return (int)len;
}

/* Build the current directory of `c` into a fresh playlist and start playback
 * at entry `sel`, honoring bookmark autoload and the playlist-erase warning,
 * plus the resume bookkeeping.  Plays in file order -- shuffle is transient, so
 * a normal folder play resets it (explicit Shuffle entries set it).  Shared by
 * the stock browser (ft_enter) and the Trimpod virtual-folder browser so there
 * is exactly one folder->playlist->play path.  Returns true if playback
 * started (caller should go to the WPS). */
bool ft_play_from_context(struct tree_context* c, int sel)
{
    /* A file tap plays that file and goes to Now Playing. Resume-where-you-
     * left-off is the Resume Playback root entry -- except m4b audiobooks,
     * which always continue from their per-file saved position. */

    /* about to create a new current playlist... allow user to cancel */
    if (!warn_on_pl_erase())
        return false;

    if (playlist_create(c->currdir, NULL) == -1)
        return false;

    int start_index = ft_build_playlist(c, sel);

    unsigned long resume_elapsed = 0, resume_offset = 0;
    struct entry *file = tree_get_entry_at(c, sel);
    if (file)
    {
        char buf[MAX_PATH];
        ft_assemble_path(buf, sizeof(buf), c->currdir, file->name);
        trimpod_m4b_resume_get(buf, &resume_elapsed, &resume_offset);
    }

    /* Normal play is in file order -- shuffle is transient, not a persisted
       default (Shuffle Songs / Shuffle Playlist set it explicitly). */
    global_settings.playlist_shuffle = false;
    playlist_start(start_index, resume_elapsed, resume_offset);

    global_status.resume_index = start_index;
    global_status.resume_crc32 = playlist_get_filename_crc32(NULL, start_index);
    global_status.resume_elapsed = resume_elapsed;
    global_status.resume_offset = resume_offset;
    status_save(false);
    return true;
}

int ft_enter(struct tree_context* c)
{
    int rc = GO_TO_PREVIOUS;
    char buf[MAX_PATH];

    struct entry* file = tree_get_entry_at(c, c->selected_item);
    if (!file)
    {
        splashf(HZ, ID2P(LANG_READ_FAILED), str(LANG_UNKNOWN));
        return rc;
    }

    int file_attr = file->attr;
    ft_assemble_path(buf, sizeof(buf), c->currdir, file->name);
    if (file_attr & ATTR_DIRECTORY) {
        memcpy(c->currdir, buf, sizeof(c->currdir));
        if ( c->dirlevel < MAX_DIR_LEVELS )
            c->selected_item_history[c->dirlevel] = c->selected_item;
        c->dirlevel++;
        c->selected_item=0;
    }
    else {
        bool play = false;
        int start_index=0;

        switch ( file_attr & FILE_ATTR_MASK ) {
            case FILE_ATTR_M3U:
                play = ft_play_playlist(c->currdir, file->name);

                if (play)
                {
                    start_index = 0;
                }

                break;

            case FILE_ATTR_AUDIO:
            {
                /* If the highlighted file is the track already playing (e.g. the
                 * user backed out of Now Playing and re-selected it), return to
                 * the WPS instead of restarting it from the top.  Any other file
                 * falls through and starts normally. */
                if (audio_status())
                {
                    struct mp3entry *cur = audio_current_track();
                    if (cur && cur->path[0] && !strcmp(cur->path, buf))
                    {
                        rc = GO_TO_WPS;
                        break;
                    }
                }

                /* ft_play_from_context does the bookmark/warn/build/shuffle/
                 * start + resume bookkeeping and returns whether it started. */
                if (ft_play_from_context(c, c->selected_item))
                    rc = GO_TO_WPS;
                break;
            }
            case FILE_ATTR_SBS:
                ft_apply_skin_file(buf, global_settings.sbs_file);
                break;
                /* wps config file */
            case FILE_ATTR_WPS:
                ft_apply_skin_file(buf, global_settings.wps_file);
                break;
            case FILE_ATTR_CFG:
                splash(0, ID2P(LANG_WAIT));
                if (!settings_load_config(buf,true))
                    break;
                splash(HZ, ID2P(LANG_SETTINGS_LOADED));
                break;

            case FILE_ATTR_BMARK:
                splash(0, ID2P(LANG_WAIT));
                bookmark_load(buf);
                rc = GO_TO_FILEBROWSER;
                break;

            case FILE_ATTR_LNG:
                splash(0, ID2P(LANG_WAIT));
                if (lang_core_load(buf))
                {
                    splash(HZ, ID2P(LANG_FAILED));
                    break;
                }
                set_file(buf, (char *)global_settings.lang_file);
                viewportmanager_theme_changed(THEME_LANGUAGE);
                settings_apply_skins();
                splash(HZ, ID2P(LANG_LANGUAGE_LOADED));
                break;

            case FILE_ATTR_FONT:
                ft_load_font(buf);
                break;

            case FILE_ATTR_KBD:
                splash(0, ID2P(LANG_WAIT));
                if (!load_kbd(buf))
                    splash(HZ, ID2P(LANG_KEYBOARD_LOADED));
                set_file(buf, (char *)global_settings.kbd_file);
                break;

            case FILE_ATTR_CUE:
                display_cuesheet_content(buf);
                break;

                /* plugin file - plugins are not supported on this target */
            case FILE_ATTR_ROCK:
                break;

            default:
                /* No viewer-plugin association: non-audio files are not
                   openable on this target. */
                break;
        }

        if ( play ) {
            /* the resume_index must always be the index in the
               shuffled list in case shuffle is enabled */
            global_status.resume_index = start_index;
            global_status.resume_crc32 =
                playlist_get_filename_crc32(NULL, start_index);
            global_status.resume_elapsed = 0;
            global_status.resume_offset = 0;
            status_save(false);
            rc = GO_TO_WPS;
        }
        else {
            if (*c->dirfilter > NUM_FILTER_MODES &&
                *c->dirfilter != SHOW_CFG &&
                *c->dirfilter != SHOW_FONT &&
                *c->dirfilter != SHOW_PLUGINS)
            {
                rc = GO_TO_ROOT;
            }
        }
    }

    return rc;
}

int ft_exit(struct tree_context* c)
{
    extern char lastfile[]; /* from tree.c */
    char buf[MAX_PATH];
    int rc = 0;
    bool exit_func = false;
    int i = strlen(c->currdir);

    /* strip trailing slashes */
    while (c->currdir[i-1] == PATH_SEPCH)
        i--;

    if (i>1) {
        while (c->currdir[i-1]!=PATH_SEPCH)
            i--;
        strcpy(buf,&c->currdir[i]);
        if (i==1)
            c->currdir[i]='\0';
        else
            c->currdir[i-1]='\0';


        if (*c->dirfilter > NUM_FILTER_MODES && c->dirlevel < 1)
            exit_func = true;

        c->dirlevel--;
        if ( c->dirlevel < MAX_DIR_LEVELS )
            c->selected_item=c->selected_item_history[c->dirlevel];
        else
            c->selected_item=0;

        /* if undefined position */
        if (c->selected_item == -1)
            strcpy(lastfile, buf);
    }
    else
    {
        if (*c->dirfilter > NUM_FILTER_MODES && c->dirlevel < 1)
            exit_func = true;
    }

    if (exit_func)
        rc = 3;

    c->out_of_tree = 0;

    return rc;
}
