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

/* Trimpod: themes are hardcoded to the iPod-style skin, so there is no Theme
 * Settings menu here.  browse_folder() remains because the .cfg config browser
 * and the EQ presets browser use it. */

#include <stddef.h>
#include "config.h"
#include "lang.h"
#include "settings.h"           /* ID2P */
#include "dir.h"
#include "tree.h"
#include "root_menu.h"
#include "exported_menus.h"
#include "splash.h"

int browse_folder(void *param)
{
    const struct browse_folder_info *info = param;
    struct browse_context browse = {
        .dirfilter = info->show_options,
        .root = info->dir,
    };

    if (!dir_exists(info->dir)) {
        splash(HZ, ID2P(LANG_PLAYLIST_DIRECTORY_ACCESS_ERROR));
        return GO_TO_PREVIOUS;
    }

    tree_get_context()->browse = NULL;  /*bugfix - force root dir reload */
    return rockbox_browse(&browse);
}
