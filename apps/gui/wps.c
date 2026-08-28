/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Jerome Kuptz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

#include "system.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "backlight.h"
#include "action.h"
#include "kernel.h"
#include "filetypes.h"
#include "settings.h"
#include "skin_engine/skin_engine.h"
#include "audio.h"
#include "usb.h"
#include "status.h"
#include "storage.h"
#include "screens.h"
#include "playlist.h"
#include "icons.h"
#include "lang.h"
#include "bookmark.h"
#include "misc.h"
#include "sound.h"
#include "onplay.h"
#include "playback.h"
#include "rbunicode.h"
#include "splash.h"
#include "cuesheet.h"
#include "ata_idle_notify.h"
#include "root_menu.h"
#include "trimpod_transition.h"   /* slide back to the browser on B */
#include "menu.h"                 /* do_menu / MENUITEM_VALUE (A: Now Playing menu) */
#include "powermgmt.h"            /* sleep timer (Now Playing knob) */
#include "trimpod_playlists.h"    /* trimpod_playlists_pick (Add to Playlist) */
#include "trimpod_page.h"          /* Now Playing is a trimpod_page */
#include "trimpod_ui.h"            /* refresh the SBS after lyrics toggle */
#include "backdrop.h"
#include "shortcuts.h"
#include "appevents.h"
#include "viewport.h"
#include "pcmbuf.h"
#include "option_select.h"
#include "playlist_viewer.h"
#include "wps.h"
#include "trimpod_visualizer.h"
#include "trimpod_lyrics.h"
#include "statusbar-skinned.h"
#include "skin_engine/wps_internals.h"


#define FF_REWIND_MAX_PERCENT 3 /* cap ff/rewind step size at max % of file */
                                /* 3% of 30min file == 54s step size */
#define MIN_FF_REWIND_STEP 500

static struct wps_state wps_state;

/* initial setup of wps_data  */
static void wps_state_init(void);
static void track_info_callback(unsigned short id, void *param);

#define WPS_DEFAULTCFG WPS_DIR "/rockbox_default.wps"
#define DEFAULT_WPS(screen) (WPS_DEFAULTCFG)

char* wps_default_skin(enum screen_type screen)
{
    static char *skin_buf[NB_SCREENS] = {
#if LCD_DEPTH > 1
            "%X(d)\n"
#endif
            "%s%?it<%?in<%in. |>%it|%fn>\n"
            "%s%?ia<%ia|%?d(2)<%d(2)|%(root%)>>\n"
            "%s%?id<%id|%?d(1)<%d(1)|%(root%)>> %?iy<%(%iy%)|>\n\n"
            "%al%pc/%pt%ar[%pp:%pe]\n"
            "%fbkBit %?fv<avg|> %?iv<%(id3v%iv%)|%(no id3%)>\n"
            "%pb\n%pm\n",
        };
    return skin_buf[screen];
}

static void update_non_static(void)
{
    FOR_NB_SCREENS(i)
        skin_update(WPS, i, SKIN_REFRESH_NON_STATIC);
}

void wps_do_action(enum wps_do_action_type action, bool updatewps)
{
    if (action == WPS_PLAYPAUSE) /* toggle the status */
    {
        struct wps_state *state = get_wps_state();
        if (state->paused)
            action = WPS_PLAY;

        state->paused = !state->paused;
    }

    if (action == WPS_PLAY) /* unpause_action */
    {
        audio_resume();
    }
    else /* WPS_PAUSE pause_action */
    {
        audio_pause();

        if (global_settings.pause_rewind) {
            unsigned long elapsed = audio_current_track()->elapsed;
            long newpos = elapsed - (global_settings.pause_rewind * 1000);

            audio_pre_ff_rewind();
            audio_ff_rewind(newpos > 0 ? newpos : 0);
        }
        if (action == WPS_PLAYPAUSE)
        {
            settings_save();
            call_storage_idle_notifys(true);   /* make sure resume info is saved */
        }
    }

    /* Bugfix only do a skin refresh if in one of the below screens */
    enum current_activity act = get_current_activity();

    bool refresh = (act == ACTIVITY_FM ||
                    act == ACTIVITY_WPS ||
                    act == ACTIVITY_RECORDING);

    if (updatewps && refresh)
        update_non_static();
}

