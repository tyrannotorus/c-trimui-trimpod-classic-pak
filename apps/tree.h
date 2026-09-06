/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Daniel Stenberg
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
#ifndef _TREE_H_
#define _TREE_H_

#include <stdbool.h>
#include <applimits.h>
#include <file.h>
#include "config.h"
#include "icon.h"

/* keep this struct compatible (total size and name member)
 * with struct tagtree_entry (tagtree.h) */
struct entry {
    char *name;
    int attr; /* FAT attributes + file type flags */
    unsigned time_write; /* Last write time */
};

struct tree_context;

struct tree_cache {
    /* A big buffer with plenty of entry structs, contains all files and dirs
     * in the current dir (with filters applied)
     * Note that they're buflib-allocated and can therefore possibly move
     * They need to be locked if used around yielding functions */
    int     entries_handle;         /* handle to the entry cache */
    int     name_buffer_handle;     /* handle to the name cache */
    int     max_entries;            /* Max entries in the cache */
    int     name_buffer_size;       /* in bytes */
};

struct browse_context {
    int dirfilter;
    const char *root;           /* full path of start directory */
};

/* browser context for file or db */
struct tree_context {
    /* The directory we are browsing */
    char currdir[MAX_PATH];
    /* the number of directories we have crossed from / */
    int dirlevel;
    /* The currently selected file/id3dbitem index (old dircursor+dirfile) */
    int selected_item;
    /* The selected item in each directory crossed
     * (used when we want to return back to a previouws directory)*/
    int selected_item_history[MAX_DIR_LEVELS];

    int *dirfilter; /* file use */
    int filesindir; /* The number of files in the dircache */
    int dirsindir; /* file use */
    int dirlength; /* total number of entries in dir, incl. those not loaded */
    int sort_dir; /* directory sort order */
    int out_of_tree; /* shortcut from elsewhere */
    struct tree_cache cache;
    bool dirfull;
    bool is_browsing; /* valid browse context? */

    struct browse_context *browse;
};

/*
 * Call one of the two below after yields since the entrys may move inbetween */
struct entry* tree_get_entries(struct tree_context *t);
/* returns NULL on invalid index */
struct entry* tree_get_entry_at(struct tree_context *t, int index);

void tree_mem_init(void) INIT_ATTR;
void tree_init(void) INIT_ATTR;
int rockbox_browse(struct browse_context *browse);

void tree_lock_cache(struct tree_context *t);
void tree_unlock_cache(struct tree_context *t);

void reload_directory(void);
bool check_rockboxdir(void);
struct tree_context* tree_get_context(void);
void tree_flush(void);
void tree_restore(void);


#endif
