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
#include "font.h"
#include "button.h"
#include "kernel.h"
#include "usb.h"
#include "tree.h"
#include "audio.h"
#include "playlist.h"
#include "menu.h"
#include "skin_engine/skin_engine.h"
#include "settings.h"
#include "debug.h"
#include "storage.h"
#include "rolo.h"
#include "icons.h"
#include "lang.h"
#include "screens.h"
#include "keyboard.h"
#include "onplay.h"
#include "core_alloc.h"
#include "power.h"
#include "action.h"
#include "filetypes.h"
#include "misc.h"
#include "pathfuncs.h"
#include "filetree.h"
#include "rtc.h"
#include "dircache.h"
#include "yesno.h"
#include "eeprom_settings.h"
#include "playlist_catalog.h"

/* gui api */
#include "list.h"
#include "splash.h"
#include "shortcuts.h"
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

static bool start_wps = false;
static int curr_context = false;/* id3db or tree*/

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
    if((dirfilter != SHOW_ID3DB) && !isdir)
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
    if (*tc.dirfilter == SHOW_ID3DB)
        return -1;
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

    const bool id3db = false;

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
        if (!id3db)
            /* use lastfile to determine the selected item */
            tc.selected_item = tree_get_file_position(lastfile);

        /* If the file doesn't exists, select the first one (default) */
        if(tc.selected_item < 0)
            tc.selected_item = 0;
        changed = true;
    }
    if (changed)
    {
        if( !id3db && tc.dirfull )
        {
            splash(HZ, ID2P(LANG_SHOWDIR_BUFFER_FULL));
        }
    }

    {
        if (tc.browse && tc.browse->title)
        {
            title = tc.browse->title;
            icon = tc.browse->icon;
            if (icon == NOICON)
                icon = filetype_get_icon(ATTR_DIRECTORY);
            /* display sub directories in the title of plugin browser */
            if (tc.dirlevel > 0 && *tc.dirfilter == SHOW_PLUGINS)
            {
                char *subdir = strrchr(tc.currdir, '/');
                if (subdir != NULL)
                    title = subdir + 1; /* step past the separator */
            }
        }
        else
        {
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

/* load tracks from specified directory to resume play */
void resume_directory(const char *dir)
{
    int dirfilter = *tc.dirfilter;
    int ret;
    const bool id3db = false;
    /* make sure the dirfilter is sane. The only time it should be possible
     * thats its not is when resume playlist is called from a plugin
     */
    if (!id3db)
        *tc.dirfilter = TP_DIRFILTER;
    ret = ft_load(&tc, dir);
    *tc.dirfilter = dirfilter;
    if (ret < 0)
        return;
    lastdir[0] = 0;

    ft_build_playlist(&tc, 0);
}

/* Returns the current working directory and also writes cwd to buf if
   non-NULL.  In case of error, returns NULL. */
char *getcwd(char *buf, getcwd_size_t size)
{
    if (!buf)
        return tc.currdir;
    else if (size)
    {
        if (strmemccpy(buf, tc.currdir, size) != NULL)
            return buf;
    }
    /* size == 0, or truncation in strmemccpy */
    return NULL;
}

/* Force a reload of the directory next time directory browser is called */
void reload_directory(void)
{
    reload_dir = true;
}

char* get_current_file(char* buffer, size_t buffer_len)
{
    struct entry *entry = tree_get_entry_at(&tc, tc.selected_item);
    if (entry && getcwd(buffer, buffer_len))
    {
        if (!tc.dirlength)
            return buffer;

        size_t usedlen = strlen(buffer);

        if (usedlen + 2 < buffer_len) /* ensure enough room for '/' + '\0' */
        {
            if (buffer[usedlen-1] != '/')
            {
                buffer[usedlen] = '/';
                /* strmemccpy will zero terminate if we run out of space after */
                usedlen++;
            }
            buffer_len -= usedlen;
            if (strmemccpy(buffer + usedlen, entry->name, buffer_len) != NULL)
                return buffer;
        }
    }
    return NULL;
}

/* Allow apps to change our dirfilter directly (required for sub browsers)
   if they're suddenly going to become a file browser for example */
void set_dirfilter(int l_dirfilter)
{
    *tc.dirfilter = l_dirfilter;
}

/* Selects a path + file and update tree context properly */
static void set_current_file_ex(const char *path, const char *filename)
{
    int i;

    if (!filename) /* path and filename supplied combined */
    {
        /* separate directory from filename */
        /* gets the directory's name and put it into tc.currdir */
        filename = strrchr(path+1,'/');
        size_t endpos = filename - path;
        if (filename && endpos < MAX_PATH - 1)
        {
            strmemccpy(tc.currdir, path, endpos + 1);
            filename++;
        }
        else
        {
            strcpy(tc.currdir, "/");
            filename = path+1;
        }
    }
    else /* path and filename came in separate ensure an ending '/' */
    {
        char *end_p = strmemccpy(tc.currdir, path, MAX_PATH);
        size_t endpos = end_p - tc.currdir;
        if (endpos < MAX_PATH)
        {
            if (tc.currdir[endpos - 2] != '/')
            {
                tc.currdir[endpos - 1] = '/';
                tc.currdir[endpos] = '\0';
            }
        }
    }
    strmemccpy(lastfile, filename, MAX_PATH);


    /* If we changed dir we must recalculate the dirlevel
       and adjust the selected history properly */
    if (strncmp(tc.currdir,lastdir,sizeof(lastdir)))
    {
        tc.dirlevel =  0;
        tc.selected_item_history[tc.dirlevel] = -1;

        /* use '/' to calculate dirlevel */
        for (i = 1; path[i] != '\0'; i++)
        {
            if (path[i] == '/')
            {
                tc.dirlevel++;
                tc.selected_item_history[tc.dirlevel] = -1;
            }
        }
    }
    if (ft_load(&tc, NULL) >= 0)
    {
        tc.selected_item = tree_get_file_position(lastfile);
        if (!tc.is_browsing && tc.out_of_tree == 0)
        {
            /* the browser is closed */
            /* don't allow the previous items to overwrite what we just loaded */
            tc.out_of_tree = tc.selected_item + 1;
        }
    }
}

/* Selects a file and update tree context properly */
void set_current_file(const char *path)
{
    set_current_file_ex(path, NULL);
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
    char buf[MAX_PATH];
    int button;
    int oldbutton;
    bool reload_root = false;
    int lastfilter = *tc.dirfilter;
    bool lastsortcase = false;
    bool exit_func = false;

    char* currdir = tc.currdir; /* just a shortcut */
        curr_context=CONTEXT_TREE;
    if (tc.selected_item < 0)
        tc.selected_item = 0;

    start_wps = false;
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
        int customaction = ONPLAY_NO_CUSTOMACTION;
        bool do_restore_display = true;
        switch ( button ) {
            case ACTION_STD_OK:
                /* nothing to do if no files to display */
                if ( numentries == 0 )
                    break;
                if (tc.browse->flags & BROWSE_SELECTONLY)
                {
                    struct entry *entry =
                                get_valid_entry(__func__, &tc, tc.selected_item);
                    short attr = entry->attr;
                    if(!(attr & ATTR_DIRECTORY))
                    {
                        tc.browse->flags |= BROWSE_SELECTED;
                        get_current_file(tc.browse->buf, tc.browse->bufsize);
                        return exit_to_new_screen(GO_TO_PREVIOUS);
                    }
                }
                switch (ft_enter(&tc))
                {
                    case GO_TO_FILEBROWSER: reload_dir = true; break;
                    case GO_TO_WPS:
                        return exit_to_new_screen(GO_TO_WPS);
                    case GO_TO_ROOT: exit_func = true; break;
                    default:
                        break;
                }
                restore = do_restore_display;
                break;

            case ACTION_TP_HOME:        /* hold BACK: jump to the Main Menu */
                trimpod_home_pending = true;
                return exit_to_new_screen(GO_TO_ROOT);

            case ACTION_STD_CANCEL:
                exit_to_new_screen(0);
                if ((*tc.dirfilter > NUM_FILTER_MODES ||
                     (tc.browse && (tc.browse->flags & BROWSE_NO_UPDIR)))
                    && tc.dirlevel < 1) {
                    exit_func = true;
                    break;
                }
                if ((*tc.dirfilter == SHOW_ID3DB && tc.dirlevel == 0) ||
                    ((*tc.dirfilter != SHOW_ID3DB && !strcmp(currdir,"/"))))
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

#ifdef HAVE_HOTKEY
            case ACTION_TREE_HOTKEY:
                if (!global_settings.hotkey_tree)
                    break;
                /* fall through */
#endif
            case ACTION_STD_CONTEXT:
            {
                bool hotkey = button == ACTION_TREE_HOTKEY;
                int onplay_result;
                int attr = 0;

                if (tc.browse->flags & BROWSE_NO_CONTEXT_MENU)
                    break;

                if(!numentries)
                    onplay_result = onplay(NULL, 0, curr_context, hotkey, customaction);
                else {
                    {
                        struct entry *entry =
                               get_valid_entry(__func__, &tc, tc.selected_item);

                        attr = entry->attr;

                        ft_assemble_path(buf, sizeof(buf), currdir, entry->name);

                    }
                    onplay_result = onplay(buf, attr, curr_context, hotkey, customaction);
                }
                switch (onplay_result)
                {
                    case ONPLAY_MAINMENU:
                        return exit_to_new_screen(GO_TO_ROOT);
                        break;

                    case ONPLAY_OK:
                        restore = do_restore_display;
                        break;

                    case ONPLAY_RELOAD_DIR:
                        reload_dir = true;
                        break;

                    case ONPLAY_START_PLAY:
                        return exit_to_new_screen(GO_TO_WPS);
                        break;
                }
                break;
            }

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
        if (start_wps)
            return exit_to_new_screen(GO_TO_WPS);
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

int create_playlist(void)
{
    bool ret;
    trigger_cpu_boost();
    ret = catalog_add_to_a_playlist(PATH_ROOTSTR, ATTR_DIRECTORY, true, NULL, NULL);
    cancel_cpu_boost();

    return (ret) ? 1 : 0;
}

#define NUM_TC_BACKUP   3
static struct tree_context backups[NUM_TC_BACKUP];
/* do not make backup if it is not recursive call */
static int backup_count = -1;
int rockbox_browse(struct browse_context *browse)
{
    tc.is_browsing = (browse != NULL);
    int ret_val = 0;
    int dirfilter = SHOW_ALL;
    if (tc.is_browsing)
        dirfilter = browse->dirfilter;
    else
    {
        DEBUGF("%s browse is [NULL] \n", __func__);
        browse = tc.browse;
    }
    if (backup_count >= NUM_TC_BACKUP)
        return GO_TO_PREVIOUS;
    if (backup_count >= 0)
        backups[backup_count] = tc;
    backup_count++;
    int *prev_dirfilter = tc.dirfilter;
    tc.dirfilter = &dirfilter;
    tc.sort_dir = TP_SORT_DIR;

    reload_dir = true;

    if (tc.out_of_tree > 0)
    {
        /* an item has already been loaded out_of_tree holds the selected index
         * what happens with the item is dependent on the browse context */
        tc.selected_item = tc.out_of_tree - 1;
        tc.out_of_tree = 0;
        ret_val = ft_enter(&tc);
    }
    else
    {
        if (*tc.dirfilter >= NUM_FILTER_MODES)
        {
            int last_context;
            /* don't reset if its the same browse already loaded */
            if (tc.browse != browse ||
                !(tc.currdir[1] && strstr(tc.currdir, browse->root) != NULL))
            {
                tc.browse = browse;
                tc.selected_item = 0;
                tc.dirlevel = 0;

                strmemccpy(tc.currdir, browse->root, sizeof(tc.currdir));
            }

            start_wps = false;
            last_context = curr_context;

            if (browse->selected)
            {
                set_current_file_ex(browse->root, browse->selected);
                /* set_current_file changes dirlevel, change it back */
                tc.dirlevel = 0;
            }

            ret_val = dirbrowse();
            curr_context = last_context;
        }
        else
        {
            if (dirfilter != SHOW_ID3DB && (browse->flags & BROWSE_DIRFILTER) == 0)
                tc.dirfilter = &default_dirfilter;
            tc.browse = browse;
            if (browse->flags & BROWSE_NO_UPDIR)
            {
                /* Trimpod: start inside the root itself rather than treating it
                 * as a file to highlight within its parent directory. */
                tc.selected_item = 0;
                tc.dirlevel = 0;
                strmemccpy(tc.currdir, browse->root, sizeof(tc.currdir));
            }
            else
                set_current_file(browse->root);
            if (browse->flags&BROWSE_RUNFILE)
                ret_val = ft_enter(&tc);
            else
                ret_val = dirbrowse();
        }
    }

    tc.is_browsing = false;
    tc.dirfilter = prev_dirfilter; /* Bugfix restore dirfilter*/

    backup_count--;
    if (backup_count >= 0)
        tc = backups[backup_count];

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

bool bookmark_play(char *resume_file, int index, unsigned long elapsed,
                   unsigned long offset, int seed, char *filename)
{
    int i;
    char* suffix = strrchr(resume_file, '.');
    bool started = false;

    if (suffix != NULL && !strncasecmp(suffix, ".m3u", sizeof(".m3u") - 1)) /* gets m3u8 too */
    {
        /* Playlist playback */
        char* slash;
        /* check that the file exists */
        if (!file_exists(resume_file))
            return false;

        slash = strrchr(resume_file,'/');
        if (slash)
        {
            char* cp;
            *slash=0;

            cp=resume_file;
            if (!cp[0])
                cp="/";

            if (playlist_create(cp, slash+1) != -1)
            {
                if (global_settings.playlist_shuffle)
                    playlist_shuffle(seed, -1);
                started = true;
            }
            *slash='/';
        }
    }
    else
    {
        /* Directory playback */
        lastdir[0]='\0';
        if (playlist_create(resume_file, NULL) != -1)
        {
            char filename_buf[MAX_PATH + 1];
            const char* peek_filename;
            resume_directory(resume_file);
            if (global_settings.playlist_shuffle)
                playlist_shuffle(seed, -1);

            /* Check if the file is at the same spot in the directory,
               else search for it */
            int amt = playlist_amount();
            for ( i=0; i < amt; i++ )
            {
                int modidx = (i + index) % amt;
                peek_filename = playlist_peek(modidx, filename_buf,
                    sizeof(filename_buf));

                if (peek_filename == NULL)
                {
                    if (index == 0) /* searched every entry didn't find a match */
                        return false;
                    /* playlist has shrunk, search from the top */
                    i = 0;
                    amt = index;
                    index = 0;
                }
                else if (!strcmp(strrchr(peek_filename, '/') + 1, filename))
                {
                    started = true;
                    index = modidx;
                    break;
                }
            }
        }
    }

    if (started)
    {
        playlist_start(index, elapsed, offset);
        start_wps = true;
    }
    return started;
}

/* These two functions are called by the USB and shutdown handlers */
void tree_flush(void)
{
     tc.is_browsing = false;/* clear browse to prevent reentry to a possibly missing file */


}

void tree_restore(void)
{


}
