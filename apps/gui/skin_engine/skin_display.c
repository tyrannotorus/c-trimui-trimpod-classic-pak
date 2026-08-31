/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002-2007 Björn Stenberg
 * Copyright (C) 2007-2008 Nicolas Pennequin
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
#include "config.h"
#include <stdio.h>
#include "string-extra.h"
#include "core_alloc.h"
#include "misc.h"
#include "font.h"
#include "system.h"
#include "rbunicode.h"
#include "sound.h"
#include "powermgmt.h"
#include "action.h"
#include "abrepeat.h"
#include "lang.h"
#include "language.h"
#include "statusbar.h"
#include "settings.h"
#include "scrollbar.h"
#include "screen_access.h"
#include "lcd.h"   /* lcd_update / lcd_set_update_suppressed: batch the meter+SBS */
#include "line.h"
#include "playlist.h"
#include "audio.h"
#include "list.h"
#include "option_select.h"
#include "buffering.h"

#include "trimpod_spectrum.h"
#include "trimpod_eq.h"

/* Refresh rate for the live audio spectrum (the %pm element). */
#define SPECTRUM_FPS 30
/* Image stuff */
#include "bmp.h"

#include "cuesheet.h"
#include "playback.h"
#include "backdrop.h"
#include "viewport.h"
#include "root_menu.h"

#include "wps.h"
#include "wps_internals.h"
#include "skin_engine.h"
#include "statusbar-skinned.h"
#include "skin_display.h"

static bool dirty[NB_SCREENS];

void skin_render(struct gui_wps *gwps, unsigned refresh_mode);

bool skin_is_dirty(enum screen_type screen)
{
    return dirty[screen] && !(dirty[screen] = false);
}

void skin_mark_dirty(enum screen_type screen)
{
    dirty[screen] = true;
}

/* update a skinned screen, update_type is WPS_REFRESH_* values.
 * Usually it should only be WPS_REFRESH_NON_STATIC
 * A full update will be done if required (skin_do_full_update() == true)
 */
void skin_update(enum skinnable_screens skin, enum screen_type screen,
                 unsigned int update_type)
{
    struct gui_wps *gwps = skin_get_gwps(skin, screen);
    /* This maybe shouldnt be here,
     * This is also safe for skined screen which dont use the id3 */
    struct mp3entry *id3 = get_wps_state()->id3;
    bool cuesheet_update = (id3 != NULL ? cuesheet_subtrack_changed(id3) : false);
    if (cuesheet_update)
        skin_request_full_update(skin);

    bool full = skin_do_full_update(skin, screen);

    if (skin == WPS && full)
    {
        /* A full WPS render clears the entire framebuffer -- including the strip
         * the shared status bar occupies -- then redraws the body and finally
         * re-composites the SBS.  On this target every present pushes the whole
         * window, and a changed scrolling body line fires an immediate present
         * MID-render (scroll_now -> update_viewport_rect), catching the header
         * after the clear but before the SBS recomposite -> the track-change
         * flash.  Make clear -> body -> SBS atomic w.r.t. presentation: suppress
         * presents across all three, then push exactly one frame.  If presents
         * are already suppressed (the spectrum loop owns a per-frame present),
         * leave that owner in charge and don't toggle/present here.  (skin != WPS
         * for the recursive SBS render, so this never recurses.) */
        bool already = lcd_is_update_suppressed();
        if (!already)
            lcd_set_update_suppressed(true);
        skin_render(gwps, SKIN_REFRESH_ALL);
        skin_mark_dirty(screen);
        sb_skin_update(screen, true);
        if (!already)
        {
            lcd_set_update_suppressed(false);
            lcd_update();
        }
    }
    else
    {
        skin_render(gwps, full ? SKIN_REFRESH_ALL : update_type);
        skin_mark_dirty(screen);
    }
}

#ifdef AB_REPEAT_ENABLE

#define DIRECTION_RIGHT 1
#define DIRECTION_LEFT -1

static int ab_calc_mark_x_pos(int mark, int capacity,
        int offset, int size)
{
    return offset + ( (size * mark) / capacity );
}

static void ab_draw_vertical_line_mark(struct screen * screen,
                                              int x, int y, int h)
{
    screen->set_drawmode(DRMODE_COMPLEMENT);
    screen->vline(x, y, y+h-1);
}

