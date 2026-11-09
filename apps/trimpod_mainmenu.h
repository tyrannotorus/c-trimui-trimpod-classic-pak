/***************************************************************************
 * Trimpod: Main Menu Settings -- toggle which root-menu entries are shown.
 *
 * A small set of root-menu entries (Music, Playlists, Browse) can be hidden
 * or shown by the user.  State is persisted in ROCKBOX_DIR/mainmenu.txt and
 * queried by root_menu.c's item_callback (ACTION_REQUEST_MENUITEM) to filter
 * the live menu, exactly like the bookmarks entry keys off global_settings.
 ****************************************************************************/
#ifndef _TRIMPOD_MAINMENU_H
#define _TRIMPOD_MAINMENU_H

#include <stdbool.h>

enum trimpod_mainmenu_id
{
    TRIMPOD_MM_MUSIC = 0,
    TRIMPOD_MM_PODCASTS,
    TRIMPOD_MM_AUDIOBOOKS,
    TRIMPOD_MM_ARTISTS,
    TRIMPOD_MM_ALBUMS,
    TRIMPOD_MM_SONGS,
    TRIMPOD_MM_PLAYLISTS,
    TRIMPOD_MM_BROWSE,
    TRIMPOD_MM_SHUFFLE,
    TRIMPOD_MM_MOVIES,
    TRIMPOD_MM_TELEVISION,
    TRIMPOD_MM_COUNT,
};

/* True if the given root-menu entry should be shown. */
bool trimpod_mainmenu_is_enabled(enum trimpod_mainmenu_id id);

/* Settings -> Main Menu Settings page (MENUITEM_FUNCTION target). */
int trimpod_mainmenu_settings(void);

#endif /* _TRIMPOD_MAINMENU_H */
