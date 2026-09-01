/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Trimpod: the Settings -> Library menu -- the Music / Podcast / Audiobook
 * source-folder lists plus a Rescan Library action, on a nested do_menu page.
 * Each folder chevron row opens its existing management page unchanged.
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

#include <stddef.h>
#include "config.h"
#include "lang.h"
#include "settings.h"        /* ID2P */
#include "menu.h"
#include "trimpod_folders.h"
#include "trimpod_library.h"     /* trimpod_library_reconcile (Rescan) */
#include "exported_menus.h"

MENUITEM_FUNCTION(tp_af_music, MENU_SHOW_CHEVRON, ID2P(LANG_TRIMPOD_FOLDERS),
                  trimpod_music_settings, NULL, Icon_NOICON);
MENUITEM_FUNCTION(tp_af_podcast, MENU_SHOW_CHEVRON, ID2P(LANG_TRIMPOD_PODCAST_FOLDERS),
                  trimpod_podcast_settings, NULL, Icon_NOICON);
MENUITEM_FUNCTION(tp_af_audiobook, MENU_SHOW_CHEVRON, ID2P(LANG_TRIMPOD_AUDIOBOOK_FOLDERS),
                  trimpod_audiobook_settings, NULL, Icon_NOICON);

/* Rescan Library: rebuild the SQLite tag index from the source folders, behind
 * the reconcile's own Scanning screen. */
static int trimpod_library_rescan(void)
{
    trimpod_library_reconcile(true);
    return 0;
}
MENUITEM_FUNCTION(tp_af_rescan, 0, ID2P(LANG_TRIMPOD_RESCAN),
                  trimpod_library_rescan, NULL, Icon_NOICON);

MAKE_MENU(trimpod_audio_folders_menu, ID2P(LANG_TRIMPOD_LIBRARY), NULL,
          Icon_Submenu_Entered, &tp_af_music, &tp_af_podcast, &tp_af_audiobook,
          &tp_af_rescan);

int trimpod_audio_folders_page(void)
{
    do_menu(&trimpod_audio_folders_menu, NULL, NULL, false);
    return 0;
}