static void ab_draw_arrow_mark(struct screen * screen,
                                      int x, int y, int h, int direction)
{
    /* draw lines in decreasing size until a height of zero is reached */
    screen->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    while( h > 0 )
    {
        screen->vline(x, y, y+h-1);
        h -= 2;
        y++;
        x += direction;
        screen->set_drawmode(DRMODE_COMPLEMENT);
    }
}

void ab_draw_markers(struct screen * screen, int capacity,
                     int x, int y, int w, int h)
{
    bool a_set, b_set;
    unsigned int a, b;
    int xa, xb;

    a_set = ab_get_A_marker(&a);
    b_set = ab_get_B_marker(&b);
    xa = ab_calc_mark_x_pos(a, capacity, x, w);
    xb = ab_calc_mark_x_pos(b, capacity, x, w);
    /* if both markers are set, determine if they're far enough apart
    to draw arrows */
    if ( a_set && b_set )
    {
        int arrow_width = (h+1) / 2;
        if ( (xb-xa) < (arrow_width*2) )
        {
            ab_draw_vertical_line_mark(screen, xa, y, h);
            ab_draw_vertical_line_mark(screen, xb, y, h);
            return;
        }
    }

    if (a_set)
        ab_draw_arrow_mark(screen, xa, y, h, DIRECTION_RIGHT);

    if (b_set)
        ab_draw_arrow_mark(screen, xb, y, h, DIRECTION_LEFT);
}

#endif

void draw_progressbar(struct gui_wps *gwps, struct skin_viewport* skin_viewport,
                      int line, struct progressbar *pb)
{
    struct screen *display = gwps->display;
    struct viewport *vp = &skin_viewport->vp;
    struct wps_state *state = get_wps_state();
    struct mp3entry *id3 = state->id3;
    int x = pb->x, y = pb->y, width = pb->width, height = pb->height;
    unsigned long length, end;
    int flags = HORIZONTAL;

    if (height < 0)
        height = font_get(vp->font)->height;

    if (y < 0)
    {
        int line_height = font_get(vp->font)->height;
        /* center the pb in the line, but only if the line is higher than the pb */
        int center = (line_height-height)/2;
        /* if Y was not set calculate by font height,Y is -line_number-1 */
        y = line*line_height + (0 > center ? 0 : center);
    }

    if (pb->type == SKIN_TOKEN_VOLUMEBAR)
    {
        int minvol = sound_min(SOUND_VOLUME);
        int maxvol = sound_max(SOUND_VOLUME);
#if defined(HAVE_PERCEPTUAL_VOLUME)
        length = 1000;
        end = to_normalized_volume(global_status.volume, minvol, maxvol, length);
#else
        length = maxvol - minvol;
        end = global_status.volume - minvol;
#endif
    }
    else if (pb->type == SKIN_TOKEN_BATTERY_PERCENTBAR)
    {
        length = 100;
        end = battery_level_display();
    }
    else if (pb->type == SKIN_TOKEN_PLAYLIST_PERCENTBAR)
    {
        length = playlist_amount();
        end = playlist_get_display_index();
    }
    else if (pb->type == SKIN_TOKEN_LIST_SCROLLBAR)
    {
        int val, min, max;
        skinlist_get_scrollbar(&val, &min, &max);
        end = val - min;
        length = max - min;
    }
    else if (pb->type == SKIN_TOKEN_SETTINGBAR)
    {
        int val, count;
        get_setting_info_for_bar(pb->setting, pb->setting_offset, &count, &val);
        length = count - 1;
        end = val;
    }
    else if (id3 && id3->length)
    {
        length = id3->length;
        end = id3->elapsed + state->ff_rewind_count;
    }
    else
    {
        length = 1;
        end = 0;
    }

    if (!pb->horizontal)
    {
        /* we want to fill upwards which is technically inverted. */
        flags = INVERTFILL;
    }

    if (pb->invert_fill_direction)
    {
        flags ^= INVERTFILL;
    }

    if (pb->nofill)
    {
        flags |= INNER_NOFILL;
    }

    if (pb->noborder)
    {
        flags |= BORDER_NOFILL;
    }

    if (SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider))
    {
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider);
        /* clear the slider */
        screen_clear_area(display, x, y, width, height);

        /* account for the sliders width in the progressbar */
        if (flags&HORIZONTAL)
        {
            width -= img->bm.width;
        }
        else
        {
            height -= img->bm.height;
        }
    }

    if (SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->backdrop))
    {
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->backdrop);
        img->bm.data = core_get_data(img->buflib_handle);
        display->bmp_part(&img->bm, 0, 0, x, y, pb->width, height);
        flags |= DONT_CLEAR_EXCESS;
    }

    if (!pb->nobar)
    {
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->image);
        if (img)
        {
            char *img_data = core_get_data(img->buflib_handle);
            img->bm.data = img_data;
            gui_bitmap_scrollbar_draw(display, &img->bm,
                                    x, y, width, height,
                                    length, 0, end, flags);
        }
        else
            gui_scrollbar_draw(display, x, y, width, height,
                               length, 0, end, flags);
    }

    if (SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider))
    {
        int xoff = 0, yoff = 0;
        int w = width, h = height;
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider);
        img->bm.data = core_get_data(img->buflib_handle);

        if (flags&HORIZONTAL)
        {
            w = img->bm.width;
            xoff = width * end / length;
            if (flags&INVERTFILL)
                xoff = width - xoff;
        }
        else
        {
            h = img->bm.height;
            yoff = height * end / length;
            if (flags&INVERTFILL)
                yoff = height - yoff;
        }
        display->bmp_part(&img->bm, 0, 0, x + xoff, y + yoff, w, h);
    }

    if (pb->type == SKIN_TOKEN_PROGRESSBAR)
    {
        if (id3 && id3->length)
        {
#ifdef AB_REPEAT_ENABLE
            if (ab_repeat_mode_enabled())
                ab_draw_markers(display, id3->length, x, y, width, height);
#endif

            /* Trimpod: no chapter tick marks on the progress bar -- the
             * inverted lines read as artifacts against the themed bar. */
        }
    }
}

