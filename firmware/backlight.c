/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
 * Additional work by Martin Ritter (2007) and Thomas Martitz (2008)
 *                  for backlight thread fading
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
#include "settings.h"
#include "action.h"
#include "../apps/gui/skin_engine/skin_engine.h"
#include <stdlib.h>
#include "cpu.h"
#include "kernel.h"
#include "thread.h"
#include "i2c.h"
#include "debug.h"
#include "rtc.h"
#include "usb.h"
#include "power.h"
#include "system.h"
#include "button.h"
#include "timer.h"
#include "backlight.h"
#include "lcd.h"
#include "screendump.h"


/*
    Device specific implementation:
    bool backlight_hw_init(void);
    void backlight_hw_on(void);
    void backlight_hw_off(void);
    void backlight_hw_brightness(int brightness);
*/
#include "backlight-target.h"

#if defined(BACKLIGHT_FULL_INIT)

#if (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_SETTING) \
    || (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_HW_REG)
#include "backlight-sw-fading.h"
#endif

#define BACKLIGHT_FADE_IN_THREAD \
    (CONFIG_BACKLIGHT_FADING &  (BACKLIGHT_FADING_SW_SETTING \
                                |BACKLIGHT_FADING_SW_HW_REG \
                                |BACKLIGHT_FADING_PWM) )

#define BACKLIGHT_THREAD_TIMEOUT HZ

enum {
    BACKLIGHT_ON,
    BACKLIGHT_OFF,
    BACKLIGHT_TMO_CHANGED,
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    BACKLIGHT_BRIGHTNESS_CHANGED,
#endif
#if defined(_BACKLIGHT_FADE_BOOST) || defined(_BACKLIGHT_FADE_ENABLE)
    BACKLIGHT_FADE_FINISH,
#endif
#ifdef BACKLIGHT_DRIVER_CLOSE
    BACKLIGHT_QUIT,
#endif
};

static void backlight_thread(void);
static long backlight_stack[DEFAULT_STACK_SIZE/sizeof(long)];
static const char backlight_thread_name[] = "backlight";
static struct event_queue backlight_queue SHAREDBSS_ATTR;
static bool ignore_backlight_on = false;
static int backlight_ignored_timer = 0;
#ifdef BACKLIGHT_DRIVER_CLOSE
static unsigned int backlight_thread_id = 0;
#endif

#ifdef HAVE_BACKLIGHT_BRIGHTNESS
int backlight_brightness = DEFAULT_BRIGHTNESS_SETTING;
#endif
static int backlight_timer SHAREDBSS_ATTR;
static int backlight_timeout_normal = 5*HZ;
static int backlight_on_button_hold = 0;
static void backlight_handle_timeout(void);




#if (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_SETTING) \
    || (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_HW_REG)
static int backlight_fading_type = (FADING_UP|FADING_DOWN);
static int backlight_fading_state = NOT_FADING;
#endif


/* backlight fading */
#if (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_PWM)
#define BL_PWM_INTERVAL 5  /* Cycle interval in ms */
#define BL_PWM_BITS     8
#define BL_PWM_COUNT    (1<<BL_PWM_BITS)

/* s15.16 fixed point variables */
static int32_t bl_fade_in_step  = ((BL_PWM_INTERVAL*BL_PWM_COUNT)<<16)/300;
static int32_t bl_fade_out_step = ((BL_PWM_INTERVAL*BL_PWM_COUNT)<<16)/2000;
static int32_t bl_dim_fraction  = 0;

static int bl_dim_target  = 0;
static int bl_dim_current = 0;
static enum {DIM_STATE_START, DIM_STATE_MAIN} bl_dim_state = DIM_STATE_START;
static bool bl_timer_active = false;

