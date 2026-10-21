/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Daniel Stenberg
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

/*
 * Rockbox button functions
 */

#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "button.h"
#include "kernel.h"
#include "thread.h"
#include "backlight.h"
#include "lcd.h"
#include "serial.h"
#include "power.h"
#include "powermgmt.h"
#if defined(HAVE_SDL)
#include <SDL.h>
#if (SDL_MAJOR_VERSION > 1)
#include "button-sdl.h"
#else
#include "button-target.h"
#endif
#endif /* HAVE_SDL */


static long lastbtn;   /* Last valid button status */
/* Trimpod: tick of the last real user button activity.  Unlike powermgmt's
 * last_event_tick (re-pinned to now during playback), this only moves on actual
 * input -- the canonical "user idle" reference (e.g. visualizer auto-start). */
static long last_activity_tick;
long button_last_activity_tick(void) { return last_activity_tick; }
void button_touch_activity(void) { last_activity_tick = current_tick; }
static long last_read; /* Last button status, for debouncing/filtering */
static bool flipped;  /* buttons can be flipped to match the LCD flip */

static bool (*keypress_filter_fn)(int, int);


/* how long until repeat kicks in, in centiseconds */
#define REPEAT_START      (30*HZ/100)

/* The next two make repeat "accelerate", which is nice for lists
 * which begin to scroll a bit faster when holding until the
 * real list acceleration kicks in (this smooths acceleration).
 *
 * Note that touchscreen pointing events are not subject to this
 * acceleration and always use REPEAT_INTERVAL_TOUCH. That value
 * essentially determines the touchscreen polling rate.
 */

/* the speed repeat starts at, in centiseconds */
#define REPEAT_INTERVAL_START   (16*HZ/100)
/* speed repeat finishes at, in centiseconds */
#define REPEAT_INTERVAL_FINISH  (5*HZ/100)
/* repeat interval for touch events */
#define REPEAT_INTERVAL_TOUCH   (4*HZ/100)

static int lastdata = 0;
static int button_read(int *data);


static void button_remote_post(void)
{
}



static void check_audio_peripheral_state(void)
{
}

/* disabled function is shared between Main & Remote LCDs */
static bool filter_first_keypress_disabled(int button, int data)
{
    button_queue_try_post(button, data);
    return false;
}

/* Trimpod: gate on the panel itself -- is_backlight_on() reads "on" whenever the
 * timeout is 0 (Auto Screen Off = Never, the visualizer), even after a power-key
 * blank.  Volume always passes through. */
static bool filter_first_keypress_enabled(int button, int data)
{
#ifdef BUTTON_VOL_UP
    if (!(button & ~(BUTTON_VOL_UP | BUTTON_VOL_DOWN)))
        return filter_first_keypress_disabled(button, data);
#endif
    if (!lcd_panel_is_dark())
    {
        return filter_first_keypress_disabled(button, data);
    }
    return true;
}


static void button_tick(void)
{
    static int count = 0;
    static int repeat_speed = REPEAT_INTERVAL_START;
    static bool repeat = false;
    static bool post = false;
    static bool skip_release = false;
    int diff;
    int btn;
    int data = 0;

    button_remote_post();

    btn = button_read(&data);

    check_audio_peripheral_state();

#ifdef HAS_BUTTON_HOLD
    /* A hold-switch flip produces no key press/release, so on an idle screen the
     * UI thread would not wake to repaint until its next timeout (~1s) and the
     * hold/lock indicator lags well behind the switch.  button_read() above has
     * just refreshed button_hold(); post a harmless wake on the edge so the
     * status bar refreshes within one tick (~10ms).  BUTTON_NONE -> ACTION_NONE,
     * which only triggers the repaint (GUI_EVENT_ACTIONUPDATE) and nothing else. */
    {
        static bool hold_last = false;
        bool hold_now = button_hold();
        if (hold_now != hold_last)
        {
            hold_last = hold_now;
            button_queue_try_post(BUTTON_NONE, 0);
        }
    }
#endif

    /* Find out if a key has been released */
    diff = btn ^ lastbtn;
    if(diff && (btn & diff) == 0)
    {
            if(!skip_release)
                button_queue_try_post(BUTTON_REL | diff, data);
            else
                skip_release = false;
    }
    else
    {
        if ( btn )
        {
            /* normal keypress */
            if ( btn != lastbtn )
            {
                post = true;
                repeat = false;
                repeat_speed = REPEAT_INTERVAL_START;
            }
            else /* repeat? */
            {


                if ( repeat )
                {
                    if (!post)
                        count--;
                    if (count == 0) {
                        post = true;
                        /* yes we have repeat */
                        if (repeat_speed > REPEAT_INTERVAL_FINISH)
                            repeat_speed--;

                        count = repeat_speed;

                    }
                }
                else
                {
                    ++count;
                    if (count > REPEAT_START
                        )
                    {
                        post = true;
                        repeat = true;
                        /* initial repeat */
                        {
                            count = REPEAT_INTERVAL_START;
                        }
                    }
                }
            }
            if ( post )
            {
                if (repeat)
                {
                    /* Trimpod: a swallowed wake press stays swallowed until
                     * released, so holding it cannot start repeating. */
                    if (skip_release)
                        post = false;
                    /* Only post repeat events if the queue is empty,
                     * to avoid afterscroll effects. */
                    else if (button_queue_try_post(BUTTON_REPEAT | btn, data))
                    {
                        skip_release = false;
                        post = false;
                        /* Need to post back/buttonlight_on() on repeat buttons */
                        {
                            backlight_on();
                            buttonlight_on();
                        }
                    }
                }
                else
                {
                    {
                        skip_release = keypress_filter_fn(btn, data);
                        backlight_on();
                        buttonlight_on();
                    }
                    post = false;
                }
                reset_poweroff_timer();
                last_activity_tick = current_tick;
            }
        }
        else
        {
            repeat = false;
            count = 0;
        }
    }
    lastbtn = btn & ~(BUTTON_REL | BUTTON_REPEAT);

    lastdata = data;
}

void button_init(void)
{
    int temp;
    /* Init used objects first */
    button_queue_init();

    /* hardware inits */
    button_init_device();

    button_read(&temp);
    lastbtn = button_read(&temp);

    reset_poweroff_timer();

    flipped = false;
    set_backlight_filter_keypress(false);
    /* Start polling last */
    tick_add_task(button_tick);
}

#ifdef BUTTON_DRIVER_CLOSE
void button_close(void)
{
    tick_remove_task(button_tick);
}
#endif /* BUTTON_DRIVER_CLOSE */


void set_backlight_filter_keypress(bool value)
{
    if (!value)
        keypress_filter_fn = filter_first_keypress_disabled;
    else
        keypress_filter_fn = filter_first_keypress_enabled;
}

/*
 * Get button pressed from hardware
 */

static int button_read(int *data)
{
#ifdef HAVE_BUTTON_DATA
    int btn = button_read_device(data);
#else
    (void) data;
    int btn = button_read_device();
#endif
    int retval;


    /* Filter the button status. It is only accepted if we get the same
       status twice in a row. */
    if (btn != last_read)
        retval = lastbtn;
    else
        retval = btn;
    last_read = btn;

    return retval;
}

int button_status(void)
{
    return lastbtn;
}

#ifdef HAVE_BUTTON_DATA
int button_status_wdata(int *pdata)
{
    *pdata = lastdata;
    return lastbtn;
}
#endif




