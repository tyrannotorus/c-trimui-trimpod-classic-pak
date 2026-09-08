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
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "string-extra.h"
#include "panic.h"

#include "applimits.h"
#include "dir.h"
#include "file.h"
#include "lcd.h"
#include "kernel.h"
#include "usb.h"
#include "tree.h"
#include "settings.h"
#include "debug.h"
#include "storage.h"
#include "lang.h"
#include "screens.h"
#include "core_alloc.h"
#include "action.h"
#include "filetypes.h"
#include "misc.h"
#include "pathfuncs.h"
#include "filetree.h"

/* gui api */
#include "list.h"
#include "splash.h"
#include "appevents.h"

#include "root_menu.h"
#include "trimpod_transition.h"   /* slide the browser in/out */
#include "trimpod_page.h"         /* trimpod_home_pending (hold-BACK -> root) */

static struct gui_synclist tree_lists;

/* I put it here because other files doesn't use it yet,
 * but should be elsewhere since it will be used mostly everywhere */
static struct tree_context tc;

/* The browser uses a fixed default filter (tc.dirfilter points here; browse
 * contexts override it). */
static int default_dirfilter = TP_DIRFILTER;

char lastfile[MAX_PATH];
static char lastdir[MAX_PATH];

static bool reload_dir = false;


static int dirbrowse(void);

struct entry* tree_get_entries(struct tree_context *t)
{
    return core_get_data(t->cache.entries_handle);
}

struct entry* tree_get_entry_at(struct tree_context *t, int index)
{
    if(index < 0 || index >= t->cache.max_entries)
        return NULL; /* no entry */
    struct entry* entries = tree_get_entries(t);
    return &entries[index];
}

static struct entry *get_valid_entry(const char* funcname,
                                     struct tree_context *t, int index)
{
    struct entry *entry = tree_get_entry_at(t, index);
    if (!entry)
        panicf("Invalid tree entry %s", funcname);
    /*DEBUGF("%s tc: %x idx: %d\n", funcname, t, index);*/
    return entry;
}

static bool ext_stripit(bool isdir, int attr, int dirfilter)
{
    if (!isdir)
    {
        switch(TP_SHOW_FILENAME_EXT)
        {
            case 0:
                /* show file extension: off */
                return true;
                break;
            case 1:
                /* show file extension: on */
                break;
            case 2:
                /* show file extension: only unknown types */
                return filetype_supported(attr);
            case 3:
            default:
                /* show file extension: only when viewing all */
                return (dirfilter != SHOW_ALL);
        }
    }
    return false;
}

static const char* tree_get_filename(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    struct tree_context * local_tc=(struct tree_context *)data;
    char *name;
    int attr=0;
    {
        struct entry *entry = get_valid_entry(__func__, local_tc, selected_item);
        name = entry->name;
        attr = entry->attr;
    }

    if(ext_stripit((attr & ATTR_DIRECTORY), attr, *(local_tc->dirfilter)))
    {
        return(strip_extension(buffer, buffer_len, name));
    }
    return(name);
}

#ifdef HAVE_LCD_COLOR
static int tree_get_filecolor(int selected_item, void * data)
{
    struct tree_context * local_tc=(struct tree_context *)data;
    struct entry *entry = get_valid_entry(__func__, local_tc, selected_item);

    return filetype_get_color(entry->name, entry->attr);
}
#endif

bool check_rockboxdir(void)
{
    if(!dir_exists(ROCKBOX_DIR))
    {   /* No need to localise this message.
           If .rockbox is missing, it wouldn't work anyway */
        FOR_NB_SCREENS(i)
            screens[i].clear_display();
        splash(HZ*2, "No .rockbox directory");
        FOR_NB_SCREENS(i)
            screens[i].clear_display();
        splash(HZ*2, "Installation incomplete");
        return false;
    }
    return true;
}

/* do this really late in the init sequence */
void tree_init(void)
{
    check_rockboxdir();
    strcpy(tc.currdir, "/");
}

struct tree_context* tree_get_context(void)
{
    return &tc;
}

void tree_lock_cache(struct tree_context *t)
{
    core_pin(t->cache.name_buffer_handle);
    core_pin(t->cache.entries_handle);
}

