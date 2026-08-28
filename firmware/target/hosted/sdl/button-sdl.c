/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Felix Arends
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

#include <math.h>
#include <stdlib.h>         /* EXIT_SUCCESS */
#include <stdio.h>
#include "sim-ui-defines.h"
#include "config.h"
#include "button.h"
#include "kernel.h"
#include "backlight.h"
#include "system.h"
#include "button-sdl.h"
#if SDL_MAJOR_VERSION > 1
#include "window-sdl.h"
#endif
#include "buttonmap.h"
#include "debug.h"
#include "powermgmt.h"
#include "storage.h"


static int btn = 0;    /* Hopefully keeps track of currently pressed keys... */


int sdl_app_has_input_focus = 1;

#ifdef HAS_BUTTON_HOLD
bool hold_button_state = false;
bool button_hold(void) {
    return hold_button_state;
}
#endif

static void button_event(int key, bool pressed);
extern bool debug_wps;
extern bool mapping;

/* Trimpod: power button.  On-device the power key is KEY_POWER on the
 * axp2202-pek PMIC node, which SDL's evdev layer delivers as scancode
 * SDL_SCANCODE_POWER, so held state is tracked from SDL key events in
 * event_handler (power_key_held); SDLK_x in button_event is the desktop
 * simulator's shortcut.  Timed in button_read_device to match NextUI: a short
 * press (< 1s) toggles the display off/on while music keeps playing, a long
 * press (>= 1s) powers off.  No deep sleep -- a short press only blanks the
 * backlight. */
#define POWER_LONG_PRESS_TICKS  HZ      /* 1s, matches NextUI's 1000ms */
static long power_down_tick = 0;        /* current_tick of power press (0 = up) */
static bool power_blanked   = false;    /* display blanked by a power short press */
static bool power_key_held  = false;    /* set from SDL_SCANCODE_POWER key events */
static bool lid_closed      = false;    /* RG34XXSP hall sensor state */
static bool lid_restore_backlight = false;

bool power_display_off(void) { return power_blanked || lid_closed; }
bool power_lid_closed(void) { return lid_closed; }

/* The TrimUI gamepad is read directly through SDL's joystick layer (no gptokeyb2
 * shim), mirroring NextUI: open every joystick at loop start, translate
 * JOYBUTTON/JOYHAT/JOYAXIS events to Rockbox buttons in event_handler. */
static SDL_Joystick **joysticks = NULL;
static int num_joysticks = 0;
static int hat_directions;
static int stick_directions;

#define JOYSTICK_DEADZONE 16384
#define DIRECTION_BUTTONS (BUTTON_UP | BUTTON_DOWN | BUTTON_LEFT | BUTTON_RIGHT)

static void update_joystick_directions(void)
{
    btn = (btn & ~DIRECTION_BUTTONS) | hat_directions | stick_directions;
}

static void open_joysticks(void)
{
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0)
    {
        DEBUGF("SDL_INIT_JOYSTICK failed: %s\n", SDL_GetError());
        return;
    }
    SDL_JoystickEventState(SDL_ENABLE);
    num_joysticks = SDL_NumJoysticks();
    if (num_joysticks > 0)
    {
        joysticks = malloc(sizeof(*joysticks) * num_joysticks);
        for (int i = 0; i < num_joysticks; i++)
        {
#if SDL_MAJOR_VERSION > 1
            const char *name = SDL_JoystickNameForIndex(i);
            if (!retrohh_should_open_joystick(name))
            {
                joysticks[i] = NULL;
                DEBUGF("Skipping built-in joystick %d: %s (evdev)\n", i,
                       name ? name : "unknown");
                continue;
            }
#endif
            joysticks[i] = SDL_JoystickOpen(i);
        }
    }
}

static void close_joysticks(void)
{
    if (joysticks)
    {
        for (int i = 0; i < num_joysticks; i++)
            if (joysticks[i] && SDL_JoystickGetAttached(joysticks[i]))
                SDL_JoystickClose(joysticks[i]);
        free(joysticks);
        joysticks = NULL;
    }
    num_joysticks = 0;
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}


