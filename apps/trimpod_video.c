/***************************************************************************
 * Trimpod: full-screen video playback.
 *
 * The stock hardware player draws on a display layer below the app window (see
 * video-target.c).  This page clears the window so the video shows through, and
 * draws the Now Playing panels in a strip at the bottom when there's something
 * to show: progress after any control, volume after the rocker.
 *
 *   A            = play / pause (pausing shows the overlay)
 *   D-pad <- ->  = back / ahead 10 s (hold to keep going)
 *   L1 / R1      = back / ahead 10 s, as on Now Playing
 *   L2 / R2      = previous / next chapter, as on Now Playing
 *   X            = aspect ratio: Fit, Stretch
 *   Volume +/-   = volume
 *   B            = with the overlay up, back to the list; else as A
 *   Hold B       = main menu
 ****************************************************************************/
#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>   /* realpath */
#include <string.h>
#include "string-extra.h"
#include "action.h"
#include "audio.h"
#include "backlight.h"
#include "button.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "misc.h"
#include "powermgmt.h"
#include "screen_access.h"
#include "settings.h"
#include "sound.h"
#include "splash.h"
#include "viewport.h"
#include "gui/scrollbar.h"
#include "recorder/bmp.h"
#include "video-target.h"
#include "trimpod_page.h"      /* trimpod_home_pending */
#include "trimpod_video.h"

extern void retrohh_cpu_hold_dynamic(void);     /* power-target.c owns CPU policy */
extern void retrohh_cpu_release_dynamic(void);

/* The bar and text of the Now Playing panels (1ST_GEN_REMIX.wps), on a taller strip. */
#define STRIP_TOP    180
#define STRIP_ROWS   (LCD_HEIGHT - STRIP_TOP)
#define BAR_X        20
#define BAR_Y        192
#define BAR_W        280
#define BAR_H        14
#define TEXT_Y       210
#define TEXT_H       25
#define BAR_BMP_DIR  ROCKBOX_DIR "/wps/1ST_GEN_REMIX"
#define BAR_BMP_BYTES (BAR_W * BAR_H * 4 + 64)

#define SEEK_STEP_MS        10000
#define SEEK_END_MARGIN_MS  1000
/* Joins D-pad repeats into one seek. */
#define SEEK_SETTLE_TICKS   (HZ / 3)
/* How long the player takes to report where a seek landed. */
#define SEEK_REPORT_TICKS   HZ
/* Past this far into a chapter, "previous" restarts it. */
#define CHAPTER_RESTART_MS  3000
#define OVERLAY_TICKS       (3 * HZ)
#define VOLUME_TICKS        (2 * HZ)
#define ASPECT_LABEL_TICKS  (2 * HZ)
#define POLL_TICKS          (HZ / 2)
#define ACTION_WAIT_TICKS   (HZ / 10)
/* Polls without movement near the end before the file counts as finished. */
#define END_STALL_POLLS     6
#define END_MARGIN_MS       2000

/* Per-video resume points: "<ms> <path>", oldest first.  Written to a temporary
 * file and renamed, so a power loss mid-write leaves the old file intact. */
#define RESUME_FILE         ROCKBOX_DIR "/.video_positions"
#define RESUME_TMP          RESUME_FILE ".tmp"
#define RESUME_MAX          256
/* Stopping earlier than this starts over next time. */
#define RESUME_MIN_MS       5000
/* Saved while watching too, in case the battery dies. */
#define RESUME_SAVE_TICKS   (60 * HZ)
/* The picture stays hidden until the resume seek lands, for at most this long.
 * It lands on a keyframe before the target, so half way counts. */
#define REVEAL_WAIT_TICKS   (4 * HZ)

enum overlay_panel { PANEL_NONE, PANEL_PROGRESS, PANEL_VOLUME };

static struct retrohh_video_info info;
static enum retrohh_video_aspect aspect;   /* kept between videos */

static struct
{
    int  position_ms, duration_ms;
    int  seek_target_ms;        /* -1: no seek waiting */
    long seek_send_tick;
    long ignore_report_until;
    bool paused;
    bool reported;              /* the player has told us anything yet */
    bool revealed;              /* the window has stopped hiding the video */
    long reveal_by;
    int  stalled_polls;
    int  last_reported_ms;
    long progress_until, volume_until, aspect_label_until;
    int  last_volume;
    enum overlay_panel shown;
    int  shown_seconds, shown_volume;
    bool shown_aspect_label;
    enum retrohh_video_aspect shown_aspect;
    bool drawn_once;
} v;