static bool ffwd_rew(int button, bool seek_from_end)
{
    unsigned int step = 0;     /* current ff/rewind step */
    unsigned int max_step = 0; /* maximum ff/rewind step */
    int ff_rewind_count = 0;   /* current ff/rewind count (in ticks) */
    int direction = -1;         /* forward=1 or backward=-1 */
    bool exit = false;
    bool usb = false;
    bool ff_rewind = false;
    const long ff_rw_accel = (global_settings.ff_rewind_accel + 3);
    struct wps_state *gstate = get_wps_state();
    struct mp3entry *old_id3 = gstate->id3;

    if (button == ACTION_NONE)
    {
        status_set_ffmode(0);
        return usb;
    }
    while (!exit)
    {
        struct mp3entry *id3 = gstate->id3;
        if (id3 != old_id3)
        {
            ff_rewind = false;
            ff_rewind_count = 0;
            old_id3 = id3;
        }
        if (id3 && seek_from_end)
            id3->elapsed = id3->length;

        switch ( button )
        {
            case ACTION_WPS_SEEKFWD:
                 direction = 1;
                 /* Fallthrough */
            case ACTION_WPS_SEEKBACK:
                if (ff_rewind)
                {
                    if (direction == 1)
                    {
                        /* fast forwarding, calc max step relative to end */
                        max_step = (id3->length -
                                    (id3->elapsed +
                                     ff_rewind_count)) *
                                     FF_REWIND_MAX_PERCENT / 100;
                    }
                    else
                    {
                        /* rewinding, calc max step relative to start */
                        max_step = (id3->elapsed + ff_rewind_count) *
                                   FF_REWIND_MAX_PERCENT / 100;
                    }

                    max_step = MAX(max_step, MIN_FF_REWIND_STEP);

                    if (step > max_step)
                        step = max_step;

                    ff_rewind_count += step * direction;

                    /* smooth seeking by multiplying step by: 1 + (2 ^ -accel) */
                    step += step >> ff_rw_accel;
                }
                else
                {
                    if ((audio_status() & AUDIO_STATUS_PLAY) && id3 && id3->length )
                    {
                        audio_pre_ff_rewind();
                        if (direction > 0)
                            status_set_ffmode(STATUS_FASTFORWARD);
                        else
                            status_set_ffmode(STATUS_FASTBACKWARD);

                        ff_rewind = true;

                        step = 1000 * global_settings.ff_rewind_min_step;
                    }
                    else
                        break;
                }

                if (direction > 0) {
                    if ((id3->elapsed + ff_rewind_count) > id3->length)
                        ff_rewind_count = id3->length - id3->elapsed;
                }
                else {
                    if ((int)(id3->elapsed + ff_rewind_count) < 0)
                        ff_rewind_count = -id3->elapsed;
                }

                /* set the wps state ff_rewind_count so the progess info
                   displays corectly */
                gstate->ff_rewind_count = ff_rewind_count;

                FOR_NB_SCREENS(i)
                {
                    skin_update(WPS, i,
                                SKIN_REFRESH_PLAYER_PROGRESS |
                                SKIN_REFRESH_DYNAMIC);
                }

                break;

            case ACTION_WPS_STOPSEEK:
                id3->elapsed = id3->elapsed + ff_rewind_count;
                audio_ff_rewind(id3->elapsed);
                gstate->ff_rewind_count = 0;
                ff_rewind = false;
                status_set_ffmode(0);
                exit = true;
                break;

            default:
                if(default_event_handler(button) == SYS_USB_CONNECTED) {
                    status_set_ffmode(0);
                    usb = true;
                    exit = true;
                }
                break;
        }
        if (!exit)
        {
            button = get_action(CONTEXT_WPS|ALLOW_SOFTLOCK,TIMEOUT_BLOCK);
            if (button != ACTION_WPS_SEEKFWD
                && button != ACTION_WPS_SEEKBACK
                && button != 0 && !IS_SYSEVENT(button))
                button = ACTION_WPS_STOPSEEK;
        }
    }
    return usb;
}

static void gwps_caption_backlight(struct wps_state *state)
{
    if (state->id3)
    {
        if (global_settings.caption_backlight)
        {
            /* turn on backlight n seconds before track ends, and turn it off n
               seconds into the new track. n == backlight_timeout, or 5s */
            int n = global_settings.backlight_timeout * 1000;

            if ( n < 1000 )
                n = 5000; /* use 5s if backlight is always on or off */

            if (((state->id3->elapsed < 1000) ||
                 ((state->id3->length - state->id3->elapsed) < (unsigned)n)) &&
                (state->paused == false))
                backlight_on();
        }
    }
}

static void prev_track(unsigned long skip_thresh)
{
    struct wps_state *state = get_wps_state();
    if (state->id3->elapsed < skip_thresh)
    {
        audio_prev();
        return;
    }
    else
    {
        if (state->id3->cuesheet)
        {
            curr_cuesheet_skip(state->id3->cuesheet, -1, state->id3->elapsed);
            return;
        }

        audio_pre_ff_rewind();
        audio_ff_rewind(0);
    }
}

static void next_track(void)
{
    struct wps_state *state = get_wps_state();
    /* take care of if we're playing a cuesheet */
    if (state->id3->cuesheet)
    {
        if (curr_cuesheet_skip(state->id3->cuesheet, 1, state->id3->elapsed))
        {
            /* if the result was false, then we really want
               to skip to the next track */
            return;
        }
    }

    audio_next();
}

static void play_hop(int direction)
{
    struct wps_state *state = get_wps_state();
    struct cuesheet *cue = state->id3->cuesheet;
    long step = global_settings.skip_length*1000;
    long elapsed = state->id3->elapsed;
    long remaining = state->id3->length - elapsed;

    /* if cuesheet is active, then we want the current tracks end instead of
     * the total end */
    if (cue && (cue->curr_track_idx+1 < cue->track_count))
    {
        int next = cue->curr_track_idx+1;
        struct cue_track_info *t = &cue->tracks[next];
        remaining = t->offset - elapsed;
    }

    if (step < 0)
    {
        if (direction < 0)
        {
            prev_track(DEFAULT_SKIP_THRESH);
            return;
        }
        else if (remaining < DEFAULT_SKIP_THRESH*2)
        {
            next_track();
            return;
        }
        else
            elapsed += (remaining - DEFAULT_SKIP_THRESH*2);
    }
    else if (!global_settings.prevent_skip &&
           (!step ||
            (direction > 0 && step >= remaining) ||
            (direction < 0 && elapsed < DEFAULT_SKIP_THRESH)))
    {   /* Do normal track skipping */
        if (direction > 0)
            next_track();
        else if (direction < 0)
        {
            if (step > 0 && global_settings.rewind_across_tracks && elapsed < DEFAULT_SKIP_THRESH && playlist_check(-1))
            {
                bool audio_paused = (audio_status() & AUDIO_STATUS_PAUSE)?true:false;
                if (!audio_paused)
                    audio_pause();
                audio_prev();
                audio_ff_rewind(-step);
                if (!audio_paused)
                    audio_resume();
                return;
            }

            prev_track(DEFAULT_SKIP_THRESH);
        }
        return;
    }
    else if (direction == 1 && step >= remaining)
    {
        system_sound_play(SOUND_TRACK_NO_MORE);
        return;
    }
    else if (direction == -1 && elapsed < step)
    {
        elapsed = 0;
    }
    else
    {
        elapsed += step * direction;
    }
    audio_pre_ff_rewind();
    audio_ff_rewind(elapsed);
}


