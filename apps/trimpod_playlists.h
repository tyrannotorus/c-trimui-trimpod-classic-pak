/* Trimpod: the Playlists screen (see trimpod_playlists.c).  A trimpod_page that
 * lists the catalog's playlists plus a "+ Add Playlist" row, mirroring the
 * Music Folders settings page. */
#ifndef _TRIMPOD_PLAYLISTS_H
#define _TRIMPOD_PLAYLISTS_H

#include <stdbool.h>
#include <stddef.h>
#include "playlist.h"          /* struct playlist_insert_context */

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

/* Hold-A music context menu: Play / Play Next / Add to Play Queue / Add to
 * Playlist.  The adds are handled here; returns true when the caller should
 * play the selection.  The tracks form takes a path set (a library
 * artist/album has no single path to expand). */
bool trimpod_music_context(const char *title, const char *path, int attr);
bool trimpod_music_context_tracks(const char *title, char **paths, int count);

/* Put a track or directory (recursive) into the live queue at `position`:
 * PLAYLIST_INSERT (after the current track), PLAYLIST_INSERT_LAST (the end)
 * or PLAYLIST_REPLACE (confirm and start a new queue from it).  Returns true
 * when the queue started playing from this call. */
bool trimpod_queue_add(const char *path, int attr, int position);

/* A queue build in steps: begin() opens the insert at `position`
 * (PLAYLIST_REPLACE confirms the erase and starts fresh; false = declined or
 * no control file), add_path()/add_paths() fill it, end() starts a fresh queue
 * at `start` (shuffled first if `shuffle`) and returns whether playback
 * started.  add_path doubles as a trimpod_library enumerator callback
 * (false = the engine refused). */
struct trimpod_queue
{
    struct playlist_insert_context ctx;
    bool fresh;
    int pos;
};
bool trimpod_queue_begin(struct trimpod_queue *q, int position);
bool trimpod_queue_add_path(const char *path, void *q);
void trimpod_queue_add_paths(struct trimpod_queue *q, char **paths, int n,
                             int first);
bool trimpod_queue_end(struct trimpod_queue *q, int start, bool shuffle);

/* True when `path` is the loaded track, resuming it if paused: the caller
 * shows Now Playing instead of restarting it. */
bool trimpod_resume_if_current(const char *path);

#endif /* _TRIMPOD_PLAYLISTS_H */