static unsigned char bar_fill_data[BAR_BMP_BYTES], bar_track_data[BAR_BMP_BYTES];
static struct bitmap bar_fill = { .data = bar_fill_data };
static struct bitmap bar_track = { .data = bar_track_data };
static bool have_bar_bitmaps;

/* ---- resume points ---------------------------------------------------- */

static struct resume_entry
{
    int  position_ms;
    char path[MAX_PATH];
} resume_entries[RESUME_MAX];
static int  resume_count;
static bool resume_loaded;
static char resume_key[MAX_PATH];  /* the video playing now, symlinks resolved */
static int  resume_start_ms;       /* where it should start */
static int  resume_saved_ms;       /* what the store holds for it */
static long resume_next_save;

static void resume_load(void)
{
    char line[MAX_PATH + 16];
    int fd;

    if (resume_loaded)
        return;
    resume_loaded = true;

    fd = open(RESUME_FILE, O_RDONLY);
    if (fd < 0)
        return;
    while (resume_count < RESUME_MAX && read_line(fd, line, sizeof line) > 0)
    {
        char *space = strchr(line, ' ');

        if (!space)
            continue;
        resume_entries[resume_count].position_ms = atoi(line);
        strlcpy(resume_entries[resume_count].path, space + 1, MAX_PATH);
        resume_count++;
    }
    close(fd);
}

static void resume_write(void)
{
    int fd = open(RESUME_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0)
        return;
    for (int i = 0; i < resume_count; i++)
        fdprintf(fd, "%d %s\n", resume_entries[i].position_ms,
                 resume_entries[i].path);
    close(fd);
    rename(RESUME_TMP, RESUME_FILE);
}

static int resume_find(const char *key)
{
    for (int i = 0; i < resume_count; i++)
        if (!strcmp(resume_entries[i].path, key))
            return i;
    return -1;
}

static void resume_remove_at(int index)
{
    memmove(&resume_entries[index], &resume_entries[index + 1],
            (resume_count - index - 1) * sizeof resume_entries[0]);
    resume_count--;
}

/* realpath: the virtual-folder farm and the real path name the same video. */
static void resume_open(const char *path)
{
    int index;

    resume_load();
    if (!realpath(path, resume_key))
        strlcpy(resume_key, path, sizeof resume_key);
    index = resume_find(resume_key);
    resume_saved_ms = index >= 0 ? resume_entries[index].position_ms : 0;
    resume_start_ms = resume_saved_ms;
    resume_next_save = current_tick + RESUME_SAVE_TICKS;
}

/* Barely started or played out: the next play starts from the top. */
static void resume_save(int position_ms, int duration_ms)
{
    int index = resume_find(resume_key);

    if (position_ms < RESUME_MIN_MS || position_ms >= duration_ms - END_MARGIN_MS)
        position_ms = 0;
    if (position_ms == resume_saved_ms)
        return;
    resume_saved_ms = position_ms;

    if (index >= 0)
        resume_remove_at(index);        /* re-added last: most recently watched */
    if (position_ms)
    {
        if (resume_count == RESUME_MAX)
            resume_remove_at(0);
        resume_entries[resume_count].position_ms = position_ms;
        strlcpy(resume_entries[resume_count].path, resume_key, MAX_PATH);
        resume_count++;
    }
    resume_write();
}

/* ---- overlay ---------------------------------------------------------- */

/* As the theme loads them, so they draw in its colors.  pb_back.bmp is the fill
 * and pb.bmp the track: %pb(...,PBB,backdrop,PB). */
static void load_bar_bitmaps(void)
{
    if (have_bar_bitmaps)
        return;
    have_bar_bitmaps =
        read_bmp_file(BAR_BMP_DIR "/pb_back.bmp", &bar_fill, sizeof bar_fill_data,
                      FORMAT_ANY, NULL) > 0 &&
        read_bmp_file(BAR_BMP_DIR "/pb.bmp", &bar_track, sizeof bar_track_data,
                      FORMAT_ANY, NULL) > 0;
}

