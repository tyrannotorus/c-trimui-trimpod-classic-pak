/* Trimpod: the video player's sound, played on Trimpod's own output. */
#ifndef _VIDEO_AUDIO_H_
#define _VIDEO_AUDIO_H_

#include <stdbool.h>

/* Before the player starts: while open, its "default" is Trimpod's output. */
bool video_audio_open(void);
void video_audio_close(void);

#endif /* _VIDEO_AUDIO_H_ */