/* clears the area where the image was shown */
void clear_image_pos(struct gui_wps *gwps, struct gui_img *img)
{
    if(!gwps)
        return;
    gwps->display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    gwps->display->fillrect(img->x, img->y, img->bm.width, img->subimage_height);
    gwps->display->set_drawmode(DRMODE_SOLID);
}

void wps_draw_image(struct gui_wps *gwps, struct gui_img *img,
                    int subimage, struct viewport* vp)
{
    struct screen *display = gwps->display;
    img->bm.data = core_get_data(img->buflib_handle);
    display->set_drawmode(DRMODE_SOLID);

    if (img->is_9_segment)
        display->nine_segment_bmp(&img->bm, 0, 0, vp->width, vp->height);
    else
        display->bmp_part(&img->bm, 0, img->subimage_height * subimage,
                          img->x, img->y, img->bm.width, img->subimage_height);
}

void wps_display_images(struct gui_wps *gwps, struct viewport* vp)
{
    if(!gwps || !gwps->data || !gwps->display)
        return;
    (void)vp;
    struct wps_data *data = gwps->data;
    struct screen *display = gwps->display;

    /* Start with album art, as it may be drawn over by mask images */
    struct skin_token_list *list = SKINOFFSETTOPTR(get_skin_buffer(data), data->images);
    while (list)
    {
        struct wps_token *token = SKINOFFSETTOPTR(get_skin_buffer(data), list->token);
        struct gui_img *img = NULL;
        if (token)
            img = (struct gui_img*)SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
        if (img) {
            if (img->using_preloaded_icons && img->display >= 0)
            {
                screen_put_icon(display, img->x, img->y, img->display);
            }
            else if (img->loaded)
            {
                if (img->display >= 0)
                {
                    wps_draw_image(gwps, img, img->display, vp);
                }
            }
        }
        list = SKINOFFSETTOPTR(get_skin_buffer(data), list->next);
    }

    display->set_drawmode(DRMODE_SOLID);
}

/* Evaluate the conditional that is at *token_index and return whether a skip
   has ocurred. *token_index is updated with the new position.
*/
int evaluate_conditional(struct gui_wps *gwps, int offset,
                         struct conditional *conditional, int num_options)
{
    if (!gwps)
        return false;

    char result[128];
    const char *value;

    int intval = num_options < 2 ? 2 : num_options;
    /* get_token_value needs to know the number of options in the enum */
    value = get_token_value(gwps, SKINOFFSETTOPTR(get_skin_buffer(gwps->data), conditional->token),
                    offset, result, sizeof(result), &intval);

    /* intval is now the number of the enum option we want to read,
       starting from 1. If intval is -1, we check if value is empty. */
    if (intval == -1)
    {
        if (num_options == 1) /* so %?AA<true> */
            intval = (value && *value) ? 1 : 0; /* returned as 0 for true, -1 for false */
        else
            intval = (value && *value) ? 1 : num_options;
    }
    else if (intval > num_options || intval < 1)
        intval = num_options;

