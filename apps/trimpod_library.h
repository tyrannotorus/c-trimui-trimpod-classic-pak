/* Trimpod: SQLite-backed library index of the configured source folders.
 *
 * The DB (ROCKBOX_DIR/library.db) is a *derived index* of the SD-card files --
 * migrated across schema versions, never wholesale-rebuilt except when the file
 * is unreadable.  Reconcile is stat-gated: a no-change load costs one stat per
 * known directory (a blink); tags are parsed only for new/changed files.  It
 * powers Shuffle Songs (no filesystem walk) and the Artists/Albums browse. */
#ifndef _TRIMPOD_LIBRARY_H
#define _TRIMPOD_LIBRARY_H

#include <stdbool.h>

/* Open-or-create+migrate the index, then reconcile it against the source
 * folders.  Call once at startup, after settings are up. */
void trimpod_library_init(void);
void trimpod_library_close(void);

/* Stat-gated incremental reconcile: at startup and whenever a source folder is
 * added or removed.  force_full re-reads every directory (manual "Rescan
 * Library", for in-place retags that don't bump dir mtimes).  Returns the
 * number of track rows inserted/updated/deleted. */
int  trimpod_library_reconcile(bool force_full);

/* Replace the current playlist with the query result; returns tracks inserted.
 * NULL browse_artist/album is a wildcard.  Caller shuffles + starts.
 * Shuffle Songs == trimpod_library_build_playlist(TP_CAT_MUSIC, NULL, NULL). */
int  trimpod_library_build_playlist(int category, const char *browse_artist,
                                    const char *album);

/* Bulk-materialize `paths` (n entries) as the current playlist via a reusable
 * m3u + one index scan -- avoids Rockbox's slow O(n) per-track control-file
 * writes.  Stages up to TRIMPOD_MAX_FILES_IN_PLAYLIST entries beginning at
 * paths[first], wrapping (so a tapped row can lead an over-cap queue).  Caller
 * then calls playlist_start().  Returns entries staged, 0 on failure. */
int  trimpod_library_stage_paths(char **paths, int n, int first);

/* Browse enumerators: one callback per row, no size caps -- the caller owns
 * storage, so there is no arbitrary MAX to silently truncate against. */
void trimpod_library_artists(int category,
        void (*cb)(const char *browse_artist, int track_count, void *ctx),
        void *ctx);
/* browse_artist==NULL -> every album in the library (the flat "Albums" facet). */
void trimpod_library_albums(int category, const char *browse_artist,
        void (*cb)(const char *album, int year, void *ctx), void *ctx);
/* Tracks of one album (album==NULL -> the artist's whole library; with
 * browse_artist==NULL too -> every song, the "Songs" facet).  Rows are in the
 * same order build_playlist uses, so a row index maps 1:1 to a playlist index.
 * `title` may be "" (untagged) -> caller falls back to path's basename. */
void trimpod_library_tracks(int category, const char *browse_artist,
        const char *album,
        void (*cb)(const char *title, const char *path, void *ctx), void *ctx);

#endif /* _TRIMPOD_LIBRARY_H */
