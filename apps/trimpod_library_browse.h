/* Trimpod: tag-based Music Library browse over the SQLite index.
 *
 * The root-menu "Artists" entry: a three-level in-place navigator -- Artists ->
 * Albums (with an "All Songs" row) -> Tracks.  Selecting a track queues the
 * listed tracks (trimpod_queue_begin/end) and starts at that one; returning
 * from Now Playing reopens the Tracks list on the playing song.
 * Music category only -- podcasts/audiobooks stay folder-based.  (Rescan Library
 * lives in Settings -> Library.)  See trimpod_library.h for the backend. */
#ifndef _TRIMPOD_LIBRARY_BROWSE_H
#define _TRIMPOD_LIBRARY_BROWSE_H

/* Which facet trimpod_library_browse() opens, passed as the root_menu items[]
 * param (cast to void*). */
enum trimpod_library_mode
{
    TP_LIB_ARTISTS = 0,   /* Artists -> Albums -> Tracks */
    TP_LIB_ALBUMS,        /* every Album -> Tracks       */
    TP_LIB_SONGS,         /* every Song (one flat list)  */
};

/* Root-menu handler (items[] signature).  `param` is an enum trimpod_library_mode
 * (cast from void*).  Returns GO_TO_WPS when a selection started playback, else
 * GO_TO_ROOT. */
int trimpod_library_browse(void *param);

#endif /* _TRIMPOD_LIBRARY_BROWSE_H */
