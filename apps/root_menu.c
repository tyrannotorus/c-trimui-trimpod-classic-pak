/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 Jonathan Gordon
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
#include "config.h"
#include "appevents.h"
#include "menu.h"
#include "root_menu.h"
#include "lang.h"
#include "settings.h"
#include "screens.h"
#include "kernel.h"
#include "debug.h"
#include "misc.h"
#include "sound.h"
#include "tdspeed.h"
#include "rolo.h"
#include "powermgmt.h"
#include "power.h"
#include "audio.h"
#include "dir.h"
#include "trimpod_folders.h"
#include "trimpod_library_browse.h"
#include "trimpod_playlists.h"
#include "trimpod_mainmenu.h"
#include "trimpod_page.h"          /* trimpod_home_pending (hold-BACK -> root) */
#include "trimpod_transition.h"    /* arm a single back-slide when homing */

int trimpod_settings_page(void);   /* main_menu.c */

/* gui api */
#include "list.h"
#include "splash.h"
#include "action.h"
#include "yesno.h"
#include "viewport.h"

#include "wps.h"
#include "playlist.h"
#include "playlist_viewer.h"
#include "menus/exported_menus.h"
#include "language.h"
#include "disk.h"

struct root_items {
    int (*function)(void* param);
    void* param;
};
static int next_screen = GO_TO_ROOT; /* holding info about the upcoming screen
                                        * which is the current screen for the
                                        * rest of the code after load_screen
                                        * is called */
static int last_screen = GO_TO_ROOT; /* unfortunatly needed so we can resume
                                        or goto current track based on previous
                                        screen */

static int previous_music = GO_TO_WPS; /* Toggles behavior of the return-to
                                        * playback-button depending
                                        * on FM radio */

static int wpsscrn(void* param)
{
    int ret_val = GO_TO_PREVIOUS;
    int audstatus = audio_status();
    (void)param;
    push_current_activity(ACTIVITY_WPS);


    if (audstatus)
    {
        ret_val = gui_wps_show();
    }
    else if (global_status.resume_index != -1)
    {
        DEBUGF("Resume index %d crc32 %lX offset %lX\n",
               global_status.resume_index,
               (unsigned long)global_status.resume_crc32,
               (unsigned long)global_status.resume_offset);
        if (playlist_resume() != -1)
        {
            playlist_resume_track(global_status.resume_index,
                global_status.resume_crc32,
                global_status.resume_elapsed,
                global_status.resume_offset);
            ret_val = gui_wps_show();
        }
    }
    else if (!file_exists(PLAYLIST_CONTROL_FILE))
    {
        splash(HZ*2, ID2P(LANG_NOTHING_TO_RESUME));
        trimpod_transition_suppress_next();   /* only a splash: don't re-slide the menu */
    }
    else if (yesno_pop(ID2P(LANG_REPLAY_FINISHED_PLAYLIST)) &&
             playlist_resume() != -1)
    {
        playlist_start(0, 0, 0);
        ret_val = gui_wps_show();
    }

    if (ret_val == GO_TO_PLAYLIST_VIEWER
        || ret_val == GO_TO_WPS
        || ret_val == GO_TO_PREVIOUS_MUSIC
        || ret_val == GO_TO_PREVIOUS_BROWSER
        || (ret_val == GO_TO_PREVIOUS
               && (last_screen == GO_TO_MAINMENU /* Settings */
                || last_screen == GO_TO_PLAYLISTS_SCREEN)))
    {
        pop_current_activity_without_refresh();
    }
    else
        pop_current_activity();

    return ret_val;
}


/* The Playlists screen is now the Trimpod playlists page (trimpod_playlists.c),
 * which lists the catalog + a "+ Add Playlist" row; see items[] below. */

/* True while the playlist viewer is being shown as the "file list" behind Now
 * Playing after a Play/Shuffle Playlist action (see playlist_view): backing out
 * of it returns to the Playlists list rather than the generic previous screen.
 * Cleared on that back-out and whenever we settle at the root menu. */