static void gwps_leave_wps(bool theme_enabled)
{
    FOR_NB_SCREENS(i)
    {
        struct gui_wps *gwps = skin_get_gwps(WPS, i);
        gwps->display->scroll_stop();
        if (theme_enabled)
        {
#ifdef HAVE_BACKDROP_IMAGE
            skin_backdrop_show(sb_get_backdrop(i));

            /* The following is supposed to erase any traces of %VB
               viewports drawn by the WPS. May need further thought... */
            struct wps_data *sbs = skin_get_gwps(CUSTOM_STATUSBAR, i)->data;
            if (gwps->data->use_extra_framebuffer && sbs->use_extra_framebuffer)
                skin_update(CUSTOM_STATUSBAR, i, SKIN_REFRESH_ALL);
#endif
            viewportmanager_theme_undo(i, skin_has_sbs(gwps));
        }
    }

    /* unhandle statusbar update delay */
    sb_skin_set_update_delay(DEFAULT_UPDATE_DELAY);
}

static void restore_theme(void)
{
    FOR_NB_SCREENS(i)
    {
        struct gui_wps *gwps = skin_get_gwps(WPS, i);
        struct screen *display = gwps->display;
        display->scroll_stop();
        viewportmanager_theme_enable(i, skin_has_sbs(gwps), NULL);
    }
}

/*
 * display the wps on entering or restoring */
static void gwps_enter_wps(bool theme_enabled)
{
    struct gui_wps *gwps;
    struct screen *display;
    if (theme_enabled)
        restore_theme();
    FOR_NB_SCREENS(i)
    {
        gwps = skin_get_gwps(WPS, i);
        display = gwps->display;
        display->scroll_stop();
        sb_set_title_text(NULL, Icon_NOICON, i);
        /* Update the values in the first (default) viewport - in case the user
           has modified the statusbar or colour settings */
#if LCD_DEPTH > 1
        if (display->depth > 1)
        {
            struct skin_viewport *svp = skin_find_item(VP_DEFAULT_LABEL_STRING,
                                                       SKIN_FIND_VP, gwps->data);
            if (svp)
            {
                struct viewport *vp = &svp->vp;
                vp->fg_pattern = display->get_foreground();
                vp->bg_pattern = display->get_background();
            }
        }
#endif
        /* make the backdrop actually take effect */
#ifdef HAVE_BACKDROP_IMAGE
        skin_backdrop_show(gwps->data->backdrop_id);
#endif
        display->clear_display();
        skin_update(WPS, i, SKIN_REFRESH_ALL);
    }
     /* Screen was cleared, so redraw SBS if enabled, and update screen */
    send_event(GUI_EVENT_ACTIONUPDATE, (void*)1);
}

static long do_wps_exit(long action, bool bookmark)
{
    audio_pause();
    update_non_static();
    if (bookmark)
        bookmark_autobookmark(true);
    audio_stop();

    gwps_leave_wps(true);
    (void)action;
    /* GO_TO_PREVIOUS, like the ACTION_WPS_BROWSE exit: _BROWSER resolves a
     * stale last_browser (default 0 = filetree) and lands in the file browser. */
    return GO_TO_PREVIOUS;
}

/* The WPS can be left in two ways:
 *      a)  call a function, which draws over the wps. In this case, the wps
 *          will be still active (i.e. the below function didn't return)
 *      b)  return with a value evaluated by root_menu.c, in this case the wps
 *          is really left, and root_menu will handle the next screen
 *
 * In either way, call gwps_leave_wps(true), in order to restore the correct
 * "main screen" backdrops and statusbars
 */
/* ---- Now Playing as a trimpod_page ------------------------------------- *
 * The While Playing Screen runs on the shared trimpod_page_run() loop
 * (poll -> on_action -> draw), with its state in struct wps_page, so it is a
 * page like every other Trimpod screen.  It drives the stock skin-engine
 * renderer (refresh classes, peak-meter fast path, scroll thread). */
struct wps_page
{
    struct trimpod_page base;
    long button;          /* last (party/ab-processed) action, for storage_spin */
    long last_left, last_right;
    long result;          /* GO_TO_* handed back to the root dispatch */
    bool restore;         /* (re-)enter the WPS theme on the next draw */
    bool exit;            /* leave via do_wps_exit() */
    bool bookmark;        /* autobookmark on exit */
    bool update;          /* force a non-static skin update on the next draw */
    bool theme_enabled;   /* pass-through to gwps_enter_wps() */
    bool lyrics_visible;
    bool lyrics_refresh_header;
    int lyrics_manual_index; /* -1 = follow playback */
    long lyrics_manual_tick;
    char lyrics_track_path[MAX_PATH];
};

#define WPS_LYRICS_X             0
#define WPS_LYRICS_W             LCD_WIDTH
#ifdef TRIMPOD_H700
/* Same overlay as tg5040, fitted to the RG34XXSP WPS: preserve the playlist
 * number/title above y=91 and the progress strip from y=192 down. */
#define WPS_LYRICS_Y             46
#define WPS_LYRICS_H             146
#define WPS_LYRICS_CONTENT_Y     45
#define WPS_LYRICS_AROUND        1
#else
#define WPS_LYRICS_Y             74  /* original metadata/spectrum area */
#define WPS_LYRICS_H             233 /* keep y=307..383 progress/volume strip */
#define WPS_LYRICS_CONTENT_Y     56  /* screen y=130: below number + title */
#define WPS_LYRICS_AROUND        2
#endif
#define WPS_LYRICS_WRAP_ROWS     2
#define WPS_LYRICS_ROW_GAP       4
#define WPS_LYRICS_MANUAL_TICKS  (5 * HZ)

