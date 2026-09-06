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
#ifndef __ROOT_MENU_H__
#define __ROOT_MENU_H__

#include "config.h"
#include "gcc_extensions.h"

void root_menu(void) NORETURN_ATTR;
struct menu_table {
    char *string;
    const struct menu_item_ex *item;
};


enum {
    /* from old menu api, but still required*/
    MENU_ATTACHED_USB = -10,
    MENU_SELECTED_EXIT = -9,

    GO_TO_ROOTITEM_CONTEXT = -5,
    GO_TO_PREVIOUS_MUSIC = -4,
    GO_TO_PREVIOUS_BROWSER = -3,
    GO_TO_PREVIOUS = -2,
    GO_TO_ROOT = -1,
    GO_TO_FILEBROWSER = 0,
    GO_TO_WPS,
    GO_TO_MAINMENU,
    GO_TO_RECENTBMARKS,
    /* Do Not add any items above here unless you want it to be able to
       be the "start screen" after a boot up. The setting in settings_list.c
       will need editing if this is the case. */
    GO_TO_TIMESCREEN,
    GO_TO_PLAYLISTS_SCREEN,
    GO_TO_PLAYLIST_VIEWER,
    GO_TO_SYSTEM_SCREEN,
    GO_TO_TRIMPOD_MUSIC,
    GO_TO_TRIMPOD_PODCASTS,
    GO_TO_TRIMPOD_AUDIOBOOKS,
    GO_TO_TRIMPOD_SHUFFLE,
    GO_TO_TRIMPOD_ARTISTS,
    GO_TO_TRIMPOD_ALBUMS,
    GO_TO_TRIMPOD_SONGS,
};
#ifndef PLUGIN
extern struct menu_item_ex root_menu_;

void root_menu_load_from_cfg(void* setting, char *value);
char* root_menu_write_to_cfg(void* setting, char*buf, int buf_len);
void root_menu_set_default(void* setting, void* defaultval);
bool root_menu_is_changed(void* setting, void* defaultval);
#endif


#endif /* __ROOT_MENU_H__ */