void tree_unlock_cache(struct tree_context *t)
{
    core_unpin(t->cache.name_buffer_handle);
    core_unpin(t->cache.entries_handle);
}

/*
 * Returns the position of a given file in the current directory
 * returns -1 if not found
 */
static int tree_get_file_position(char * filename)
{
    int i, ret = -1;/* no file match, return undefined */

    tree_lock_cache(&tc);
    struct entry *entries = tree_get_entries(&tc);

    /* use lastfile to determine the selected item (default=0) */
    for (i=0; i < tc.filesindir; i++)
    {
        if (!strcmp(entries[i].name, filename))
        {
            ret = i;
            break;
        }
    }
    tree_unlock_cache(&tc);
    return(ret);
}

/*
 * Called when a new dir is loaded (for example when returning from other apps ...)
 * also completely redraws the tree
 */
static int update_dir(void)
{
    struct gui_synclist * const list = &tree_lists;
    int show_path_in_browser = TP_SHOW_PATH_IN_BROWSER;
    bool changed = false;

    const char* title = NULL;/* Must clear the title as the list is reused */
    int icon = NOICON;

    /* Ensure that list is initialized before update_dir returns */
    gui_synclist_init(list, &tree_get_filename, &tc, false, 1, NULL);

    {
        tc.sort_dir = TP_SORT_DIR;
        /* if the tc.currdir has been changed, reload it ...*/
        if (reload_dir || strncmp(tc.currdir, lastdir, sizeof(lastdir)))
        {
            if (ft_load(&tc, NULL) < 0)
                return -1;
            strmemccpy(lastdir, tc.currdir, MAX_PATH);
            changed = true;
        }
    }
    /* if selected item is undefined */
    if (tc.selected_item == -1)
    {
        /* use lastfile to determine the selected item */
        tc.selected_item = tree_get_file_position(lastfile);

        /* If the file doesn't exists, select the first one (default) */
        if(tc.selected_item < 0)
            tc.selected_item = 0;
        changed = true;
    }
    if (changed)
    {
        if (tc.dirfull)
        {
            splash(HZ, ID2P(LANG_SHOWDIR_BUFFER_FULL));
        }
    }

    if (show_path_in_browser == SHOW_PATH_FULL)
    {
        title = tc.currdir;
        icon = filetype_get_icon(ATTR_DIRECTORY);
    }
    else if (show_path_in_browser == SHOW_PATH_CURRENT)
    {
        title = strrchr(tc.currdir, '/');
        if (title != NULL)
        {
            title++; /* step past the separator */
            if (*title == '\0')
            {
                /* Display "Files" for the root dir */
                title = ID2P(LANG_DIR_BROWSER);
            }
            icon = filetype_get_icon(ATTR_DIRECTORY);
        }
    }

    /* set title and icon, if nothing is set, clear the title
     * with NULL and icon as NOICON as the list is reused */
    gui_synclist_set_title(list, P2STR((unsigned char*)title), icon);

    gui_synclist_set_nb_items(list, tc.filesindir);
#ifdef HAVE_LCD_COLOR
    gui_synclist_set_color_callback(list, &tree_get_filecolor);
#endif
    if( tc.selected_item >= tc.filesindir)
        tc.selected_item=tc.filesindir-1;

    gui_synclist_select_item(list, tc.selected_item);
    gui_synclist_draw(list);
    return tc.filesindir;
}

/* Force a reload of the directory next time directory browser is called */
void reload_directory(void)
{
    reload_dir = true;
}

static int exit_to_new_screen(int screen)
{
    gui_synclist_scroll_stop(&tree_lists);
    return screen;
}

/* main loop, handles key events */
/* render callback for the browser's slide-in: draw the dir (off-screen) and
 * stash its entry count for dirbrowse() to read back */
static int s_browse_numentries;
static void browse_first_render(void *ctx)
{
    (void)ctx;
    s_browse_numentries = update_dir();
}