struct wps_wrapped_lyric
{
    char row[WPS_LYRICS_WRAP_ROWS][256];
    int rows;
    int font;
    int line_height;
};

static int wps_lyrics_fit_row(struct screen *s, const char *text,
                              int max_width, char *dst, size_t dst_size,
                              const char **next)
{
    const char *p = text;
    const char *fit = text;
    const char *last_space = NULL;
    char trial[256];

    while (*p)
    {
        int bytes = utf8seek((const unsigned char *)p, 1);
        const char *after;
        size_t len;
        int width;

        if (bytes <= 0)
            bytes = 1;
        after = p + bytes;
        len = (size_t)(after - text);
        if (len >= sizeof(trial))
            break;
        memcpy(trial, text, len);
        trial[len] = '\0';
        s->getstringsize((const unsigned char *)trial, &width, NULL);
        if (width > max_width)
            break;
        fit = after;
        if (*p == ' ' || *p == '\t')
            last_space = p;
        p = after;
    }

    if (!*p)
        fit = p;
    else if (last_space && last_space > text)
        fit = last_space;
    else if (fit == text)
    {
        int bytes = utf8seek((const unsigned char *)text, 1);
        fit = text + (bytes > 0 ? bytes : 1);
    }

    size_t len = (size_t)(fit - text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'))
        len--;
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, text, len);
    dst[len] = '\0';

    p = fit;
    while (*p == ' ' || *p == '\t')
        p++;
    *next = p;
    return (int)len;
}

static void wps_lyrics_wrap(struct screen *s, const char *text, int font,
                            int max_width, struct wps_wrapped_lyric *wrapped)
{
    const char *p = text;
    memset(wrapped, 0, sizeof(*wrapped));
    wrapped->font = font;
    s->setfont(font);
    s->getstringsize((const unsigned char *)"Ag", NULL,
                     &wrapped->line_height);

    while (*p && wrapped->rows < WPS_LYRICS_WRAP_ROWS)
    {
        const char *next;
        wps_lyrics_fit_row(s, p, max_width,
                           wrapped->row[wrapped->rows],
                           sizeof(wrapped->row[wrapped->rows]), &next);
        wrapped->rows++;
        if (next <= p)
            break;
        p = next;
    }
}

static void wps_lyrics_center_text(struct screen *s, const char *text,
                                   int y, int width)
{
    int text_width;
    s->getstringsize((const unsigned char *)text, &text_width, NULL);
    s->putsxy(MAX(0, (width - text_width) / 2), y,
              (const unsigned char *)text);
}

static void wps_lyrics_sync_track(struct wps_page *w,
                                  const struct wps_state *state)
{
    if (!state->id3 || !state->id3->path[0])
        return;

    if (strcmp(w->lyrics_track_path, state->id3->path))
    {
        snprintf(w->lyrics_track_path, sizeof(w->lyrics_track_path), "%s",
                 state->id3->path);
        w->lyrics_manual_index = -1;
    }

    /* Also revalidate the same path: fast-skip preview metadata may be
     * replaced later by the complete artist/title without another path change. */
    trimpod_lyrics_request(state->id3);
}

static void wps_lyrics_stop_covered_scrollers(void)
{
    struct gui_wps *gwps = skin_get_gwps(WPS, SCREEN_MAIN);
    struct wps_data *data = gwps->data;
    char *skin_buffer = get_skin_buffer(data);
    struct skin_element *element;
    const int top = WPS_LYRICS_Y + WPS_LYRICS_CONTENT_Y;
    const int bottom = WPS_LYRICS_Y + WPS_LYRICS_H;

    if (!skin_buffer)
        return;

    for (element = SKINOFFSETTOPTR(skin_buffer, data->tree);
         element;
         element = SKINOFFSETTOPTR(skin_buffer, element->next))
    {
        struct skin_viewport *skin_vp =
            SKINOFFSETTOPTR(skin_buffer, element->data);
        struct viewport *vp;

        if (!skin_vp)
            continue;
        vp = &skin_vp->vp;
        if (vp->y < bottom && vp->y + vp->height > top)
            gwps->display->scroll_stop_viewport(vp);
    }
}