static void backlight_isr(void)
{
    int timer_period = (TIMER_FREQ*BL_PWM_INTERVAL/1000);
    bool idle = false;

    switch (bl_dim_state)
    {
      /* New cycle */
      case DIM_STATE_START:
        bl_dim_current = bl_dim_fraction >> 16;

        if (bl_dim_current > 0 && bl_dim_current < BL_PWM_COUNT)
        {
            _backlight_on_isr();
            timer_period = (timer_period * bl_dim_current) >> BL_PWM_BITS;
            bl_dim_state = DIM_STATE_MAIN;
        }
        else
        {
            if (bl_dim_current)
                _backlight_on_isr();
            else
                _backlight_off_isr();
            if (bl_dim_current == bl_dim_target)
                idle = true;
        }
        if (bl_dim_current < bl_dim_target)
        {
            bl_dim_fraction = MIN(bl_dim_fraction + bl_fade_in_step,
                                  (BL_PWM_COUNT<<16));
        }
        else if (bl_dim_current > bl_dim_target)
        {
            bl_dim_fraction = MAX(bl_dim_fraction - bl_fade_out_step, 0);
        }
        break;

      /* Dim main screen */
      case DIM_STATE_MAIN:
        _backlight_off_isr();
        timer_period = (timer_period * (BL_PWM_COUNT - bl_dim_current))
                     >> BL_PWM_BITS;
        bl_dim_state = DIM_STATE_START;
        break ;
    }
    if (idle)
    {
#if defined(_BACKLIGHT_FADE_BOOST) || defined(_BACKLIGHT_FADE_ENABLE)
        queue_post(&backlight_queue, BACKLIGHT_FADE_FINISH, 0);
#endif
        timer_unregister();
        bl_timer_active = false;

    }
    else
        timer_set_period(timer_period);
}

static void backlight_switch(void)
{
    if (bl_dim_target > (BL_PWM_COUNT/2))
    {
        _backlight_on_normal();
        bl_dim_fraction = (BL_PWM_COUNT<<16);
    }
    else
    {
        _backlight_off_normal();
        bl_dim_fraction = 0;

    }
}

static void backlight_release_timer(void)
{
#ifdef _BACKLIGHT_FADE_BOOST
    cpu_boost(false);
#endif
    timer_unregister();
    bl_timer_active = false;
    backlight_switch();
}

static void backlight_dim(int value)
{
    /* protect from extraneous calls with the same target value */
    if (value == bl_dim_target)
        return;

    bl_dim_target = value;

    if (bl_timer_active)
        return ;

    if (timer_register(0, backlight_release_timer, 2, backlight_isr
                       IF_COP(, CPU)))
    {
#ifdef _BACKLIGHT_FADE_BOOST
        /* Prevent cpu frequency changes while dimming. */
        cpu_boost(true);
#endif
        bl_timer_active = true;
    }
    else
        backlight_switch();
}

static void backlight_setup_fade_up(void)
{
    if (bl_fade_in_step > 0)
    {
#ifdef _BACKLIGHT_FADE_ENABLE
        _backlight_hw_enable(true);
#endif
        backlight_dim(BL_PWM_COUNT);
    }
    else
    {
        bl_dim_target = BL_PWM_COUNT;
        bl_dim_fraction = (BL_PWM_COUNT<<16);
        _backlight_on_normal();
    }
}

static void backlight_setup_fade_down(void)
{
    /* Start fading display if it's not dimmed yet */
    if (bl_fade_out_step > 0 && bl_dim_current > 0)
    {
        backlight_dim(0);
    }
    else
    {
        bl_dim_target = bl_dim_fraction = 0;
        _backlight_off_normal();
    }
}

void backlight_set_fade_in(int value)
{
    if (value > 0)
        bl_fade_in_step = ((BL_PWM_INTERVAL*BL_PWM_COUNT)<<16) / value;
    else
        bl_fade_in_step = 0;
}

void backlight_set_fade_out(int value)
{
    if (value > 0)
        bl_fade_out_step = ((BL_PWM_INTERVAL*BL_PWM_COUNT)<<16) / value;
    else
        bl_fade_out_step = 0;
}

#elif  (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_SETTING) \
    || (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_HW_REG)