static bool pl_owns_viewer = false;

static int playlist_view(void * param)
{
    (void)param;
    char viewfile[MAX_PATH];

    /* A Play/Shuffle Playlist pick pending from the Playlists screen: start it
     * now and slide to Now Playing, leaving this viewer as the screen behind. */
    switch (trimpod_playlists_start_pending())
    {
        case 1:  pl_owns_viewer = true;  return GO_TO_WPS;
        case -1: return GO_TO_PLAYLISTS_SCREEN;   /* empty/failed: back to the list */
        default: break;                           /* 0: nothing pending -> show it */
    }

    /* Tap A on a playlist row: show its track listing.  Picking a track there
     * makes it the current playlist and plays from it (-> Now Playing, with
     * the current-queue viewer behind); B backs out to the Playlists list. */
    if (trimpod_playlists_take_pending_view(viewfile, sizeof(viewfile)))
    {
        switch (playlist_viewer_ex(viewfile, NULL))
        {
            case PLAYLIST_VIEWER_MAINMENU:
            case PLAYLIST_VIEWER_USB:
                pl_owns_viewer = false;
                return GO_TO_ROOT;
            case PLAYLIST_VIEWER_OK:        /* track picked: it's playing now */
                pl_owns_viewer = true;
                return GO_TO_WPS;
            default:                        /* B: back to the Playlists list */
                return GO_TO_PLAYLISTS_SCREEN;
        }
    }

    switch (playlist_viewer())
    {
        case PLAYLIST_VIEWER_MAINMENU:
        case PLAYLIST_VIEWER_USB:
            pl_owns_viewer = false;
            return GO_TO_ROOT;
        case PLAYLIST_VIEWER_OK:            /* select: to Now Playing */
            return GO_TO_WPS;
        default:                            /* B: leave the file list */
            if (pl_owns_viewer)
            {
                pl_owns_viewer = false;     /* our play flow: back to the list */
                return GO_TO_PLAYLISTS_SCREEN;
            }
            return GO_TO_PREVIOUS;
    }
}


/* Settings: run the Settings menu_page, then return to the root. */
static int trimpod_settings_screen(void *param)
{
    (void)param;
    trimpod_settings_page();
    return GO_TO_ROOT;
}

static const struct root_items items[] = {
    [GO_TO_FILEBROWSER] =   { trimpod_files_browse, NULL },
    [GO_TO_WPS] =           { wpsscrn, NULL },
    [GO_TO_MAINMENU] =      { trimpod_settings_screen, NULL },
    [GO_TO_PLAYLISTS_SCREEN] = { trimpod_playlists_screen, NULL },
    [GO_TO_PLAYLIST_VIEWER] = { playlist_view, NULL },
    [GO_TO_TRIMPOD_MUSIC] =     { trimpod_music_browse, NULL },
    [GO_TO_TRIMPOD_PODCASTS] =  { trimpod_podcast_browse, NULL },
    [GO_TO_TRIMPOD_AUDIOBOOKS] = { trimpod_audiobook_browse, NULL },
    [GO_TO_TRIMPOD_SHUFFLE] =   { trimpod_shuffle_all, NULL },
    [GO_TO_TRIMPOD_ARTISTS] =   { trimpod_library_browse, (void*)(intptr_t)TP_LIB_ARTISTS },
    [GO_TO_TRIMPOD_ALBUMS] =    { trimpod_library_browse, (void*)(intptr_t)TP_LIB_ALBUMS },
    [GO_TO_TRIMPOD_SONGS] =     { trimpod_library_browse, (void*)(intptr_t)TP_LIB_SONGS },
};
#define NUM_ITEMS (int)(sizeof(items)/sizeof(*items))

static int item_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list);