static void wps_lyrics_draw(struct wps_page *w, struct wps_state *state)
{
    struct screen *s = &screens[SCREEN_MAIN];
    struct viewport vp = {0};
    struct viewport *old_vp;
    enum trimpod_lyrics_status status;
    int old_font = lcd_getfont();
    int old_mode = lcd_get_drawmode();
    unsigned old_fg = s->get_foreground();
    unsigned old_bg = s->get_background();

    wps_lyrics_sync_track(w, state);
    trimpod_lyrics_poll();
    if (w->lyrics_manual_index >= 0 &&
        TIME_AFTER(current_tick,
                   w->lyrics_manual_tick + WPS_LYRICS_MANUAL_TICKS))
        w->lyrics_manual_index = -1;

    viewport_set_defaults(&vp, SCREEN_MAIN);
    vp.x = WPS_LYRICS_X;
    vp.y = WPS_LYRICS_Y;
    vp.width = WPS_LYRICS_W;
    vp.height = WPS_LYRICS_H;
    vp.font = FONT_UI;
    old_vp = s->set_viewport(&vp);
    s->set_drawmode(DRMODE_SOLID);
    s->set_background(global_settings.bg_color);
    s->set_foreground(global_settings.bg_color);
    /* Preserve the playlist number and track title already rendered at
     * y=74..129, while replacing artist/album/spectrum below them.  Keeping
     * the taller viewport lets the five lyric lines sit almost as high as in
     * the original full metadata-area overlay. */
    s->fillrect(0, WPS_LYRICS_CONTENT_Y, vp.width,
                vp.height - WPS_LYRICS_CONTENT_Y);
    s->set_foreground(global_settings.fg_color);

    status = trimpod_lyrics_get_status();
    if (status != TRIMPOD_LYRICS_READY)
    {
        const char *message;
        int height;
        s->setfont(FONT_UI);
        if (status == TRIMPOD_LYRICS_LOADING)
            message = str(LANG_TRIMPOD_LYRICS_LOADING);
        else if (status == TRIMPOD_LYRICS_NOT_FOUND)
            message = str(LANG_TRIMPOD_LYRICS_NOT_FOUND);
        else
            message = str(LANG_TRIMPOD_LYRICS_ERROR);
        s->getstringsize((const unsigned char *)message, NULL, &height);
        wps_lyrics_center_text(s, message, (vp.height - height) / 2,
                               vp.width);
    }
    else
    {
        struct wps_wrapped_lyric wrapped[WPS_LYRICS_AROUND * 2 + 1];
        int count = trimpod_lyrics_count();
        int current = trimpod_lyrics_current(
            state->id3 ? state->id3->elapsed : 0);
        int anchor = w->lyrics_manual_index >= 0 ?
                     w->lyrics_manual_index : current;
        int first, last, entries, total_height = 0, y;

        if (anchor < 0) anchor = 0;
        if (anchor >= count) anchor = count - 1;
        first = MAX(0, anchor - WPS_LYRICS_AROUND);
        last = MIN(count - 1, anchor + WPS_LYRICS_AROUND);
        entries = last - first + 1;

        for (int i = 0; i < entries; i++)
        {
            int index = first + i;
            const struct trimpod_lyric_line *line = trimpod_lyrics_line(index);
            int font = index == current ? FONT_UI : 2;
            wps_lyrics_wrap(s, line ? line->text : "", font,
                            vp.width - 32, &wrapped[i]);
            total_height += wrapped[i].rows * wrapped[i].line_height;
            if (i + 1 < entries)
                total_height += WPS_LYRICS_ROW_GAP;
        }

        y = MAX(WPS_LYRICS_CONTENT_Y,
                (vp.height - total_height) / 2);
        for (int i = 0; i < entries; i++)
        {
            s->setfont(wrapped[i].font);
            for (int row = 0; row < wrapped[i].rows; row++)
            {
                wps_lyrics_center_text(s, wrapped[i].row[row], y, vp.width);
                y += wrapped[i].line_height;
            }
            y += WPS_LYRICS_ROW_GAP;
        }
    }

    s->set_viewport(old_vp);
    s->setfont(old_font);
    s->set_drawmode(old_mode);
    s->set_foreground(old_fg);
    s->set_background(old_bg);
}

static void wps_page_draw(struct trimpod_page *p)
{
    struct wps_page *w = (struct wps_page *)p;
    struct wps_state *state = get_wps_state();

    bool audio_paused = (audio_status() & AUDIO_STATUS_PAUSE)?true:false;
    /* did someone else (i.e power thread) change audio pause mode? */
    if (state->paused != audio_paused) {
        state->paused = audio_paused;

        /* if another thread paused audio, we are probably in car mode,
           about to shut down. lets save the settings. */
        if (state->paused) {
            settings_save();
            call_storage_idle_notifys(true);
        }
    }

    if (w->restore)
    {
        w->restore = false;
    /* we remove the update delay since it's not very usable in the wps,
     * e.g. during volume changing or ffwd/rewind */
        sb_skin_set_update_delay(0);
        skin_request_full_update(WPS);
        w->update = true;
        gwps_enter_wps(w->theme_enabled);
        w->theme_enabled = true;
    }
    else
    {
        gwps_caption_backlight(state);

        FOR_NB_SCREENS(i)
        {
            {
                /* Refresh dynamic content every tick, as stock Rockbox does:
                 * the progress bar / time counters advance and the %mv volume
                 * conditional re-evaluates -- which is how the volume bar reverts
                 * to the progress bar on its own ~1s after the last change.
                 * skin_update() only draws into the framebuffer (it never
                 * presents -- the peak-meter loop owns presents) and only tokens
                 * whose value changed actually repaint, so this is cheap.  Do
                 * not gate this on "changed" -- that freezes the %mv swap. */
                bool full_update = skin_do_full_update(WPS, i);
                skin_update(WPS, i, full_update ?
                             SKIN_REFRESH_ALL : SKIN_REFRESH_NON_STATIC);
            }
        }
        w->update = false;
    }

    if (w->lyrics_visible)
    {
        /* Artist/album marquee callbacks run outside the WPS draw loop. Stop
         * only the viewports hidden by lyrics so they cannot repaint through
         * the overlay; keep the track title and bottom strip scrolling/live. */
        wps_lyrics_stop_covered_scrollers();
        wps_lyrics_draw(w, state);
        /* %pm is normally animated from skin_wait_for_action(), after this
         * page draw. Disable that fast path while lyrics cover the spectrum;
         * the next ordinary skin render re-enables it automatically. */
        FOR_NB_SCREENS(i)
            skin_get_gwps(WPS, i)->data->spectrum_enabled = false;
    }

    if (w->lyrics_visible || w->lyrics_refresh_header)
    {
        w->lyrics_refresh_header = false;
        trimpod_header_refresh();
        if (!w->lyrics_visible)
            lcd_update();
    }

    /* With %pm disabled, skin_wait_for_action() no longer owns presentation.
     * Present the complete lyrics frame (including the restored SBS header)
     * once at the normal 5 Hz WPS cadence. */
    if (w->lyrics_visible)
        lcd_update();
}