void backlight_set_fade_out(bool value)
{
    if(value) /* on */
        backlight_fading_type |= FADING_DOWN;
    else
        backlight_fading_type &= FADING_UP;
}

void backlight_set_fade_in(bool value)
{
    if(value) /* on */
        backlight_fading_type |= FADING_UP;
    else
        backlight_fading_type &= FADING_DOWN;
}

static void backlight_setup_fade_up(void)
{
    if (backlight_fading_type & FADING_UP)
    {
        if (backlight_fading_state == NOT_FADING)
        {
            /* make sure the backlight is at lowest level */
            backlight_hw_on();
        }
        backlight_fading_state = FADING_UP;
    }
    else
    {
        backlight_fading_state = NOT_FADING;
        _backlight_fade_update_state(backlight_brightness);
        backlight_hw_on();
        backlight_hw_brightness(backlight_brightness);
    }
}

static void backlight_setup_fade_down(void)
{
    if (backlight_fading_type & FADING_DOWN)
    {
        backlight_fading_state = FADING_DOWN;
    }
    else
    {
        backlight_fading_state = NOT_FADING;
        _backlight_fade_update_state(MIN_BRIGHTNESS_SETTING-1);
        backlight_hw_off();
#if (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_HW_REG)
        /* write the lowest brightness level to the hardware so that
         * fading up is glitch free */
        backlight_hw_brightness(MIN_BRIGHTNESS_SETTING);
#endif
    }
}
#endif /* CONFIG_BACKLIGHT_FADING */

static inline void do_backlight_off(void)
{
    backlight_timer = 0;
#if BACKLIGHT_FADE_IN_THREAD
    backlight_setup_fade_down();
#else
    backlight_hw_off();
    /* targets that have fading need to start the countdown when done with
     * fading */
#endif
}

/* Update state of backlight according to timeout setting */
static void backlight_update_state(void)
{

    int timeout = backlight_get_current_timeout();

    /* Backlight == OFF in the setting? */
    if (UNLIKELY(timeout < 0))
    {
        do_backlight_off();
#if (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_SETTING) \
    || (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_HW_REG)
        /* necessary step to issue fading down when the setting is selected */
        if (queue_empty(&backlight_queue))
            queue_post(&backlight_queue, SYS_TIMEOUT, 0);
#endif
    }
    else
    {
        backlight_timer = timeout;


#if BACKLIGHT_FADE_IN_THREAD
        backlight_setup_fade_up();
#else
        backlight_hw_on();
#endif
    }
}


static void backlight_queue_wait(struct queue_event *ev)
{
#if  (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_SETTING) \
    || (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_HW_REG)
    if (backlight_fading_state != NOT_FADING)
        queue_wait_w_tmo(&backlight_queue, ev, FADE_DELAY);
    else
#endif
        queue_wait_w_tmo(&backlight_queue, ev, BACKLIGHT_THREAD_TIMEOUT);
}

void backlight_thread(void)
{
    struct queue_event ev;
    bool locked = false;

    while(1)
    {
        backlight_queue_wait(&ev);
        switch(ev.id)
        {   /* These events must always be processed */
#ifdef _BACKLIGHT_FADE_BOOST
            case BACKLIGHT_FADE_FINISH:
                cpu_boost(false);
                break;
#endif
#ifdef _BACKLIGHT_FADE_ENABLE
            case BACKLIGHT_FADE_FINISH:
                _backlight_hw_enable((bl_dim_current|bl_dim_target) != 0);
                break;
#endif

            /* Here for now or else the aggressive init messes up scrolling */
            case SYS_USB_CONNECTED:
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                break;

#ifdef BACKLIGHT_DRIVER_CLOSE
            /* Get out of here */
            case BACKLIGHT_QUIT:
                return;
#endif
        }
        if (locked)
            continue;

        switch(ev.id)
        {   /* These events are only processed if backlight isn't locked */

            case BACKLIGHT_TMO_CHANGED:
            case BACKLIGHT_ON:
                backlight_update_state();
                break;

            case BACKLIGHT_OFF:
                do_backlight_off();
                break;
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
            case BACKLIGHT_BRIGHTNESS_CHANGED:
                backlight_brightness = (int)ev.data;
                backlight_hw_brightness((int)ev.data);
#if  (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_SETTING) \
    || (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_HW_REG)
                /* receive backlight brightness */
                _backlight_fade_update_state((int)ev.data);
#endif
                break;
#endif /* HAVE_BACKLIGHT_BRIGHTNESS */

            case SYS_REBOOT:
            case SYS_POWEROFF:  /* Lock backlight on poweroff so it doesn't */
                locked = true;      /* go off before power is actually cut. */
                /* Trimpod: always show shutdown message -- fall through */
#if CONFIG_CHARGING
            case SYS_CHARGER_CONNECTED:
            case SYS_CHARGER_DISCONNECTED:
#endif
                backlight_update_state();
                break;
            case SYS_TIMEOUT:
                backlight_handle_timeout();
                break;
        }
    } /* end while */
}