#if ((defined(BUTTON_SCROLL_FWD) && defined(BUTTON_SCROLL_BACK)))
static void scrollwheel_event(int x, int y)
{
    int new_btn = 0;
    if (y > 0)
        new_btn = BUTTON_SCROLL_BACK;
    else if (y < 0)
        new_btn = BUTTON_SCROLL_FWD;
    else
        return;

    backlight_on();
    reset_poweroff_timer();
    if (new_btn && !button_queue_full())
        button_queue_post(new_btn, 1<<24);

    (void)x;
}
#endif
static void mouse_event(SDL_MouseButtonEvent *event, bool button_up)
{
#define SQUARE(x) ((x)*(x))
    static int x,y;

    if(button_up) {
        switch ( event->button )
        {
        case SDL_BUTTON_MIDDLE:
        case SDL_BUTTON_RIGHT:
            button_event( event->button, false );
            break;
        /* The scrollwheel button up events are ignored as they are queued immediately */
        case SDL_BUTTON_LEFT:
            if ( mapping && background ) {
                printf("    { SDLK_,     %d, %d, %d, \"\" },\n", x, y,
                (int)sqrt( SQUARE(x-(int)event->x) + SQUARE(y-(int)event->y))
                );
            }
            break;
        }
    } else {   /* button down */
        switch ( event->button )
        {
        case SDL_BUTTON_MIDDLE:
        case SDL_BUTTON_RIGHT:
            button_event( event->button, true );
            break;
        case SDL_BUTTON_LEFT:
            if ( mapping && background ) {
                x = event->x;
                y = event->y;
            }
            break;
        }

        if (debug_wps && event->button == SDL_BUTTON_LEFT)
        {
            int m_x, m_y;

            if ( background )
            {
                m_x = event->x - 1;
                m_y = event->y - 1;
                {
                    m_x -= UI_LCD_POSX;
                    m_y -= UI_LCD_POSY;
                }
            }
            else
            {
                m_x = event->x;
                m_y = event->y;
            }

            printf("Mouse at 2: (%d, %d)\n", m_x, m_y);
        }
    }
#undef SQUARE
}