/* Trimpod: cleanly quit the app (returns to NextUI). sys_poweroff() does not
 * power off the hardware on this hosted/SDL target -- it tears down SDL and
 * exit()s the process, identical to the existing button quit. */
static int trimpod_exit(void)
{
    sys_poweroff();
    return 0;
}
MENUITEM_FUNCTION(exit_item, 0, ID2P(LANG_TRIMPOD_EXIT),
                        trimpod_exit, NULL, Icon_System_menu);

MENUITEM_RETURNVALUE_CHEVRON(file_browser, ID2P(LANG_TRIMPOD_BROWSE), GO_TO_FILEBROWSER,
                        item_callback, Icon_file_view_menu);

static char *get_wps_item_name(int selected_item, void * data,
                               char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    if (audio_status())
        return ID2P(LANG_NOW_PLAYING);
    return ID2P(LANG_RESUME_PLAYBACK);
}
MENUITEM_RETURNVALUE_DYNTEXT(wps_item, GO_TO_WPS, item_callback, get_wps_item_name,
                                NULL, NULL, Icon_Playback_menu);
MENUITEM_RETURNVALUE_CHEVRON(menu_, ID2P(LANG_SETTINGS), GO_TO_MAINMENU,
                        NULL, Icon_Submenu_Entered);
MENUITEM_RETURNVALUE_CHEVRON(playlists, ID2P(LANG_PLAYLISTS), GO_TO_PLAYLISTS_SCREEN,
                     item_callback, Icon_Playlist);
MENUITEM_RETURNVALUE_CHEVRON(music_item, ID2P(LANG_TRIMPOD_MUSIC), GO_TO_TRIMPOD_MUSIC,
                     item_callback, Icon_Audio);
MENUITEM_RETURNVALUE_CHEVRON(podcast_item, ID2P(LANG_TRIMPOD_PODCASTS), GO_TO_TRIMPOD_PODCASTS,
                     item_callback, Icon_Audio);
MENUITEM_RETURNVALUE_CHEVRON(audiobook_item, ID2P(LANG_TRIMPOD_AUDIOBOOKS), GO_TO_TRIMPOD_AUDIOBOOKS,
                     item_callback, Icon_Audio);
MENUITEM_RETURNVALUE_CHEVRON(artists_item, ID2P(LANG_TRIMPOD_ARTISTS), GO_TO_TRIMPOD_ARTISTS,
                     item_callback, Icon_Audio);
MENUITEM_RETURNVALUE_CHEVRON(albums_item, ID2P(LANG_TRIMPOD_ALBUMS), GO_TO_TRIMPOD_ALBUMS,
                     item_callback, Icon_Audio);
MENUITEM_RETURNVALUE_CHEVRON(songs_item, ID2P(LANG_TRIMPOD_SONGS), GO_TO_TRIMPOD_SONGS,
                     item_callback, Icon_Audio);
/* Shuffle All is an action (plays immediately), not a submenu -> no chevron. */
MENUITEM_RETURNVALUE(shuffle_item, ID2P(LANG_TRIMPOD_SHUFFLE_ALL), GO_TO_TRIMPOD_SHUFFLE,
                     item_callback, Icon_Audio);

struct menu_item_ex root_menu_;
static struct menu_callback_with_desc root_menu_desc = {
        item_callback, ID2P(LANG_ROCKBOX_TITLE), Icon_Rockbox };

static struct menu_table menu_table[] = {
    /* Order here represents the default ordering (Trimpod layout) */
    { "music", &music_item },
    { "podcasts", &podcast_item },
    { "audiobooks", &audiobook_item },
    { "artists", &artists_item },
    { "albums", &albums_item },
    { "songs", &songs_item },
    { "playlists", &playlists },
    { "files", &file_browser },
    { "settings", &menu_ },
    { "exit", &exit_item },
    { "shuffle", &shuffle_item },
    /* Trimpod: Resume Playback / Now Playing sits below Exit */
    { "wps", &wps_item },
};
#define MAX_MENU_ITEMS (sizeof(menu_table) / sizeof(struct menu_table))
static struct menu_item_ex *root_menu__[MAX_MENU_ITEMS];


