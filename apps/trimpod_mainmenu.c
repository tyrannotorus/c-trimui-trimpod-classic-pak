/***************************************************************************
 * Trimpod: Main Menu Settings (see trimpod_mainmenu.h).
 *
 * A do_menu of inline value rows, one per toggleable root-menu entry, with a
 * "[x]"/"[ ]" toggle shown right-aligned through do_menu's value indicator.
 * A / LEFT / RIGHT flip the highlighted row, B leaves.  State persists in
 * ROCKBOX_DIR/mainmenu.txt and is read by root_menu.c to hide/show the entries.
 ****************************************************************************/
#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "file.h"
#include "settings.h"
#include "lang.h"
#include "action.h"
#include "screens.h"
#include "gui/list.h"
#include "icon.h"
#include "misc.h"
#include "trimpod_mainmenu.h"
#include "trimpod_ui.h"   /* trimpod_toggle_str (shared [x]/[ ] glyph) */
#include "menu.h"

#define MAINMENU_FILE   ROCKBOX_DIR "/mainmenu.txt"

/* persisted key + display label for each toggle, indexed by trimpod_mainmenu_id */
static const char *const mm_keys[TRIMPOD_MM_COUNT] = {
    "music", "podcasts", "audiobooks", "artists", "albums", "songs",
    "playlists", "browse", "shuffle",
};

/* Shipped default when there is no saved mainmenu.txt: Music + Playlists
 * (Podcasts / Audiobooks / Artists / Albums / Songs / Browse / Shuffle off),
 * matching the pared-down music player. */
static const bool mm_default[TRIMPOD_MM_COUNT] = {
    true, false, false, false, false, false, true, false, false,
};
static bool mm_enabled[TRIMPOD_MM_COUNT];
static bool mm_loaded;

/* ---- storage ---------------------------------------------------------- */

static void trimpod_mainmenu_load(void)
{
    /* no saved file: fall back to the shipped default-on set */
    for (int i = 0; i < TRIMPOD_MM_COUNT; i++)
        mm_enabled[i] = mm_default[i];
    mm_loaded = true;

    int fd = open(MAINMENU_FILE, O_RDONLY);
    if (fd < 0)
        return;                 /* first run: keep the all-on defaults */

    char rbuf[256];
    int len = read(fd, rbuf, sizeof(rbuf) - 1);
    close(fd);
    if (len <= 0)
        return;
    rbuf[len] = '\0';

    char *line = rbuf, *nl;
    while (line && *line)
    {
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (eq)
        {
            *eq = '\0';
            for (int i = 0; i < TRIMPOD_MM_COUNT; i++)
                if (!strcmp(line, mm_keys[i]))
                    mm_enabled[i] = (eq[1] != '0');
        }
        line = nl ? nl + 1 : NULL;
    }
}

static void mm_save(void)
{
    int fd = open(MAINMENU_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd < 0)
        return;
    for (int i = 0; i < TRIMPOD_MM_COUNT; i++)
    {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "%s=%d\n", mm_keys[i],
                         mm_enabled[i] ? 1 : 0);
        write(fd, buf, n);
    }
    close(fd);
}

bool trimpod_mainmenu_is_enabled(enum trimpod_mainmenu_id id)
{
    if (!mm_loaded)
        trimpod_mainmenu_load();
    if (id < 0 || id >= TRIMPOD_MM_COUNT)
        return true;
    return mm_enabled[id];
}

/* ---- the page: a do_menu of inline toggle value-rows ------------------ */

/* Each row is a MENUITEM_VALUE bound to mm_enabled[i] via a menu_value_cb:
 * A / LEFT / RIGHT all flip it, and the "[x]"/"[ ]" state shows right-aligned
 * through do_menu's value indicator. */
static const char *mm_toggle_get(void *ctx, char *buf, int len)
{
    (void)buf; (void)len;
    return trimpod_toggle_str(*(bool *)ctx);
}
static void mm_toggle_cycle(void *ctx, int dir)
{
    (void)dir;
    bool *b = (bool *)ctx;
    *b = !*b;                           /* A/LEFT/RIGHT all toggle */
    mm_save();
}
static const struct menu_value_cb mm_value[TRIMPOD_MM_COUNT] = {
    { mm_toggle_get, mm_toggle_cycle, &mm_enabled[0] },
    { mm_toggle_get, mm_toggle_cycle, &mm_enabled[1] },
    { mm_toggle_get, mm_toggle_cycle, &mm_enabled[2] },
    { mm_toggle_get, mm_toggle_cycle, &mm_enabled[3] },
    { mm_toggle_get, mm_toggle_cycle, &mm_enabled[4] },
    { mm_toggle_get, mm_toggle_cycle, &mm_enabled[5] },
    { mm_toggle_get, mm_toggle_cycle, &mm_enabled[6] },
    { mm_toggle_get, mm_toggle_cycle, &mm_enabled[7] },
    { mm_toggle_get, mm_toggle_cycle, &mm_enabled[8] },
};
MENUITEM_VALUE(mm_item_music,      ID2P(LANG_TRIMPOD_MUSIC),         &mm_value[0], Icon_NOICON);
MENUITEM_VALUE(mm_item_podcasts,   ID2P(LANG_TRIMPOD_PODCASTS),      &mm_value[1], Icon_NOICON);
MENUITEM_VALUE(mm_item_audiobooks, ID2P(LANG_TRIMPOD_AUDIOBOOKS),    &mm_value[2], Icon_NOICON);
MENUITEM_VALUE(mm_item_artists,    ID2P(LANG_TRIMPOD_ARTISTS),       &mm_value[3], Icon_NOICON);
MENUITEM_VALUE(mm_item_albums,     ID2P(LANG_TRIMPOD_ALBUMS),        &mm_value[4], Icon_NOICON);
MENUITEM_VALUE(mm_item_songs,      ID2P(LANG_TRIMPOD_SONGS),         &mm_value[5], Icon_NOICON);
MENUITEM_VALUE(mm_item_playlists,  ID2P(LANG_PLAYLISTS),             &mm_value[6], Icon_NOICON);
MENUITEM_VALUE(mm_item_browse,     ID2P(LANG_TRIMPOD_BROWSE),        &mm_value[7], Icon_NOICON);
MENUITEM_VALUE(mm_item_shuffle,    ID2P(LANG_TRIMPOD_SHUFFLE_ALL),   &mm_value[8], Icon_NOICON);
MAKE_MENU(trimpod_mainmenu_menu, ID2P(LANG_TRIMPOD_MAINMENU), NULL, Icon_Submenu_Entered,
          &mm_item_music, &mm_item_podcasts, &mm_item_audiobooks, &mm_item_artists,
          &mm_item_albums, &mm_item_songs, &mm_item_playlists, &mm_item_browse,
          &mm_item_shuffle);

int trimpod_mainmenu_settings(void)
{
    trimpod_mainmenu_load();
    do_menu(&trimpod_mainmenu_menu, NULL, NULL, false);
    mm_save();                          /* persist on every exit path */
    return 0;
}