    return intval -1;
}


/* Display a line appropriately according to its alignment format.
   format_align contains the text, separated between left, center and right.
   line is the index of the line on the screen.
   scroll indicates whether the line is a scrolling one or not.
*/
void write_line(struct screen *display, struct align_pos *format_align,
                int line, bool scroll, struct line_desc *linedes)
{
    int left_width = 0;
    int center_width = 0, center_xpos;
    int right_width = 0,  right_xpos;
    int space_width;
    int string_height;
    int scroll_width;
    int viewport_width = display->getwidth();

    /* calculate different string sizes and positions */
    display->getstringsize((unsigned char *)" ", &space_width, &string_height);
    if (format_align->left != 0) {
        display->getstringsize((unsigned char *)format_align->left,
                                &left_width, &string_height);
    }

    if (format_align->right != 0) {
        display->getstringsize((unsigned char *)format_align->right,
                                &right_width, &string_height);
    }

    if (format_align->center != 0) {
        display->getstringsize((unsigned char *)format_align->center,
                                &center_width, &string_height);
    }

    right_xpos = (viewport_width - right_width);
    center_xpos = (viewport_width - center_width) / 2;

    scroll_width = viewport_width;

    /* Checks for overlapping strings.
        If needed the overlapping strings will be merged, separated by a
        space */

    /* CASE 1: left and centered string overlap */
    /* there is a left string, need to merge left and center */
    if (center_width != 0)
    {
        if (left_width != 0 && left_width + space_width > center_xpos) {
            /* replace the former separator '\0' of left and
                center string with a space */
            *(--format_align->center) = ' ';
            /* calculate the new width and position of the merged string */
            left_width = left_width + space_width + center_width;
            /* there is no centered string anymore */
            center_width = 0;
        }
        /* there is no left string, move center to left */
        else if (left_width == 0 && center_xpos < 0) {
            /* move the center string to the left string */
            format_align->left = format_align->center;
            /* calculate the new width and position of the string */
            left_width = center_width;
            /* there is no centered string anymore */
            center_width = 0;
        }
    } /*(center_width != 0)*/

    /* CASE 2: centered and right string overlap */
    /* there is a right string, need to merge center and right */
    if (center_width != 0)
    {
        int center_left_x = center_xpos + center_width;
        if (right_width != 0 && center_left_x + space_width > right_xpos) {
            /* replace the former separator '\0' of center and
                right string with a space */
            *(--format_align->right) = ' ';
            /* move the center string to the right after merge */
            format_align->right = format_align->center;
            /* calculate the new width and position of the merged string */
            right_width = center_width + space_width + right_width;
            right_xpos = (viewport_width - right_width);
            /* there is no centered string anymore */
            center_width = 0;
        }
        /* there is no right string, move center to right */
        else if (right_width == 0 && center_left_x > right_xpos) {
            /* move the center string to the right string */
            format_align->right = format_align->center;
            /* calculate the new width and position of the string */
            right_width = center_width;
            right_xpos = (viewport_width - right_width);
            /* there is no centered string anymore */
            center_width = 0;
        }
    } /*(center_width != 0)*/

    /* CASE 3: left and right overlap
        There is no center string anymore, either there never
        was one or it has been merged in case 1 or 2 */
    /* there is a left string, need to merge left and right */
    if (center_width == 0 && right_width != 0)
    {
        if (left_width != 0 && left_width + space_width > right_xpos) {
            /* replace the former separator '\0' of left and
                right string with a space */
            *(--format_align->right) = ' ';
            /* calculate the new width and position of the string */
            left_width = left_width + space_width + right_width;
            /* there is no right string anymore */
            right_width = 0;
        }
        /* there is no left string, move right to left */
        else if (left_width == 0 && right_xpos < 0) {
            /* move the right string to the left string */
            format_align->left = format_align->right;
            /* calculate the new width and position of the string */
            left_width = right_width;
            /* there is no right string anymore */
            right_width = 0;
        }
    } /* (center_width == 0 && right_width != 0)*/

    if (scroll && ((left_width > scroll_width) ||
                   (center_width > scroll_width) ||
                   (right_width > scroll_width)))
    {
        /* strings can be as large as MAX_LINE which exceeds put_lines()
         * limit for inline strings. Use $t to avoid truncation */
        linedes->scroll = true;
        display->put_line(0, line * string_height, linedes, "$t", format_align->left);
    }
    else
    {
        linedes->scroll = false;
        /* clear the line first */
        display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
        display->fillrect(0, line*string_height, viewport_width, string_height);
        display->set_drawmode(DRMODE_SOLID);

        /* Nasty hack: we output an empty scrolling string,
        which will reset the scroller for that line */
        display->puts_scroll(0, line, (unsigned char *)"");
        line *= string_height;
        center_xpos = (viewport_width-center_width)/2;
        right_xpos = viewport_width-right_width;
        /* print aligned strings. print whole line at once so that %Vs works
         * across the full viewport width */
        char *left   = format_align->left   ?: "";
        char *center = format_align->center ?: "";
        char *right  = format_align->right  ?: "";

        display->put_line(0, line, linedes, "$t$*s$t$*s$t", left_width == 0 ? "" : left ,
                center_xpos - left_width, center_width == 0 ? "" : center,
                right_xpos - center_xpos - center_width, right_width == 0 ? "" : right);
    }
}