void root_menu_load_from_cfg(void* setting, char *value)
{
    char *next = value, *start, *end;
    unsigned int menu_item_count = 0, i;
    bool main_menu_added = false;

    if (*value == '-')
    {
        root_menu_set_default(setting, NULL);
        return;
    }
    root_menu_.flags = MENU_HAS_DESC | MT_MENU;
    root_menu_.submenus = (const struct menu_item_ex **)&root_menu__;
    root_menu_.callback_and_desc = &root_menu_desc;

    while (next && menu_item_count < MAX_MENU_ITEMS)
    {
        start = next;
        next = strchr(next, ',');
        if (next)
        {
            *next = '\0';
            next++;
        }
        start = skip_whitespace(start);
        if ((end = strchr(start, ' ')))
            *end = '\0';
        for (i=0; i<MAX_MENU_ITEMS; i++)
        {
            if (*start && !strcmp(start, menu_table[i].string))
            {
                root_menu__[menu_item_count++] = (struct menu_item_ex *)menu_table[i].item;
                if (menu_table[i].item == &menu_)
                    main_menu_added = true;
                break;
            }
        }
    }
    if (!main_menu_added)
        root_menu__[menu_item_count++] = (struct menu_item_ex *)&menu_;
    root_menu_.flags |= MENU_ITEM_COUNT(menu_item_count);
    *(bool*)setting = true;
}

char* root_menu_write_to_cfg(void* setting, char*buf, int buf_len)
{
    (void)setting;
    unsigned i, written, j;
    for (i = 0; i < MENU_GET_COUNT(root_menu_.flags); i++)
    {
        for (j=0; j<MAX_MENU_ITEMS; j++)
        {
            if (menu_table[j].item == root_menu__[i])
            {
                written = snprintf(buf, buf_len, "%s, ", menu_table[j].string);
                buf_len -= written;
                buf += written;
                break;
            }
        }
    }
    return buf;
}

void root_menu_set_default(void* setting, void* defaultval)
{
    unsigned i;
    (void)defaultval;

    root_menu_.flags = MENU_HAS_DESC | MT_MENU;
    root_menu_.submenus = (const struct menu_item_ex **)&root_menu__;
    root_menu_.callback_and_desc = &root_menu_desc;

    for (i=0; i<MAX_MENU_ITEMS; i++)
    {
        root_menu__[i] = (struct menu_item_ex *)menu_table[i].item;
    }
    root_menu_.flags |= MENU_ITEM_COUNT(MAX_MENU_ITEMS);
    *(bool*)setting = false;
}

bool root_menu_is_changed(void* setting, void* defaultval)
{
    (void)defaultval;
    return *(bool*)setting;
}

static int item_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list)
{
    (void)this_list;
    switch (action)
    {
        case ACTION_TREE_STOP:
            return ACTION_REDRAW;
        case ACTION_REQUEST_MENUITEM:
            /* user-toggleable entries (Settings -> Main Menu) */
            if (this_item == &music_item)
            {
                if (!trimpod_mainmenu_is_enabled(TRIMPOD_MM_MUSIC))
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &podcast_item)
            {
                if (!trimpod_mainmenu_is_enabled(TRIMPOD_MM_PODCASTS))
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &audiobook_item)
            {
                if (!trimpod_mainmenu_is_enabled(TRIMPOD_MM_AUDIOBOOKS))
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &artists_item)
            {
                if (!trimpod_mainmenu_is_enabled(TRIMPOD_MM_ARTISTS))
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &albums_item)
            {
                if (!trimpod_mainmenu_is_enabled(TRIMPOD_MM_ALBUMS))
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &songs_item)
            {
                if (!trimpod_mainmenu_is_enabled(TRIMPOD_MM_SONGS))
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &playlists)
            {
                if (!trimpod_mainmenu_is_enabled(TRIMPOD_MM_PLAYLISTS))
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &file_browser)
            {
                if (!trimpod_mainmenu_is_enabled(TRIMPOD_MM_BROWSE))
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &wps_item)
            {
                /* Now Playing / Resume Playback: shown only when there is
                 * something to play or resume (audio active, a saved resume
                 * point, or a playlist control file to replay). */
                if (!audio_status()
                    && global_status.resume_index == -1
                    && !file_exists(PLAYLIST_CONTROL_FILE))
                    return ACTION_EXIT_MENUITEM;
            }
        break;
    }
    return action;
}