/* Each part of a panel has its own viewport, as in the theme: it clips the bar. */
static void panel_viewport(struct screen *display, int x, int y, int w, int h)
{
    static struct viewport vp;

    viewport_set_defaults(&vp, SCREEN_MAIN);
    vp.x = x;
    vp.y = y;
    vp.width = w;
    vp.height = h;
    vp.font = FONT_UI;
    vp.fg_pattern = global_settings.fg_color;
    vp.bg_pattern = global_settings.bg_color;
    display->set_viewport(&vp);
}

static void draw_bar(struct screen *display, int value, int max)
{
    if (max <= 0)
        max = 1;
    panel_viewport(display, BAR_X, BAR_Y, BAR_W, BAR_H);
    if (have_bar_bitmaps)
    {
        display->bmp_part(&bar_track, 0, 0, 0, 0, BAR_W, BAR_H);
        gui_bitmap_scrollbar_draw(display, &bar_fill, 0, 0, BAR_W, BAR_H,
                                  max, 0, value, HORIZONTAL | DONT_CLEAR_EXCESS);
    }
    else
        gui_scrollbar_draw(display, 0, 0, BAR_W, BAR_H, max, 0, value, HORIZONTAL);
    panel_viewport(display, BAR_X, TEXT_Y, BAR_W, TEXT_H);
}

static int display_position(void)
{
    return v.seek_target_ms >= 0 ? v.seek_target_ms : v.position_ms;
}

static const char *aspect_label(void)
{
    static const int labels[RETROHH_VIDEO_ASPECT_COUNT] = {
        LANG_TRIMPOD_ASPECT_FIT, LANG_TRIMPOD_ASPECT_STRETCH,
    };
    return str(labels[aspect]);
}

static void draw_progress(struct screen *display)
{
    char elapsed[16], remaining[16];
    int position = display_position(), w, h;

    draw_bar(display, position, v.duration_ms);

    format_time(elapsed, sizeof elapsed, position);
    format_time(remaining + 1, sizeof remaining - 1, v.duration_ms - position);
    remaining[0] = '-';

    display->putsxy(0, 0, elapsed);
    display->getstringsize(remaining, &w, &h);
    display->putsxy(BAR_W - w, 0, remaining);
    if (TIME_BEFORE(current_tick, v.aspect_label_until))
    {
        const char *label = aspect_label();

        display->getstringsize(label, &w, &h);
        display->putsxy((BAR_W - w) / 2, 0, label);
    }
}

static void draw_volume(struct screen *display)
{
    char label[32];
    int minvol = sound_min(SOUND_VOLUME), maxvol = sound_max(SOUND_VOLUME);
    int w, h;

    draw_bar(display,
             to_normalized_volume(global_status.volume, minvol, maxvol, 1000),
             1000);
    snprintf(label, sizeof label, "%s %d/%d", str(LANG_VOLUME),
             get_normalized_volume(), get_normalized_volume_steps());
    display->getstringsize(label, &w, &h);
    display->putsxy((BAR_W - w) / 2, 0, label);
}

/* Redraws only on a visible change: every present costs power. */
static void overlay_update(void)
{
    enum overlay_panel panel = PANEL_NONE;
    int seconds = display_position() / 1000;
    bool aspect_label_on = TIME_BEFORE(current_tick, v.aspect_label_until);

    if (TIME_BEFORE(current_tick, v.volume_until))
        panel = PANEL_VOLUME;
    else if (v.paused || TIME_BEFORE(current_tick, v.progress_until))
        panel = PANEL_PROGRESS;

    if (v.drawn_once && panel == v.shown &&
        (panel != PANEL_PROGRESS || (seconds == v.shown_seconds &&
                                     aspect == v.shown_aspect &&
                                     aspect_label_on == v.shown_aspect_label)) &&
        (panel != PANEL_VOLUME || global_status.volume == v.shown_volume))
        return;

    v.shown = panel;
    v.shown_seconds = seconds;
    v.shown_aspect = aspect;
    v.shown_aspect_label = aspect_label_on;
    v.shown_volume = global_status.volume;
    v.drawn_once = true;

    retrohh_video_set_overlay(true, panel == PANEL_NONE ? 0 : STRIP_ROWS);
    if (panel != PANEL_NONE)
    {
        struct screen *display = &screens[SCREEN_MAIN];

        panel_viewport(display, 0, STRIP_TOP, LCD_WIDTH, STRIP_ROWS);
        display->clear_viewport();
        if (panel == PANEL_VOLUME)
            draw_volume(display);
        else
            draw_progress(display);
        display->set_viewport(NULL);
    }
    screens[SCREEN_MAIN].update();
}