static void backlight_timeout_handler(void)
{
    if(backlight_timer > 0)
    {
        backlight_timer -= BACKLIGHT_THREAD_TIMEOUT;
        if(backlight_timer <= 0)
        {
            do_backlight_off();
        }
    }
    if (backlight_ignored_timer > 0)
    {
        backlight_ignored_timer -= BACKLIGHT_THREAD_TIMEOUT;
        if (backlight_ignored_timer <= 0)
            ignore_backlight_on = false;
    }
}

static void backlight_handle_timeout(void)
{
#if  (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_SETTING) \
    || (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_HW_REG)
    if (backlight_fading_state != NOT_FADING)
    {
        if ((_backlight_fade_step(backlight_fading_state)))
        {   /* finished fading */
            backlight_fading_state = NOT_FADING;
        }
    }
    else
#endif /* CONFIG_BACKLIGHT_FADING */
        backlight_timeout_handler();
}

void backlight_init(void)
{
    queue_init(&backlight_queue, true);

    if (backlight_hw_init())
    {
#if (CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_PWM)
        /* If backlight is already on, don't fade in. */
        bl_dim_target = BL_PWM_COUNT;
        bl_dim_fraction = (BL_PWM_COUNT<<16);
#endif
    }
    /* Leave all lights as set by the bootloader here. The settings load will
     * call the appropriate backlight_set_*() functions, only changing light
     * status if necessary. */
#ifdef BACKLIGHT_DRIVER_CLOSE
    backlight_thread_id =
#endif
    create_thread(backlight_thread, backlight_stack,
                  sizeof(backlight_stack), 0, backlight_thread_name
                  IF_PRIO(, PRIORITY_USER_INTERFACE)
                  IF_COP(, CPU));
}

#ifdef BACKLIGHT_DRIVER_CLOSE
void backlight_close(void)
{
    unsigned int thread = backlight_thread_id;

    /* Wait for thread to exit */
    if (thread == 0)
        return;

    backlight_thread_id = 0;

    queue_post(&backlight_queue, BACKLIGHT_QUIT, 0);
    thread_wait(thread);
}
#endif /* BACKLIGHT_DRIVER_CLOSE */

void backlight_on(void)
{
#ifdef RETRO_HANDHELD
    /* The RG34XXSP lid is a hard display-wake inhibit. Volume presses and
     * background UI activity must not light a closed clamshell. */
    if (power_lid_closed())
        return;
#endif
    if(!ignore_backlight_on)
    {
        queue_remove_from_head(&backlight_queue, BACKLIGHT_ON);
        queue_post(&backlight_queue, BACKLIGHT_ON, 0);
    }
}

void backlight_on_ignore(bool value, int timeout)
{
    ignore_backlight_on = value;
    backlight_ignored_timer = timeout;
}

void backlight_off(void)
{
    queue_post(&backlight_queue, BACKLIGHT_OFF, 0);
}

