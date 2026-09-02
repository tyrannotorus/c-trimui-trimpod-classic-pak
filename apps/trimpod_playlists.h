/* Trimpod: the Playlists screen (see trimpod_playlists.c).  A trimpod_page that
 * lists the catalog's playlists plus a "+ Add Playlist" row, mirroring the
 * Music Folders settings page. */
#ifndef _TRIMPOD_PLAYLISTS_H
#define _TRIMPOD_PLAYLISTS_H

#include <stdbool.h>
#include <stddef.h>

/* root menu Playlists entry (a root_menu items[] function) */
int trimpod_playlists_screen(void *param);

/* Consume a pending Play/Shuffle Playlist request and start playback.  Returns
 * 1 (started -> go to Now Playing), 0 (nothing pending -> show the file list),
 * or -1 (empty/failed -> back to the list).  Called by root_menu.c's
 * playlist_view() so the file list ends up behind Now Playing. */
int trimpod_playlists_start_pending(void);

/* Consume a pending view request (tap A on a playlist row): fill `path` with
 * the playlist's full .m3u8 path and return true.  Called by playlist_view()
 * to open the playlist's track listing in the stock viewer. */
bool trimpod_playlists_take_pending_view(char *path, size_t len);

/* Show the Playlists screen in "pick" mode: the chosen (or newly created)
 * playlist receives `sel` (a file or directory) via the stock Rockbox
 * catalog_insert_into() -- a directory is expanded into its tracks. */
void trimpod_playlists_pick(const char *sel, int sel_attr);

/* Hold-A music context menu: Play / Add to Playlist / Add to Play Queue.
 * The adds are handled here; returns true when the caller should play the
 * selection.  The tracks form takes a path set (a library artist/album has no
 * single path to expand). */
bool trimpod_music_context(const char *title, const char *path, int attr);
bool trimpod_music_context_tracks(const char *title, char **paths, int count);

/* Append a track or directory (recursive) to the live queue; `replace`
 * confirms and starts a new queue from it instead.  Returns true when the
 * queue started playing from this call. */
bool trimpod_queue_add(const char *path, int attr, bool replace);

/* Start the current playlist at `index`, file order, with the same resume
 * bookkeeping a browser tap does. */
void trimpod_queue_start(int index);

#endif /* _TRIMPOD_PLAYLISTS_H */