static bool event_handler(SDL_Event *event)
{
#if SDL_MAJOR_VERSION > 1
    SDL_Keycode ev_key;
#else
    SDLKey ev_key;
#endif

    switch(event->type)
    {
#if SDL_MAJOR_VERSION > 1
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            sdl_app_has_input_focus = 1;
        else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            sdl_app_has_input_focus = 0;
        else if(event->window.event == SDL_WINDOWEVENT_RESIZED)
        {
            SDL_LockMutex(window_mutex);
            sdl_window_adjustment_needed();
            SDL_UnlockMutex(window_mutex);
            static unsigned long last_tick;
            if (TIME_AFTER(current_tick, last_tick + HZ/20) && !button_queue_full())
                  button_queue_post(SDLK_UNKNOWN, 0); /* update window on main thread */
            else
                  last_tick = current_tick;
        }
#endif /* SDL_MAJOR_VERSION */
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        /* Power key (axp2202-pek PMIC node) arrives as scancode
         * SDL_SCANCODE_POWER; track held state for the gesture timing in
         * button_read_device.  Everything else is the simulator keymap. */
        if (event->key.keysym.scancode == SDL_SCANCODE_POWER)
        {
            /* H700 reads the PMIC node directly; accepting both paths would
             * create a release edge before evdev has drained. */
            if (!retrohh_h700_active())
                power_key_held = (event->type == SDL_KEYDOWN);
            break;
        }
        ev_key = event->key.keysym.sym;
        button_event(ev_key, event->type == SDL_KEYDOWN);
        break;

    /* Gamepad (read directly via SDL's joystick layer, no gptokeyb2). */
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP:
    {
        int b = joybutton_to_button(event->jbutton.button);
        if (b != BUTTON_NONE)
        {
            if (event->type == SDL_JOYBUTTONDOWN)
                btn |= b;
            else
                btn &= ~b;
        }
        break;
    }
    case SDL_JOYHATMOTION:
    {
        /* Brick d-pad is a hat; keep it independent from the left stick. */
        int hat = event->jhat.value;
        hat_directions = BUTTON_NONE;
        if (hat & SDL_HAT_UP)    hat_directions |= BUTTON_UP;
        if (hat & SDL_HAT_DOWN)  hat_directions |= BUTTON_DOWN;
        if (hat & SDL_HAT_LEFT)  hat_directions |= BUTTON_LEFT;
        if (hat & SDL_HAT_RIGHT) hat_directions |= BUTTON_RIGHT;
        update_joystick_directions();
        break;
    }
    case SDL_JOYAXISMOTION:
    {
        /* Brick Pro left stick (axes 0/1) behaves like the d-pad. */
        if (event->jaxis.axis == 0)
        {
            stick_directions &= ~(BUTTON_LEFT | BUTTON_RIGHT);
            if (event->jaxis.value < -JOYSTICK_DEADZONE)
                stick_directions |= BUTTON_LEFT;
            else if (event->jaxis.value > JOYSTICK_DEADZONE)
                stick_directions |= BUTTON_RIGHT;
            update_joystick_directions();
            break;
        }
        if (event->jaxis.axis == 1)
        {
            stick_directions &= ~(BUTTON_UP | BUTTON_DOWN);
            if (event->jaxis.value < -JOYSTICK_DEADZONE)
                stick_directions |= BUTTON_UP;
            else if (event->jaxis.value > JOYSTICK_DEADZONE)
                stick_directions |= BUTTON_DOWN;
            update_joystick_directions();
            break;
        }

        /* L2/R2 triggers report as axes; any positive deflection = pressed. */
        int b = joyaxis_to_button(event->jaxis.axis);
        if (b != BUTTON_NONE)
        {
            if (event->jaxis.value > 0)
                btn |= b;
            else
                btn &= ~b;
        }
        break;
    }

    case SDL_MOUSEMOTION:
    {
        break;

    }
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEBUTTONDOWN:
    {
        SDL_MouseButtonEvent *mev = &event->button;
        mouse_event(mev, event->type == SDL_MOUSEBUTTONUP);
        break;
    }
    case SDL_QUIT:
        /* Will post SDL_USEREVENT in shutdown_hw() if successful. */
        sdl_sys_quit();
        break;
    case SDL_USEREVENT:
        return true;
        break;
    }

    return false;
}


void gui_message_loop(void)
{
    SDL_Event event;
    bool quit;

    /* Open the gamepad here, on the same thread that pumps SDL events. */
    open_joysticks();

    do {
        /* wait for the next event */
        if(SDL_WaitEvent(&event) == 0) {
            printf("SDL_WaitEvent(): %s\n", SDL_GetError());
            break; /* error, out of here */
        }

        sim_enter_irq_handler();
        quit = event_handler(&event);
        sim_exit_irq_handler();

    } while(!quit);

    close_joysticks();
}