/* Root menu: the stock root_menu_ (built from menu_table, with chevrons and the
 * dynamic Now Playing / Resume label) rendered by the canonical do_menu, which
 * returns the chosen item's GO_TO_* into the dispatch loop.  Visibility follows
 * the Main Menu toggles + the recent-bookmarks setting; Settings, Exit and Now
 * Playing are always present.  B is inert at the root (do_menu keeps root_menu_
 * open on cancel); the very first screen at launch renders with no slide. */
static int trimpod_root_run(int last)
{
    /* Settled back at the root: any playlist-viewer ownership is stale now. */
    pl_owns_viewer = false;

    int n = 0;
#define ROOT_ADD(cond, itemp) \
    do { if (cond) root_menu__[n++] = (struct menu_item_ex *)(itemp); } while (0)
    ROOT_ADD(trimpod_mainmenu_is_enabled(TRIMPOD_MM_MUSIC),      &music_item);
    ROOT_ADD(trimpod_mainmenu_is_enabled(TRIMPOD_MM_PODCASTS),   &podcast_item);
    ROOT_ADD(trimpod_mainmenu_is_enabled(TRIMPOD_MM_AUDIOBOOKS), &audiobook_item);
    ROOT_ADD(trimpod_mainmenu_is_enabled(TRIMPOD_MM_ARTISTS),    &artists_item);
    ROOT_ADD(trimpod_mainmenu_is_enabled(TRIMPOD_MM_ALBUMS),     &albums_item);
    ROOT_ADD(trimpod_mainmenu_is_enabled(TRIMPOD_MM_SONGS),      &songs_item);
    ROOT_ADD(trimpod_mainmenu_is_enabled(TRIMPOD_MM_PLAYLISTS),  &playlists);
    ROOT_ADD(trimpod_mainmenu_is_enabled(TRIMPOD_MM_BROWSE),     &file_browser);
    ROOT_ADD(true, &menu_);          /* Settings  */
    ROOT_ADD(true, &exit_item);      /* Exit      */
    ROOT_ADD(trimpod_mainmenu_is_enabled(TRIMPOD_MM_SHUFFLE), &shuffle_item);
    ROOT_ADD(true, &wps_item);       /* Now Playing / Resume Playback */
#undef ROOT_ADD

    root_menu_.flags = MENU_HAS_DESC | MT_MENU | MENU_ITEM_COUNT(n);
    root_menu_.submenus = (const struct menu_item_ex **)&root_menu__;
    root_menu_.callback_and_desc = &root_menu_desc;

    /* highlight the row we came back from (matched by its GO_TO_* value) */
    int sel = 0;
    for (int i = 0; i < n; i++)
        if ((root_menu__[i]->flags & MENU_TYPE_MASK) == MT_RETURN_VALUE &&
            root_menu__[i]->value == last)
            { sel = i; break; }

    return do_menu(&root_menu_, &sel, NULL, false);
}