static int wps_page_poll(struct trimpod_page *p, int timeout)
{
    struct wps_page *w = (struct wps_page *)p;
    (void)timeout;   /* the WPS sets its own cadence (peak-meter fast path) */

    if (w->button && !IS_SYSEVENT(w->button) )
        storage_spin();

    /* 5Hz keeps the counters/progress bar live on screen; while the panel is
     * dark drop to 1Hz -- each pass redraws the whole WPS (art, spectrum, text)
     * into the framebuffer, and nobody is looking.  A button still returns
     * immediately, and 1s-stale content on wake is imperceptible. */
    long button = skin_wait_for_action(WPS, CONTEXT_WPS|ALLOW_SOFTLOCK,
                                       lcd_panel_is_dark() ? HZ : HZ/5);

    /* Exit if audio has stopped playing. This happens e.g. at end of
       playlist or if using the sleep timer. */
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        w->exit = true;

    w->button = button;
    return button;
}

/* MENU on Now Playing opens this popup (returns NP_ADD_TO_PLAYLIST /
 * NP_START_VIZ; GO_TO_PREVIOUS on cancel).  Sleep Timer is an inline knob: LEFT/RIGHT
 * (or A) cycle Off/5/10.../90 min and arm the timer live; the value shows minutes left.
 * It uses the current WPS theme and does not leave the skin while open. */
enum { NP_PAUSE = 0, NP_ADD_TO_PLAYLIST, NP_START_VIZ };

static const int np_sleep_min[] = { 0, 5, 10, 15, 30, 45, 60, 75, 90 };
#define NP_SLEEP_N ((int)(sizeof np_sleep_min / sizeof np_sleep_min[0]))