static void show_progress(void)
{
    v.progress_until = current_tick + OVERLAY_TICKS;
    v.volume_until = current_tick;      /* a control press swaps straight back */
}

static void queue_seek(int position_ms, long delay)
{
    int last = v.duration_ms - SEEK_END_MARGIN_MS;

    if (position_ms > last)
        position_ms = last;
    if (position_ms < 0)
        position_ms = 0;
    v.seek_target_ms = position_ms;
    v.seek_send_tick = current_tick + delay;
    show_progress();
}

static void send_queued_seek(void)
{
    if (v.seek_target_ms < 0 || TIME_BEFORE(current_tick, v.seek_send_tick))
        return;
    retrohh_video_seek(v.seek_target_ms);
    v.position_ms = v.seek_target_ms;
    v.seek_target_ms = -1;
    v.stalled_polls = 0;
    v.ignore_report_until = current_tick + SEEK_REPORT_TICKS;
}

static void skip_chapter(bool forward)
{
    int position = display_position();

    if (!info.chapters)
    {
        show_progress();
        return;
    }
    if (forward)
    {
        for (int i = 0; i < info.chapters; i++)
            if (info.chapter_ms[i] > position)
            {
                queue_seek(info.chapter_ms[i], 0);
                return;
            }
        show_progress();                /* already in the last chapter */
        return;
    }
    for (int i = info.chapters - 1; i >= 0; i--)
        if (info.chapter_ms[i] + CHAPTER_RESTART_MS < position)
        {
            queue_seek(info.chapter_ms[i], 0);
            return;
        }
    queue_seek(0, 0);
}

static void set_paused(bool paused)
{
    v.paused = paused;
    retrohh_video_pause(paused);
    show_progress();
}

static void pause_and_save(void)
{
    set_paused(true);
    if (v.reported)
        resume_save(display_position(), v.duration_ms);
}

/* Opaque black, overlay included: the video layer below can't show until the
 * window goes clear. */
static void hide_video(void)
{
    struct screen *display = &screens[SCREEN_MAIN];

    retrohh_video_set_overlay(false, 0);
    display->set_background(LCD_BLACK);
    display->clear_display();
    display->set_background(global_settings.bg_color);
    display->update();
}

/* Behind an opaque window, which clears once the picture is where it should
 * be. */
static bool start_player(const char *path, int start_ms)
{
    hide_video();
    if (!retrohh_video_start(path, start_ms))
        return false;
    v.position_ms = start_ms;
    v.seek_target_ms = -1;
    v.reveal_by = current_tick + REVEAL_WAIT_TICKS;
    v.ignore_report_until = start_ms ? v.reveal_by : current_tick;
    return true;
}

/* False once the video is over. */
static bool poll_player(void)
{
    int position, duration;
    bool playing;

    reset_poweroff_timer();             /* watching isn't idling */

    if (!retrohh_video_running())
        return false;
    if (!retrohh_video_status(&position, &duration, &playing))
        return true;

    v.duration_ms = duration;
    if (!v.reported)
    {
        v.reported = true;
        retrohh_video_set_aspect(&info, aspect);
    }
    if (position >= resume_start_ms / 2 || !TIME_BEFORE(current_tick, v.reveal_by))
        v.revealed = true;
    if (TIME_BEFORE(current_tick, v.ignore_report_until))
        return true;
    v.position_ms = position;

    if (v.paused)
        return true;
    if (!playing)
        return false;
    /* The player can sit on the last frame without saying it finished. */
    if (duration > 0 && position >= duration - END_MARGIN_MS &&
        position == v.last_reported_ms)
    {
        if (++v.stalled_polls >= END_STALL_POLLS)
            return false;
    }
    else
        v.stalled_polls = 0;
    v.last_reported_ms = position;
    return true;
}