static inline int load_screen(int screen)
{
    /* set the global_status.last_screen before entering,
        if we dont we will always return to the wrong screen on boot */
    int old_previous = last_screen;
    int ret_val;
    enum current_activity activity = ACTIVITY_UNKNOWN;
    if (screen <= GO_TO_ROOT)
        return screen;
    if (screen == old_previous)
        old_previous = GO_TO_ROOT;
    global_status.last_screen = (char)screen;
    status_save(false);

    if (screen == GO_TO_MAINMENU)
        activity = ACTIVITY_SETTINGS;

    if (activity != ACTIVITY_UNKNOWN)
        push_current_activity(activity);

    ret_val = items[screen].function(items[screen].param);

    if (activity != ACTIVITY_UNKNOWN)
    {
        if (ret_val == GO_TO_WPS
            || ret_val == GO_TO_PREVIOUS_MUSIC
            || ret_val == GO_TO_PREVIOUS_BROWSER
            || ret_val == GO_TO_FILEBROWSER)
        {
            pop_current_activity_without_refresh();
        }
        else
            pop_current_activity();
    }

    last_screen = screen;
    if (ret_val == GO_TO_PREVIOUS)
        last_screen = old_previous;

    /* Shuffle is a one-shot action, not a re-enterable screen: once it has
     * started playback and handed off to the WPS, backing out of the WPS must
     * return to the root menu -- NOT resolve "previous" back to here and
     * re-shuffle.  Pin its "previous" to the root. */
    if (screen == GO_TO_TRIMPOD_SHUFFLE && ret_val == GO_TO_WPS)
        last_screen = GO_TO_ROOT;

    return ret_val;
}

static void ignore_back_button_stub(bool ignore)
{
    (void) ignore;
}

static int root_menu_setup_screens(void)
{
    return GO_TO_ROOT;   /* Trimpod: always start at the Main Menu */
}


void root_menu(void)
{
    int previous_browser = global_status.last_browser;

    push_current_activity(ACTIVITY_MAINMENU);
    next_screen = root_menu_setup_screens();

    while (true)
    {
        switch (next_screen)
        {
            case MENU_ATTACHED_USB:
            case MENU_SELECTED_EXIT:
                /* fall through */
            case GO_TO_ROOT:
                global_status.last_screen = GO_TO_ROOT; /* We've returned to ROOT */
                /* idle return to Now Playing: load the WPS forward from here (B
                 * from it comes back); if the music stopped meanwhile, stay */
                if (trimpod_wps_pending)
                {
                    trimpod_wps_pending = trimpod_home_pending = false;
                    if (audio_status() & AUDIO_STATUS_PLAY)
                    {
                        trimpod_transition_take_back();   /* drop the unwound screens' back slide */
                        pl_owns_viewer = false;
                        last_screen = GO_TO_ROOT;
                        next_screen = GO_TO_WPS;
                        break;
                    }
                }
                if (trimpod_home_pending)
                    trimpod_transition_arm_back();  /* home: one back-slide, as if
                                                     * the Main Menu were the prev
                                                     * screen (not the deep stack) */
                trimpod_home_pending = false;  /* home reached: stop unwinding */
                /* hardware BACK handled by HOST, not rockbox, at the root */
                ignore_back_button_stub(true);

                next_screen = trimpod_root_run(last_screen);

                ignore_back_button_stub(false);

                if (next_screen != GO_TO_PREVIOUS)
                    last_screen = GO_TO_ROOT;
                break;
            case GO_TO_FILEBROWSER:
            case GO_TO_PLAYLISTS_SCREEN:
                global_status.last_browser = previous_browser = next_screen;
                goto load_next_screen;
                break;

            case GO_TO_PREVIOUS:
            {
                next_screen = last_screen;
                if (last_screen == GO_TO_PREVIOUS)
                    next_screen = GO_TO_ROOT;
                break;
            }

            case GO_TO_PREVIOUS_BROWSER:
                next_screen = previous_browser;
                break;

            case GO_TO_PREVIOUS_MUSIC:
                next_screen = previous_music;
                break;
            default:
                goto load_next_screen;
                break;
        } /* switch() */
        continue;
load_next_screen: /* load_screen is inlined */
        next_screen = load_screen(next_screen);
    }

}