static int dirbrowse(void)
{
    int numentries=0;
    int button;
    int oldbutton;
    bool reload_root = false;
    int lastfilter = *tc.dirfilter;
    bool lastsortcase = false;
    bool exit_func = false;

    char* currdir = tc.currdir; /* just a shortcut */
    if (tc.selected_item < 0)
        tc.selected_item = 0;

    /* slide the browser in: BACK if we got here by backing out of a deeper
     * screen that armed it (e.g. B out of Now Playing), else FORWARD */
    trimpod_transition_animate(
        trimpod_transition_take_back() ? TRIMPOD_TRANS_BACK : TRIMPOD_TRANS_FORWARD,
        browse_first_render, NULL);
    numentries = s_browse_numentries;
    reload_dir = false;
    if (numentries == -1)
        return exit_to_new_screen(GO_TO_PREVIOUS);  /* currdir is not a directory */

    if (*tc.dirfilter > NUM_FILTER_MODES && numentries==0)
    {
        splash(HZ*2, *tc.dirfilter == SHOW_M3U ?
                     ID2P(LANG_CATALOG_NO_PLAYLISTS) : ID2P(LANG_NO_FILES));
        return exit_to_new_screen(GO_TO_PREVIOUS);  /* No files found for rockbox_browse() */
    }

    while(tc.browse && tc.is_browsing) {
        bool restore = false;

        /* hold BACK anywhere: unwind this browser too and land on the Main Menu */
        if (trimpod_home_pending)
            return exit_to_new_screen(GO_TO_ROOT);

        if (tc.dirlevel < 0)
            tc.dirlevel = 0; /* shouldnt be needed.. this code needs work! */

        int dirlevel_before = tc.dirlevel;  /* detect a folder change for the slide */

        keyclick_set_callback(gui_synclist_keyclick_callback, &tree_lists);
        button = get_action(CONTEXT_TREE|ALLOW_SOFTLOCK,
                            list_do_action_timeout(&tree_lists, HZ/2));
        oldbutton = button;
        gui_synclist_do_button(&tree_lists, &button);
        tc.selected_item = gui_synclist_get_sel_pos(&tree_lists);
        if (button == ACTION_NONE)
            trimpod_idle_to_wps();          /* the loop-top home check exits */
        bool do_restore_display = true;
        switch ( button ) {
            case ACTION_STD_OK:
                /* nothing to do if no files to display */
                if ( numentries == 0 )
                    break;
                ft_enter(&tc);
                restore = do_restore_display;
                break;

            case ACTION_TP_HOME:        /* hold BACK: jump to the Main Menu */
                trimpod_home_pending = true;
                return exit_to_new_screen(GO_TO_ROOT);

            case ACTION_STD_CANCEL:
                exit_to_new_screen(0);
                if (*tc.dirfilter > NUM_FILTER_MODES && tc.dirlevel < 1) {
                    exit_func = true;
                    break;
                }
                if (!strcmp(currdir, "/"))
                {
                    if (oldbutton == ACTION_TREE_PGLEFT)
                        break;
                    else
                        return exit_to_new_screen(GO_TO_ROOT);
                }

                    if (ft_exit(&tc) == 3)
                        exit_func = true;

                restore = do_restore_display;
                break;

            case ACTION_TREE_STOP:
                if (list_stop_handler())
                    restore = do_restore_display;
                break;

            case ACTION_STD_MENU:
                return exit_to_new_screen(GO_TO_ROOT);
                break;

            case ACTION_TREE_WPS:
                return exit_to_new_screen(GO_TO_PREVIOUS_MUSIC);
                break;

            default:
                if (default_event_handler(button) == SYS_USB_CONNECTED)
                {
                    if(*tc.dirfilter > NUM_FILTER_MODES)
                        /* leave sub-browsers after usb, doing otherwise
                           might be confusing to the user */
                        exit_func = true;
                    else
                        reload_dir = true;
                }
                break;
        }
        if (button && !IS_SYSEVENT(button))
        {
            storage_spin();
        }


    check_rescan:
        /* do we need to rescan dir? */
        if (reload_dir || reload_root ||
            lastfilter != *tc.dirfilter ||
            lastsortcase != false)
        {
            if (reload_root) {
                strcpy(currdir, "/");
                tc.dirlevel = 0;
                reload_root = false;
            }

            if (!reload_dir)
            {
                gui_synclist_select_item(&tree_lists, 0);
                gui_synclist_draw(&tree_lists);
                tc.selected_item = 0;
                lastdir[0] = 0;
            }

            lastfilter = *tc.dirfilter;
            lastsortcase = false;
            restore = do_restore_display;
        }

        if (exit_func)
            return exit_to_new_screen(GO_TO_PREVIOUS);

        if (restore || reload_dir) {
            FOR_NB_SCREENS(i)
                screens[i].scroll_stop();
            /* restore display.  A folder change (dirlevel moved) slides like the
             * menus -- deeper = forward, parent = back; an in-place reload
             * (context menu, filter, ...) just redraws. */
            if (tc.dirlevel != dirlevel_before) {
                trimpod_transition_animate(
                    tc.dirlevel > dirlevel_before ? TRIMPOD_TRANS_FORWARD
                                                  : TRIMPOD_TRANS_BACK,
                    browse_first_render, NULL);
                numentries = s_browse_numentries;
            } else {
                numentries = update_dir();
            }
            reload_dir = false;
            if (currdir[1] && (numentries < 0))
            {   /* not in root and reload failed */
                reload_root = true; /* try root */
                goto check_rescan;
            }
        }
    }
    return exit_to_new_screen(GO_TO_ROOT);
}