void draw_spectrum(struct gui_wps *gwps, int line_number,
                   struct viewport *viewport)
{
    struct wps_data *data = gwps->data;
    if (!data->spectrum_enabled)
        return;

    (void)line_number;
    trimpod_spectrum_draw(gwps->display, viewport);
    trimpod_eq_draw(gwps->display, viewport);   /* EQ curve over the spectrum */
}


bool skin_has_sbs(struct gui_wps *gwps)
{
    struct wps_data *data = gwps->data;

    bool draw = false;
    if (data->wps_sb_tag)
        draw = data->show_sb_on_wps;
    else if (statusbar_position(gwps->display->screen_type) != STATUSBAR_OFF)
        draw = true;
    return draw;
}

/* Wait for a button, driving the audio spectrum at SPECTRUM_FPS meanwhile.
 * Returns as soon as a button arrives, or when `timeout` ticks have passed.
 */
int skin_wait_for_action(enum skinnable_screens skin, int context, int timeout)
{
    int button = ACTION_NONE;
    /* When the audio spectrum is on screen we want a few extra updates to
       animate it smoothly; otherwise don't waste energy. */
    bool spectrum=false;
    FOR_NB_SCREENS(i)
    {
       if(skin_get_gwps(skin, i)->data->spectrum_enabled)
           spectrum = true;
    }

    /* A dark panel shows no spectrum: fall through to the plain blocking wait
     * rather than spinning every tick to animate something invisible. */
    if (lcd_panel_is_dark())
        spectrum = false;

    if (spectrum) {
        long next_refresh = current_tick;
        long next_big_refresh = current_tick + timeout;
        button = BUTTON_NONE;
        while (TIME_BEFORE(current_tick, next_big_refresh)) {
            /* On this target every lcd_update() presents the whole 3.2x
             * upscaled window, so more than one present per frame tears and
             * the shared status bar blanks when the spectrum starts.  Suppress
             * presents for the whole iteration: get_action() fires
             * GUI_EVENT_ACTIONUPDATE -> sb_skin_update (so the SBS is repainted
             * into the framebuffer here), and the spectrum redraw below draws
             * into the same framebuffer; we then present exactly ONCE per
             * frame, with the SBS and the spectrum composited together.
             *
             * Wait for the next frame's deadline rather than polling every tick:
             * each extra wake repaints the whole SBS for no extra present.  The
             * timed wait still returns the instant a button arrives, so this is
             * no less responsive than the poll it replaces. */
            long wait = next_refresh - current_tick;
            if (wait < 0)
                wait = 0;   /* frame is due: don't block, refresh below */

            lcd_set_update_suppressed(true);
            button = get_action(context, wait);
            if (button != ACTION_NONE) {
                lcd_set_update_suppressed(false);
                break;
            }

            /* ">=", not ">": waking exactly on the deadline must refresh, else
             * the zero-length wait above would spin until the tick rolled over. */
            bool refreshed = false;
            if (!TIME_BEFORE(current_tick, next_refresh)) {
                FOR_NB_SCREENS(i)
                {
                    if(skin_get_gwps(skin, i)->data->spectrum_enabled)
                        skin_update(skin, i, SKIN_REFRESH_SPECTRUM);
                    next_refresh += HZ / SPECTRUM_FPS;
                }
                refreshed = true;
            }
            lcd_set_update_suppressed(false);
            if (refreshed)
                lcd_update();   /* single full-window present for this frame */
        }

    }

    /* The spectrum is not on screen -> no additional screen updates needed */
    else
    {
        button = get_action(context, timeout);
    }
    return button;
}