static void run_session(void)
{
    long next_poll = current_tick;

    for (;;)
    {
        int action = get_action(CONTEXT_TP_VIDEO, ACTION_WAIT_TICKS);

        if (action == SYS_MEDIA_KEY)            /* a headset: as A, R2, L2 */
        {
            int command = button_get_data();

            action = command == MEDIA_KEY_NEXT ? ACTION_TP_CHAP_NEXT :
                     command == MEDIA_KEY_PREV ? ACTION_TP_CHAP_PREV :
                                                 ACTION_WPS_PLAY;
        }

        if (action & SYS_EVENT)
        {
            long handled = default_event_handler(action);

            if (handled == SYS_POWEROFF || handled == SYS_REBOOT)
                return;
            /* Headphones gone, from the jack, USB-C or Bluetooth: pause, as
             * music does.  The sound follows the output on its own. */
            if (action == SYS_PHONE_UNPLUGGED && !v.paused)
                pause_and_save();
        }
        else switch (action)
        {
            case ACTION_TP_HOME:
                trimpod_home_pending = true;
                return;
            case ACTION_WPS_BROWSE:
                /* The overlay up, B leaves; otherwise it pauses and shows it. */
                if (v.paused || v.shown != PANEL_NONE)
                    return;
                pause_and_save();
                break;
            case ACTION_WPS_PLAY:
                if (v.paused)
                    set_paused(false);
                else
                    pause_and_save();
                break;
            case ACTION_WPS_SEEKBACK:
                queue_seek(display_position() - SEEK_STEP_MS, SEEK_SETTLE_TICKS);
                break;
            case ACTION_WPS_SEEKFWD:
                queue_seek(display_position() + SEEK_STEP_MS, SEEK_SETTLE_TICKS);
                break;
            case ACTION_TP_CHAP_PREV:
                skip_chapter(false);
                break;
            case ACTION_TP_CHAP_NEXT:
                skip_chapter(true);
                break;
            case ACTION_TP_VIDEO_ASPECT:
                aspect = (aspect + 1) % RETROHH_VIDEO_ASPECT_COUNT;
                retrohh_video_set_aspect(&info, aspect);
                v.aspect_label_until = current_tick + ASPECT_LABEL_TICKS;
                v.volume_until = current_tick;
                if (TIME_BEFORE(v.progress_until, v.aspect_label_until))
                    v.progress_until = v.aspect_label_until;
                break;
            default:
                break;
        }

        if (power_display_off() && !v.paused)
            set_paused(true);

        send_queued_seek();

        if (global_status.volume != v.last_volume)
        {
            v.last_volume = global_status.volume;
            v.volume_until = current_tick + VOLUME_TICKS;
        }

        if (!TIME_BEFORE(current_tick, next_poll))
        {
            if (!poll_player())
                return;
            next_poll = current_tick + (v.revealed ? POLL_TICKS : 0);
            if (v.reported && !TIME_BEFORE(current_tick, resume_next_save))
            {
                resume_next_save = current_tick + RESUME_SAVE_TICKS;
                resume_save(display_position(), v.duration_ms);
            }
        }
        if (v.revealed)
            overlay_update();
    }
}

void trimpod_video_play(const char *path)
{
    if (retrohh_video_probe(path, &info) && info.hevc)
    {
        splash(2 * HZ, ID2P(LANG_TRIMPOD_VIDEO_HEVC));
        return;
    }

    if (audio_status())
        audio_stop();
    load_bar_bitmaps();
    resume_open(path);

    retrohh_cpu_hold_dynamic();
    backlight_set_timeout(0);           /* a video is active use, not idle */
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);

    memset(&v, 0, sizeof v);
    v.last_volume = global_status.volume;
    if (start_player(path, resume_start_ms))
    {
        button_clear_queue();
        run_session();
    }

    /* Picture and overlay go in one frame; the player exits behind it. */
    hide_video();
    if (v.reported)
        resume_save(display_position(), v.duration_ms);
    retrohh_video_stop();
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, true);
    if (power_display_off())
        backlight_set_timeout_quiet(global_settings.backlight_timeout);
    else
        backlight_set_timeout(global_settings.backlight_timeout);
    retrohh_cpu_release_dynamic();
    button_clear_queue();
    button_touch_activity();

    if (!v.reported)
        splash(2 * HZ, ID2P(LANG_TRIMPOD_VIDEO_FAILED));
}