/* Run the stock browser over `browse` (a Settings sub-browser: one fixed
 * dirfilter above NUM_FILTER_MODES); returns the GO_TO_* it exits with. */
int rockbox_browse(struct browse_context *browse)
{
    int dirfilter = browse->dirfilter;
    int *prev_dirfilter = tc.dirfilter;

    tc.is_browsing = true;
    tc.dirfilter = &dirfilter;
    tc.sort_dir = TP_SORT_DIR;
    reload_dir = true;

    /* don't reset if its the same browse already loaded */
    if (tc.browse != browse ||
        !(tc.currdir[1] && strstr(tc.currdir, browse->root) != NULL))
    {
        tc.browse = browse;
        tc.selected_item = 0;
        tc.dirlevel = 0;
        strmemccpy(tc.currdir, browse->root, sizeof(tc.currdir));
    }

    int ret_val = dirbrowse();

    tc.is_browsing = false;
    tc.dirfilter = prev_dirfilter; /* Bugfix restore dirfilter*/

    /* backing out -> tell the menu/page we return to slide us away */
    if (ret_val == GO_TO_PREVIOUS || ret_val == GO_TO_ROOT)
        trimpod_transition_arm_back();
    return ret_val;
}

static int move_callback(int handle, void* current, void* new)
{
    struct tree_cache* cache = &tc.cache;
    ptrdiff_t diff = new - current;
    /* FIX_PTR makes sure to not accidentally update static allocations */
#define FIX_PTR(x) \
    { if ((void*)x >= current && (void*)x < (current+cache->name_buffer_size)) x+= diff; }

    if (handle == cache->name_buffer_handle)
    {   /* update entry structs, *even if they are struct tagentry */
        struct entry *this = core_get_data(cache->entries_handle);
        struct entry *last = this + cache->max_entries;
        for(; this < last; this++)
            FIX_PTR(this->name);
    }
    /* nothing to do if entries moved */
    return BUFLIB_CB_OK;
}

static struct buflib_callbacks ops = {
    .move_callback = move_callback,
    .shrink_callback = NULL,
};

void tree_mem_init(void)
{
    /* initialize tree context struct */
    struct tree_cache* cache = &tc.cache;
    memset(&tc, 0, sizeof(tc));
    tc.dirfilter = &default_dirfilter;
    tc.sort_dir = TP_SORT_DIR;

    cache->name_buffer_size = AVERAGE_FILENAME_LENGTH *
        TRIMPOD_MAX_FILES_IN_DIR;
    cache->name_buffer_handle = core_alloc_ex(cache->name_buffer_size, &ops);

    cache->max_entries = TRIMPOD_MAX_FILES_IN_DIR;
    cache->entries_handle =
            core_alloc_ex(cache->max_entries*(sizeof(struct entry)), &ops);
}

/* These two functions are called by the USB and shutdown handlers */
void tree_flush(void)
{
     tc.is_browsing = false;/* clear browse to prevent reentry to a possibly missing file */


}

void tree_restore(void)
{


}
