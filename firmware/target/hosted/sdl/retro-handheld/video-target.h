/* Trimpod: runs TrimUI's stock hardware video player (tplayerslave) as a child. */
#ifndef _VIDEO_TARGET_H_
#define _VIDEO_TARGET_H_

#include <stdbool.h>

#define RETROHH_VIDEO_MAX_CHAPTERS 99

enum retrohh_video_aspect
{
    RETROHH_VIDEO_FIT = 0,   /* whole picture, black bars */
    RETROHH_VIDEO_FILL,      /* fills the screen, edges cropped */
    RETROHH_VIDEO_STRETCH,   /* fills the screen, shape distorted */
    RETROHH_VIDEO_ASPECT_COUNT,
};

struct retrohh_video_info
{
    bool hevc;
    int  width, height;
    int  ratio_num, ratio_den;     /* display shape, or 0 if the file doesn't say */
    int  chapter_ms[RETROHH_VIDEO_MAX_CHAPTERS];
    int  chapters;
};

/* Reads the file with ffprobe; false if it couldn't be read. */
bool retrohh_video_probe(const char *path, struct retrohh_video_info *info);

/* start_ms > 0 starts partway through the file. */
bool retrohh_video_start(const char *path, int start_ms);
void retrohh_video_stop(void);
bool retrohh_video_running(void);

/* False while the player hasn't reported yet. */
bool retrohh_video_status(int *position_ms, int *duration_ms, bool *playing);

void retrohh_video_pause(bool paused);
void retrohh_video_seek(int position_ms);
void retrohh_video_set_aspect(const struct retrohh_video_info *info,
                              enum retrohh_video_aspect aspect);

/* Show or hide the video through the app window; the bottom overlay_rows LCD rows
 * stay on top of it. */
void retrohh_video_set_overlay(bool video_visible, int overlay_rows);

#endif /* _VIDEO_TARGET_H_ */
