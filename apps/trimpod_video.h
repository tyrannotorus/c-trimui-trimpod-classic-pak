/* Trimpod: full-screen video playback with Now Playing style overlays. */
#ifndef _TRIMPOD_VIDEO_H
#define _TRIMPOD_VIDEO_H

#include <stdbool.h>

/* Plays one file until it ends or the user leaves.  Stops any music first.
 * False if it couldn't play and only showed a notice. */
bool trimpod_video_play(const char *path);

#endif /* _TRIMPOD_VIDEO_H */
