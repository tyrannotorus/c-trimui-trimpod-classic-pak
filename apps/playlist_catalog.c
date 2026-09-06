/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 Sebastian Henriksen, Hardeep Sidhu
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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "string-extra.h"
#include "dir.h"
#include "file.h"
#include "kernel.h"
#include "lang.h"
#include "list.h"
#include "misc.h"
#include "pathfuncs.h"
#include "playlist.h"
#include "settings.h"
#include "rbpaths.h"
#include "splash.h"
#include "yesno.h"
#include "filetypes.h"
#include "playlist_catalog.h"

/* Use for recursive directory search */
struct add_track_context {
    int fd;
    int count;
};

/* we need playlist_dir_length for easy removal of playlist dir prefix */
static size_t playlist_dir_length;

static size_t get_directory(char* dirbuf, size_t dirbuf_sz)
{
    const char *pl_dir = PLAYLIST_CATALOG_DEFAULT_DIR;

    /* directory config is of the format: "dir: /path/to/dir" */
    if (global_settings.playlist_catalog_dir[0] != '\0')
    {
        pl_dir = global_settings.playlist_catalog_dir;
    }

    return path_append(dirbuf, pl_dir, PA_SEP_SOFT, dirbuf_sz);
}

/* Retrieve playlist directory from config file and verify it exists
 * attempts to create directory
 * catalog dir is returned in dirbuf */
static int initialize_catalog_buf(char* dirbuf, size_t dirbuf_sz)
{
    playlist_dir_length = get_directory(dirbuf, dirbuf_sz);
    if (playlist_dir_length >= dirbuf_sz)
    {
        return -2;
    }

    if (!dir_exists(dirbuf) && mkdir(dirbuf) < 0)
    {
        splashf(HZ*2, str(LANG_CATALOG_NO_DIRECTORY), dirbuf);
        return -1;
    }

    return 0;
}

void catalog_get_directory(char* dirbuf, size_t dirbuf_sz)
{
    if (initialize_catalog_buf(dirbuf, dirbuf_sz) < 0)
    {
        dirbuf[0] = '\0';
        return;
    }
}

/* display number of tracks inserted into playlists.  Used for directory
   insert */
static void display_insert_count(int count)
{
    splashf(0, str(LANG_PLAYLIST_INSERT_COUNT), count, str(LANG_OFF_ABORT));
}

/* Add specified track into playlist.  Callback from directory insert */
static int add_track_to_playlist(char* filename, void* context)
{
    struct add_track_context* c = (struct add_track_context*) context;

    if (fdprintf(c->fd, "%s\n", filename) <= 0)
        return -1;

    (c->count)++;

    if (((c->count)%PLAYLIST_DISPLAY_COUNT) == 0)
        display_insert_count(c->count);

    return 0;
}

/* Add "sel" file into specified "playlist".  How to insert depends on type
   of file */
int catalog_insert_into(const char* playlist, bool new_playlist,
                        const char* sel, int sel_attr)
{
    int fd;
    int result = -1;

    if (new_playlist)
        fd = open_utf8(playlist, O_CREAT|O_WRONLY|O_TRUNC);
    else
        fd = open(playlist, O_CREAT|O_WRONLY|O_APPEND, 0666);

    if(fd < 0)
    {
        splash(HZ*2, ID2P(LANG_FAILED));
        return result;
    }
    /* In case we're in the playlist directory */
    reload_directory();

    if ((sel_attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
    {
        /* append the selected file */
        if (fdprintf(fd, "%s\n", sel) > 0)
            result = 0;
    }
    else if ((sel_attr & FILE_ATTR_MASK) == FILE_ATTR_M3U)
    {
        /* append playlist */
        int f, fs, i;
        char buf[1024];

        if(strcasecmp(playlist, sel) == 0)
            goto exit;

        f = open_utf8(sel, O_RDONLY);
        if (f < 0)
            goto exit;

        i = lseek(f, 0, SEEK_CUR);
        fs = filesize(f);
        while (i < fs)
        {
            int n;

            n = read(f, buf, sizeof(buf));
            if (n < 0)
                break;

            if (write(fd, buf, n) < 0)
                break;

            i += n;
        }

        if (i >= fs)
            result = 0;

        close(f);
    }
    else if (sel_attr & ATTR_DIRECTORY)
    {
        /* search directory for tracks and append to playlist */
        bool recurse;
        const char *lines[] = {
            ID2P(LANG_RECURSE_DIRECTORY_QUESTION), sel};
        const struct text_message message={lines, 2};
        struct add_track_context context;


        if (sel[1] == '\0' && sel[0] == PATH_ROOTCHR)
            recurse = true;
        else if (TP_RECURSIVE_DIR_INSERT != RECURSE_ASK)
            recurse = (bool)TP_RECURSIVE_DIR_INSERT;
        else
        {
            /* Ask if user wants to recurse directory */
            recurse = (gui_syncyesno_run(&message, NULL, NULL)==YESNO_YES);
        }

        context.fd = fd;
        context.count = 0;

        display_insert_count(0);

        result = playlist_directory_tracksearch(sel, recurse,
            add_track_to_playlist, &context);

        display_insert_count(context.count);
    }

exit:
    close(fd);
    return result;
}