/* returns true when the backlight is on,
 * and optionally when it's set to always off. */
bool is_backlight_on(bool ignore_always_off)
{
    int timeout = backlight_get_current_timeout();
    return (backlight_timer > 0)   /* countdown */
        || (timeout == 0) /* always on */
        || ((timeout < 0) && !ignore_always_off);
}

/* return value in ticks; 0 means always on, <0 means always off */
int backlight_get_current_timeout(void)
{
    if ((backlight_on_button_hold != 0)
#if   defined(HAS_BUTTON_HOLD)
        && button_hold()
#else
        && is_keys_locked()
#endif
        )
        return (backlight_on_button_hold == 2) ? 0 : -1;
        /* always on or always off */
    else
        /* Trimpod: a SINGLE screen-off timer (Auto Screen Off) governs the
         * backlight whether plugged in or not -- no separate charging timeout. */
        return backlight_timeout_normal;
}

void backlight_set_timeout(int value)
{
    backlight_timeout_normal = HZ * value;
    queue_post(&backlight_queue, BACKLIGHT_TMO_CHANGED, 0);
}

/* Trimpod: set the timeout without posting BACKLIGHT_TMO_CHANGED, so the screen
 * isn't woken.  Next backlight_on() picks up the value. */
void backlight_set_timeout_quiet(int value)
{
    backlight_timeout_normal = HZ * value;
}

#if CONFIG_CHARGING
/* Trimpod: a single screen-off timer (Auto Screen Off) applies whether plugged
 * in or not, so there is no charging-specific timeout.  This stays a no-op to
 * satisfy the API function table. */
void backlight_set_timeout_plugged(int value)
{
    (void)value;
}
#endif /* CONFIG_CHARGING */

#ifdef HAS_BUTTON_HOLD
/* Hold button change event handler. */
void backlight_hold_changed(bool hold_button)
{
    skin_request_update_locked(hold_button);
    if (!hold_button || (backlight_on_button_hold > 0))
    {
        /* if unlocked or override in effect */

        /*backlight_on(); REMOVED*/
        queue_remove_from_head(&backlight_queue, BACKLIGHT_ON);
        queue_post(&backlight_queue, BACKLIGHT_ON, 0);
    }
}
#endif /* HAS_BUTTON_HOLD */

void backlight_set_on_button_hold(int index)
{
    if ((unsigned)index >= 3)
        /* if given a weird value, use default */
        index = 0;

    backlight_on_button_hold = index;
    queue_post(&backlight_queue, BACKLIGHT_TMO_CHANGED, 0);
}



#ifdef HAVE_BACKLIGHT_BRIGHTNESS
void backlight_set_brightness(int val)
{
    if (val < MIN_BRIGHTNESS_SETTING)
        val = MIN_BRIGHTNESS_SETTING;
    else if (val > MAX_BRIGHTNESS_SETTING)
        val = MAX_BRIGHTNESS_SETTING;

    queue_post(&backlight_queue, BACKLIGHT_BRIGHTNESS_CHANGED, val);
}
#endif /* HAVE_BACKLIGHT_BRIGHTNESS */


#else /* !defined(BACKLIGHT_FULL_INIT) -- empty dummy functions */

#if !defined(BACKLIGHT_FULL_INIT)
void backlight_init(void)
{
    (void)backlight_hw_init();
    backlight_hw_on();
}
#endif

void backlight_on(void) {}
void backlight_off(void) {}
void backlight_set_timeout(int value) {(void)value;}

bool is_backlight_on(bool ignore_always_off)
{
    (void)ignore_always_off;
    return true;
}
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
void backlight_set_brightness(int val) { (void)val; }
#endif


#endif /* defined(BACKLIGHT_FULL_INIT) */

void buttonlight_on(void) {}
void buttonlight_on_ignore(bool value, int timeout){(void)value;(void)timeout;}
void buttonlight_off(void) {}
void buttonlight_set_timeout(int value) {(void)value;}

void buttonlight_set_brightness(int val) { (void)val; }