/* Stateless: the current preset is the one nearest the live time remaining. */
static int np_sleep_cur_index(void)
{
    int sec = get_sleep_timer();
    if (sec <= 0)
        return 0;                       /* Off */
    int m = (sec + 59) / 60, best = 1, bestd = 1000000;
    for (int i = 1; i < NP_SLEEP_N; i++)
    {
        int d = np_sleep_min[i] - m;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static const char *np_sleep_get(void *ctx, char *buf, int len)
{
    (void)ctx;
    int sec = get_sleep_timer();
    if (sec <= 0)
        return str(LANG_OFF);
    snprintf(buf, len, "%dm", (sec + 59) / 60);   /* minutes left, round up; 'm' = our selector standard */
    return buf;
}

static void np_sleep_cycle(void *ctx, int dir)       /* resets + arms live */
{
    (void)ctx;
    int idx = np_sleep_cur_index() + (dir < 0 ? -1 : 1);
    if (idx < 0) idx = 0;
    if (idx >= NP_SLEEP_N) idx = NP_SLEEP_N - 1;
    set_sleeptimer_duration(np_sleep_min[idx]);      /* 0 = Off/disarm */
}

static const struct menu_value_cb np_sleep_value =
    { np_sleep_get, np_sleep_cycle, NULL };

/* Pause/Play: label-only value row. A (or LEFT/RIGHT) toggles playback in place
 * and the row relabels; the menu stays open until B. */
static const char *np_pause_get(void *ctx, char *buf, int len)
{
    (void)ctx; (void)buf; (void)len;
    return str((audio_status() & AUDIO_STATUS_PAUSE) ? LANG_PLAY : LANG_PAUSE);
}
static void np_pause_cycle(void *ctx, int dir)
{
    (void)ctx; (void)dir;
    get_wps_state()->paused = (audio_status() & AUDIO_STATUS_PAUSE) != 0;
    wps_do_action(WPS_PLAYPAUSE, false);
}
static const struct menu_value_cb np_pause_value =
    { np_pause_get, np_pause_cycle, NULL };
MENUITEM_VALUE(np_pause, "", &np_pause_value, Icon_NOICON);
MENUITEM_VALUE(np_sleeptimer, ID2P(LANG_SLEEP_TIMER), &np_sleep_value, Icon_NOICON);
MENUITEM_RETURNVALUE(np_addpl, ID2P(LANG_ADD_TO_PL), NP_ADD_TO_PLAYLIST,
                     NULL, Icon_NOICON);
MENUITEM_RETURNVALUE(np_viz, ID2P(LANG_TRIMPOD_START_VISUALIZER), NP_START_VIZ,
                     NULL, Icon_NOICON);
/* Shuffle/Repeat: inline setting rows (LEFT/RIGHT cycle, applied live to the
 * running playlist via their settings_list callbacks); auto-labelled. */
MENUITEM_SETTING(np_shuffle, &global_settings.playlist_shuffle, NULL);
MENUITEM_SETTING(np_repeat, &global_settings.repeat_mode, NULL);
MAKE_MENU(nowplaying_menu, ID2P(LANG_NOW_PLAYING), NULL, Icon_NOICON,
          &np_pause, &np_shuffle, &np_repeat, &np_sleeptimer, &np_addpl, &np_viz);

static enum trimpod_page_result wps_page_on_action(struct trimpod_page *p,
                                                   int button)
{
    struct wps_page *w = (struct wps_page *)p;
    struct wps_state *state = get_wps_state();
    bool hotkey = false;

        switch(button)
        {
            case ACTION_TP_LYRICS_TOGGLE:
                w->lyrics_visible = !w->lyrics_visible;
                w->lyrics_manual_index = -1;
                w->lyrics_refresh_header = true;
                if (w->lyrics_visible)
                    wps_lyrics_sync_track(w, state);
                else
                {
                    skin_request_full_update(WPS);
                    skin_request_full_update(CUSTOM_STATUSBAR);
                }
                w->update = true;
                break;

            case ACTION_TP_LYRICS_UP:
            case ACTION_TP_LYRICS_DOWN:
                if (w->lyrics_visible &&
                    trimpod_lyrics_get_status() == TRIMPOD_LYRICS_READY)
                {
                    int count = trimpod_lyrics_count();
                    int index = w->lyrics_manual_index;
                    if (index < 0)
                        index = trimpod_lyrics_current(
                            state->id3 ? state->id3->elapsed : 0);
                    index += button == ACTION_TP_LYRICS_UP ? -1 : 1;
                    w->lyrics_manual_index = MAX(0, MIN(count - 1, index));
                    w->lyrics_manual_tick = current_tick;
                    w->update = true;
                }
                break;

#ifdef HAVE_HOTKEY
            case ACTION_WPS_HOTKEY:
            {
                hotkey = true;
                int act = HK_CTX_GET(0, global_settings.context_wps);
                if (act == HOTKEY_OFF)
                    break;
                if (get_hotkey(act)->flags & HOTKEY_FLAG_NOSBS)
                {
                    /* leave WPS without re-enabling theme */
                    w->theme_enabled = false;
                    gwps_leave_wps(w->theme_enabled);
                    onplay(state->id3->path,
                           FILE_ATTR_AUDIO, CONTEXT_WPS, hotkey, ONPLAY_NO_CUSTOMACTION);
                    if (!audio_status())
                    {
                        /* re-enable theme since we're returning to SBS */
                        gwps_leave_wps(true);
                        w->result = GO_TO_ROOT;
                        return TRIMPOD_PAGE_DONE;
                    }
                    w->restore = true;
                    break;
                }
            }
            /* fall through */
#endif /* def HAVE_HOTKEY */
            case ACTION_WPS_CONTEXT:
            {
                gwps_leave_wps(true);
                int retval = onplay(state->id3->path,
                       FILE_ATTR_AUDIO, CONTEXT_WPS, hotkey, ONPLAY_NO_CUSTOMACTION);
                /* if music is stopped in the context menu we want to exit the wps */
                if (retval == ONPLAY_MAINMENU
                    || !audio_status())
                    { w->result = GO_TO_ROOT; return TRIMPOD_PAGE_DONE; }
                else if (retval == ONPLAY_PLAYLIST)
                    { w->result = GO_TO_PLAYLIST_VIEWER; return TRIMPOD_PAGE_DONE; }

                w->restore = true;
            }
            break;

            case ACTION_WPS_BROWSE:
                gwps_leave_wps(true);
                /* B backs out of Now Playing -> slide the previous screen in L->R.
                 * GO_TO_PREVIOUS (not _BROWSER): the root dispatch resolves the
                 * real previous screen via last_screen. */
                trimpod_transition_arm_back();
                w->result = GO_TO_PREVIOUS;
                return TRIMPOD_PAGE_DONE;

                /* A tap: play/pause (the tap/hold standard: tap executes).
                 * Sync paused first in case another thread changed it. */
            case ACTION_WPS_PLAY:
                state->paused = (audio_status() & AUDIO_STATUS_PAUSE) != 0;
                wps_do_action(WPS_PLAYPAUSE, true);
                break;

                /* MENU: centered Now Playing popup (pause/shuffle/repeat/sleep/
                 * add-to-playlist/visualizer). Keep the skin entered while the
                 * modal is open; leave it only for an action that opens another
                 * full screen. */
            case ACTION_STD_CONTEXT:
            {
                int sel = do_menu_popup(&nowplaying_menu, NULL);
                if (sel == NP_ADD_TO_PLAYLIST &&
                         state->id3 && state->id3->path[0])
                {
                    gwps_leave_wps(true);
                    trimpod_playlists_pick(state->id3->path, FILE_ATTR_AUDIO);
                    w->restore = true;
                }
                else if (sel == NP_START_VIZ)
                {
                    gwps_leave_wps(true);
                    trimpod_visualizer_run();   /* B returns to Now Playing */
                    w->restore = true;
                }

                if (!audio_status())   /* music stopped while away -> leave WPS */
                    { w->result = GO_TO_ROOT; return TRIMPOD_PAGE_DONE; }
                break;
            }

            case ACTION_WPS_VOLUP: /* fall through */
            case ACTION_WPS_VOLDOWN:
                if (button == ACTION_WPS_VOLUP)
                    adjust_volume(1);
                else
                    adjust_volume(-1);

                setvol();
                FOR_NB_SCREENS(i)
                {
                    skin_update(WPS, i, SKIN_REFRESH_NON_STATIC);
                }
                w->update = false;
                break;
            /* L1 / R1: jump -10s / +10s within the current track. */
            case ACTION_WPS_SEEKFWD:
            case ACTION_WPS_SEEKBACK:
                if ((audio_status() & AUDIO_STATUS_PLAY) && state->id3
                    && state->id3->length)
                {
                    long pos = (long)state->id3->elapsed +
                               (button == ACTION_WPS_SEEKFWD ? 10000 : -10000);
                    if (pos < 0) pos = 0;
                    if (pos > (long)state->id3->length)
                        pos = state->id3->length;
                    audio_pre_ff_rewind();
                    audio_ff_rewind(pos);
                    w->update = true;
                }
                break;

                /* prev / restart */
            case ACTION_WPS_SKIPPREV:
                w->last_left = current_tick;
                play_hop(-1);
                break;

                /* next */
            case ACTION_WPS_SKIPNEXT:
                w->last_right = current_tick;
                play_hop(1);
                break;
            /* menu key functions */
            case ACTION_WPS_MENU:
                gwps_leave_wps(true);
                w->result = GO_TO_ROOT;
                return TRIMPOD_PAGE_DONE;



                /* screen settings */

                /* stop and exit wps */
            case ACTION_WPS_STOP:
                w->bookmark = true;
                w->exit = true;
                break;

            case ACTION_WPS_LIST_BOOKMARKS:
                gwps_leave_wps(true);
                if (bookmark_load_menu() == BOOKMARK_USB_CONNECTED)
                {
                    w->result = GO_TO_ROOT;
                    return TRIMPOD_PAGE_DONE;
                }
                w->restore = true;
                break;

            case ACTION_WPS_CREATE_BOOKMARK:
                gwps_leave_wps(true);
                bookmark_create_menu();
                w->restore = true;
                break;

             /* this case is used by the softlock feature
              * it requests a full update here */
            case ACTION_REDRAW:
                skin_request_full_update(WPS);
                skin_request_full_update(CUSTOM_STATUSBAR); /* if SBS is used */
                break;
            case ACTION_NONE: /* Timeout, do a partial update */
                /* Auto-start the visualizer after the configured idle while
                 * music plays (0 = Never).  Runs the sequence inline rather
                 * than via trimpod_visualizer_maybe_autostart() because the
                 * skin must be left/restored around the run. */
                if (trimpod_visualizer_autostart_due())
                {
                    /* Fade Now Playing to black first (hides the viz load).  A
                     * keypress during the fade cancels -> stay here. */
                    if (trimpod_visualizer_fade_to_black())
                    {
                        w->update = true;   /* repaint Now Playing */
                        break;
                    }
                    gwps_leave_wps(true);
                    trimpod_visualizer_run();   /* loads while black, fades in; B returns */
                    if (!audio_status())
                        { w->result = GO_TO_ROOT; return TRIMPOD_PAGE_DONE; }
                    w->restore = true;
                    break;
                }
                w->update = true;
                ffwd_rew(button, false); /* hopefully fix the ffw/rwd bug */
                break;
            case ACTION_WPS_VIEW_PLAYLIST:
                gwps_leave_wps(true);
                w->result = GO_TO_PLAYLIST_VIEWER;
                return TRIMPOD_PAGE_DONE;
            default:
                switch(default_event_handler(button))
                {   /* music has been stopped by the default handler */
                    case SYS_USB_CONNECTED:
                    case SYS_CALL_INCOMING:
                    case BUTTON_MULTIMEDIA_STOP:
                        gwps_leave_wps(true);
                        w->result = GO_TO_ROOT;
                        return TRIMPOD_PAGE_DONE;
                }
                w->update = true;
                break;
        }

    /* ACTION_WPS_STOP, or audio stopped on its own: leave via do_wps_exit so
       the bookmark/pause/stop bookkeeping runs exactly as it always did. */
    if (w->exit)
    {
        w->result = do_wps_exit(w->button, w->bookmark);
        return TRIMPOD_PAGE_DONE;
    }
    return TRIMPOD_PAGE_STAY;
}

static const struct trimpod_page_vtable wps_page_vtable =
{
    .legend    = NULL,            /* the skin engine paints the SBS header */
    .draw      = wps_page_draw,
    .poll      = wps_page_poll,
    .on_action = wps_page_on_action,
};

long gui_wps_show(void)
{
/* NOTE: if USBAudio ever gets its own DSP channel, this block can go away! */
    struct wps_page w =
    {
        .base = {
            .vt                = &wps_page_vtable,
            .context           = CONTEXT_WPS,
            /* Slide in like every other page (only the very first screen at app
             * launch skips it).  The slide can flicker or show missing data
             * while a track loads -- the destination frame is captured before
             * id3 is ready; left visible rather than hidden by snapping. */
            .no_enter_anim     = trimpod_transition_first_screen(),
            .no_arm_back       = true,  /* B arms its own back slide to browser */
            .no_header_refresh = true,  /* the skin engine owns the SBS header */
            .animated          = true,  /* counters/EQ/spectrum/viz repaint live */
        },
        .result        = GO_TO_ROOT,
        .restore       = true,
        .theme_enabled = true,
        .lyrics_manual_index = -1,
    };

    wps_state_init();

    trimpod_page_run(&w.base);
    trimpod_lyrics_clear();
    if (w.base.home)
    {
        /* Hold-B home: the run loop breaks out before on_action can leave the
         * skin.  w.restore doubles as "the skin is currently left" -- leave
         * when still entered, and also after a themeless leave (NOSBS hotkey),
         * whose viewportmanager theme push is still outstanding. */
        if (!w.restore || !w.theme_enabled)
            gwps_leave_wps(true);
        return GO_TO_ROOT;
    }
    return w.result;
}

struct wps_state *get_wps_state(void)
{
    return &wps_state;
}

/* this is called from the playback thread so NO DRAWING! */
static void track_info_callback(unsigned short id, void *param)
{
    struct wps_state *state = get_wps_state();

    if (id == PLAYBACK_EVENT_TRACK_CHANGE || id == PLAYBACK_EVENT_CUR_TRACK_READY)
    {
        state->id3 = ((struct track_event *)param)->id3;
        if (state->id3->cuesheet)
        {
            cue_find_current_track(state->id3->cuesheet, state->id3->elapsed);
        }
    }
#ifdef AUDIO_FAST_SKIP_PREVIEW
    else if (id == PLAYBACK_EVENT_TRACK_SKIP)
    {
        state->id3 = audio_current_track();
    }
#endif
    if (id == PLAYBACK_EVENT_NEXTTRACKID3_AVAILABLE)
        state->nid3 = audio_next_track();
    skin_request_full_update(WPS);
}

static void wps_state_init(void)
{
    struct wps_state *state = get_wps_state();
    state->paused = false;
    if(audio_status() & AUDIO_STATUS_PLAY)
    {
        state->id3 = audio_current_track();
        state->nid3 = audio_next_track();
    }
    else
    {
        state->id3 = NULL;
        state->nid3 = NULL;
    }
    /* We'll be updating due to restore initialized with true */
    skin_request_full_update(WPS);
    /* add the WPS track event callbacks */
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_info_callback);
    add_event(PLAYBACK_EVENT_NEXTTRACKID3_AVAILABLE, track_info_callback);
    /* Use the same callback as ..._TRACK_CHANGE for when remaining handles have
       finished */
    add_event(PLAYBACK_EVENT_CUR_TRACK_READY, track_info_callback);
#ifdef AUDIO_FAST_SKIP_PREVIEW
    add_event(PLAYBACK_EVENT_TRACK_SKIP, track_info_callback);
#endif
}