static void button_event(int key, bool pressed)
{
    int new_btn = 0;
    switch (key)
    {
    case SDLK_x:    /* desktop simulator only: the 'X' key = shutdown */
        sys_poweroff();
        break;
#ifdef HAS_BUTTON_HOLD
    case SDLK_h:
        if(pressed)
        {
            hold_button_state = !hold_button_state;
            DEBUGF("Hold button is %s\n", hold_button_state?"ON":"OFF");
        }
        return;
#endif


    default:
            new_btn = key_to_button(key);
        break;
    }

#if defined(BUTTON_SCROLL_FWD) && defined(BUTTON_SCROLL_BACK)
    if((new_btn == BUTTON_SCROLL_FWD || new_btn == BUTTON_SCROLL_BACK) &&
        pressed)
    {
        scrollwheel_event(0, new_btn == BUTTON_SCROLL_FWD ? -1 : 1);
        new_btn &= ~(BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK);
    }
#endif

    /* Update global button press state */
    if (pressed)
        btn |= new_btn;
    else
        btn &= ~new_btn;
}
#if defined(HAVE_BUTTON_DATA)
int button_read_device(int* data)
{
    (void) *data;   /* suppress compiler warnings */
#else
int button_read_device(void)
{
#endif
    bool raw_power = false;
    int raw_buttons = retrohh_evdev_buttons(&raw_power);
    if (retrohh_h700_active())
        power_key_held = raw_power;

    /* RG34XXSP lid policy: close only blanks the display; playback, decoding,
     * Bluetooth and USB audio keep running. Restore the previous display state
     * on open, and never wake a closed panel from a volume key. */
    {
        bool now_closed = retrohh_lid_closed();
        if (now_closed != lid_closed)
        {
            if (now_closed)
            {
                lid_restore_backlight = !power_blanked && is_backlight_on(true);
                lid_closed = true;
                backlight_off();
                btn = BUTTON_NONE;
                hat_directions = BUTTON_NONE;
                stick_directions = BUTTON_NONE;
            }
            else
            {
                lid_closed = false;
                if (lid_restore_backlight)
                    backlight_on();
                lid_restore_backlight = false;
            }
        }
    }

    /* Trimpod: power button (held state tracked from SDL_SCANCODE_POWER events).
     * Match NextUI's timing -- a short press (held < 1s) toggles the display
     * off/on in-app while music keeps playing; a long press (>= 1s) powers off.
     * Run before the hold check so power works even when input is locked. */
    {
        bool held = power_key_held;
        if (held)
        {
            if (!power_down_tick)
                power_down_tick = current_tick ? current_tick : 1;
            else if (current_tick - power_down_tick >= POWER_LONG_PRESS_TICKS)
            {
                power_down_tick = 0;        /* long press -> power off (once) */
                sys_poweroff();
            }
        }
        else if (power_down_tick)           /* released */
        {
            if (!lid_closed &&
                current_tick - power_down_tick < POWER_LONG_PRESS_TICKS)
            {   /* Short press: if the screen is dark -- whether from a manual
                 * blank OR an Auto-Screen-Off timeout -- WAKE it, like any other
                 * button (is_backlight_on() is the real state; power_blanked
                 * covers the always-on cases where it can't tell, e.g. viz).
                 * Otherwise blank it. */
                /* The power key never reaches button_tick's stamp, so mark the
                 * activity here -- a wake must restart the idle timers (viz
                 * auto-start), not leave them expired. */
                button_touch_activity();
                if (power_blanked || !is_backlight_on(true))
                {
                    backlight_on();
                    power_blanked = false;
                }
                else
                {
                    backlight_off();
                    power_blanked = true;
                }
            }
            power_down_tick = 0;
        }
    }

    /* Brick physical hold/lock switch (gpio243): locks INPUT ONLY -- no
     * backlight_hold_changed() or skin_request_update_locked(), which would
     * dim/flash the screen on every toggle. */
    hold_button_state = retrohh_hold_switch();
    if (hold_button_state)
    {
        /* The side switch reports as EV_SW SW_TABLET_MODE on the gamepad node,
         * which SDL's joystick layer ignores (it's not a button), so nothing
         * leaks from the switch itself.  Still discard any button held across
         * the lock edge so it can't survive into the locked state. */
        btn = 0;
        hat_directions = BUTTON_NONE;
        stick_directions = BUTTON_NONE;
        return BUTTON_NONE;
    }

    /* Volume rocker arrives as joystick buttons 14/13 (folded into btn by
     * event_handler, like every other gamepad key) -- no separate read here. */
    int result = btn | raw_buttons;

    /* Closed clamshell accepts only volume and long Power. Power is handled
     * above and is never returned as a UI button. */
    if (lid_closed)
        result &= BUTTON_VOL_UP | BUTTON_VOL_DOWN;

    /* Any non-power button wakes the screen (backlight_on() in button_tick), so
     * a manual power-blank is over -- the next power tap blanks again rather
     * than being treated as a wake. */
    if (result != 0 && !lid_closed)
        power_blanked = false;

    return result;
}

void button_init_device(void)
{
    retrohh_evdev_init();
}
