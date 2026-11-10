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
#ifdef RETRO_HANDHELD
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "trimpod_alsa.h"
#endif
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
#include "debug.h"
#include "powermgmt.h"
#include "storage.h"


static int btn = 0;    /* Hopefully keeps track of currently pressed keys... */
static int released;   /* buttons released since the last button_read_device */


int sdl_app_has_input_focus = 1;

#ifdef HAS_BUTTON_HOLD
bool hold_button_state = false;
bool button_hold(void) {
    return hold_button_state;
}
#endif

#ifdef RETRO_HANDHELD
static void media_key(int code, bool down);
#endif
extern bool debug_wps;
extern bool mapping;

/* Trimpod: power button.  On-device the power key is KEY_POWER on the
 * axp2202-pek PMIC node, which SDL's evdev layer delivers as scancode
 * SDL_SCANCODE_POWER, so held state is tracked from SDL key events in
 * event_handler (power_key_held).  Timed in button_read_device to match
 * NextUI: a short press (< 1s) toggles the display off/on while music keeps
 * playing, a long press (>= 1s) powers off.  No deep sleep -- a short press
 * only blanks the backlight. */
#define POWER_LONG_PRESS_TICKS  HZ      /* 1s, matches NextUI's 1000ms */
static long power_down_tick = 0;        /* current_tick of power press (0 = up) */
static bool power_blanked   = false;    /* display blanked by a power short press */
static bool power_key_held  = false;    /* set from SDL_SCANCODE_POWER key events */

bool power_display_off(void) { return power_blanked; }

/* The TrimUI gamepad is read directly through SDL's joystick layer (no gptokeyb2
 * shim), mirroring NextUI: open every joystick at loop start, translate
 * JOYBUTTON/JOYHAT/JOYAXIS events to Rockbox buttons in event_handler. */
static SDL_Joystick **joysticks = NULL;
static int num_joysticks = 0;

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
            joysticks[i] = SDL_JoystickOpen(i);
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


static void mouse_event(SDL_MouseButtonEvent *event, bool button_up)
{
#define SQUARE(x) ((x)*(x))
    static int x,y;

    if(button_up) {
        switch ( event->button )
        {
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
         * button_read_device.  Of the rest only media keys count, such as a
         * Bluetooth headset's; volume keys come from the pad as joystick
         * buttons. */
        if (event->key.keysym.scancode == SDL_SCANCODE_POWER)
        {
            power_key_held = (event->type == SDL_KEYDOWN);
            break;
        }
#ifdef RETRO_HANDHELD
        {
            static const struct { SDL_Scancode scancode; int key; } media[] = {
                { SDL_SCANCODE_AUDIOPLAY, KEY_PLAYPAUSE },
                { SDL_SCANCODE_AUDIONEXT, KEY_NEXTSONG },
                { SDL_SCANCODE_AUDIOPREV, KEY_PREVIOUSSONG },
            };

            for (unsigned i = 0; i < ARRAYLEN(media); i++)
                if (event->key.keysym.scancode == media[i].scancode &&
                    !event->key.repeat)
                    media_key(media[i].key, event->type == SDL_KEYDOWN);
        }
#endif
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
        /* Brick d-pad is a hat; rebuild the four direction bits each event. */
        int hat = event->jhat.value;
        btn &= ~(BUTTON_UP | BUTTON_DOWN | BUTTON_LEFT | BUTTON_RIGHT);
        if (hat & SDL_HAT_UP)    btn |= BUTTON_UP;
        if (hat & SDL_HAT_DOWN)  btn |= BUTTON_DOWN;
        if (hat & SDL_HAT_LEFT)  btn |= BUTTON_LEFT;
        if (hat & SDL_HAT_RIGHT) btn |= BUTTON_RIGHT;
        break;
    }
    case SDL_JOYAXISMOTION:
    {
        /* L2/R2 triggers report as axes; any positive deflection = pressed
         * (matches NextUI's val > 0 test).  Non-trigger axes map to nothing. */
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


#ifdef RETRO_HANDHELD
/* With a joystick open SDL_WaitEvent polls every 1 ms.  Block in poll() instead,
 * on our own handles to the input nodes SDL reads, plus a pipe that
 * gui_message_loop_wake() writes after an SDL_PushEvent and a watch for nodes
 * that appear later, such as a USB-C headset's. */
#define MAX_INPUT_FDS 8
#define WAKE_SLOT     0
#define WATCH_SLOT    1
#define NODE_SLOT     2                 /* the first input node */
static struct pollfd input_fds[NODE_SLOT + MAX_INPUT_FDS];
static bool input_headset[NODE_SLOT + MAX_INPUT_FDS];
static int num_input_fds = NODE_SLOT;
static int wake_pipe[2] = { -1, -1 };
static int settings_wd = -1;            /* NextUI's settings directory */

static bool has_code(int fd, int type, int code)
{
    unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(unsigned long))] = { 0 };

    if (ioctl(fd, EVIOCGBIT(type, sizeof bits), bits) < 0)
        return false;
    return bits[code / (8 * sizeof bits[0])] >> (code % (8 * sizeof bits[0])) & 1;
}

/* ---- media buttons, by Android's rules but for the voice assistant ------
 * From a USB-C headset (its grabbed node) and from SDL's keyboard events, which
 * carry Bluetooth's.  The play button counts taps: one is play/pause, two next,
 * three previous; a hold does nothing.  A headset's +/- are the volume rocker,
 * repeating while held.  Devices report only down and up: the timing is ours.
 * media_key(), commit_taps() and headset_gone() run with the "interrupt" lock
 * held. */
#define TAP_GAP_MS  300                 /* Android's multi-press timeout */
#define NEXTUI_SETTINGS "msettings.bin" /* in $USERDATA_PATH */
#define HOLD_MS     400                 /* Android's long-press timeout */

static struct
{
    int    taps;
    bool   down, pending;
    Uint32 down_at, commit_at;
} tap;
static int headset_held;                /* BUTTON_VOL_* held on the headset */

/* A play button and no axes: the Brick's own pad is USB too, with volume keys. */
static bool is_headset(int fd)
{
    struct input_id id;

    return ioctl(fd, EVIOCGID, &id) == 0 && id.bustype == BUS_USB &&
           has_code(fd, EV_KEY, KEY_PLAYPAUSE) && !has_code(fd, 0, EV_ABS);
}

static void commit_taps(void)
{
    static const int command[] = {
        0, MEDIA_KEY_PLAYPAUSE, MEDIA_KEY_NEXT, MEDIA_KEY_PREV,
    };

    button_queue_post(SYS_MEDIA_KEY, command[tap.taps]);
    tap.taps = 0;
    tap.pending = false;
}

static void headset_volume(int button, bool down)
{
    int was = btn;

    if (down)
        btn |= button;
    else
        btn &= ~button;
    headset_held = down ? headset_held | button : headset_held & ~button;
    released |= was & ~btn;
}

/* code is a Linux key code; down is press or release. */
static void media_key(int code, bool down)
{
    Uint32 now = SDL_GetTicks();

    switch (code)
    {
        case KEY_VOLUMEUP:
            headset_volume(BUTTON_VOL_UP, down);
            break;
        case KEY_VOLUMEDOWN:
            headset_volume(BUTTON_VOL_DOWN, down);
            break;
        case KEY_NEXTSONG:
            if (down)
                button_queue_post(SYS_MEDIA_KEY, MEDIA_KEY_NEXT);
            break;
        case KEY_PREVIOUSSONG:
            if (down)
                button_queue_post(SYS_MEDIA_KEY, MEDIA_KEY_PREV);
            break;
        case KEY_PLAYPAUSE:
            if (down)
            {
                tap.down = true;
                tap.down_at = now;
                tap.pending = false;    /* another tap may follow */
            }
            else if (tap.down)
            {
                tap.down = false;
                if (now - tap.down_at >= HOLD_MS)
                    tap.taps = 0;       /* the assistant's gesture: unused */
                else if (++tap.taps == 3)
                    commit_taps();
                else
                {
                    tap.pending = true;
                    tap.commit_at = now + TAP_GAP_MS;
                }
            }
            break;
    }
}

static void headset_gone(void)
{
    if (headset_held & BUTTON_VOL_UP)
        headset_volume(BUTTON_VOL_UP, false);
    if (headset_held & BUTTON_VOL_DOWN)
        headset_volume(BUTTON_VOL_DOWN, false);
    tap.down = tap.pending = false;
    tap.taps = 0;
}

/* ---- the poll set ------------------------------------------------------ */

/* A headset's node is grabbed: only Trimpod reads it, so SDL can't act on it too. */
static void add_node(const char *name)
{
    char path[64];
    int i, fd;

    if (strncmp(name, "event", 5))
        return;
    for (i = NODE_SLOT; i < num_input_fds && input_fds[i].fd >= 0; i++)
        ;
    if (i == NODE_SLOT + MAX_INPUT_FDS)
        return;
    snprintf(path, sizeof path, "/dev/input/%s", name);
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    input_fds[i] = (struct pollfd){ .fd = fd, .events = POLLIN };
    input_headset[i] = is_headset(fd) && ioctl(fd, EVIOCGRAB, 1) == 0;
    if (i == num_input_fds)
        num_input_fds++;
}

static void open_input_fds(void)
{
    input_fds[WAKE_SLOT].fd = input_fds[WATCH_SLOT].fd = -1;
    if (pipe(wake_pipe) < 0)
        return;
    for (int i = 0; i < 2; i++)
    {
        fcntl(wake_pipe[i], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    input_fds[WAKE_SLOT] = (struct pollfd){ .fd = wake_pipe[0], .events = POLLIN };

    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;
    struct dirent *de;
    while ((de = readdir(dir)))
        add_node(de->d_name);
    closedir(dir);

    int watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watch >= 0 && inotify_add_watch(watch, "/dev/input", IN_CREATE) < 0)
    {
        close(watch);
        watch = -1;
    }
    /* NextUI ends every volume change, audiomon's switch of output included,
     * by saving NEXTUI_SETTINGS, after its mixer and mute writes: the moment
     * to put Trimpod's volume back.  The directory is watched, so the file
     * needn't exist yet and may be replaced. */
    const char *userdata = getenv("USERDATA_PATH");
    if (watch >= 0 && userdata)
        settings_wd = inotify_add_watch(watch, userdata, IN_CLOSE_WRITE | IN_MOVED_TO);
    input_fds[WATCH_SLOT] = (struct pollfd){ .fd = watch, .events = POLLIN };
}

static void post_event(long id, intptr_t data)
{
    sim_enter_irq_handler();
    button_queue_post(id, data);
    sim_exit_irq_handler();
}

static void read_watch(void)
{
    char buf[512] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t got;

    while ((got = read(input_fds[WATCH_SLOT].fd, buf, sizeof buf)) > 0)
        for (char *p = buf; p < buf + got;
             p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len)
        {
            const struct inotify_event *ev = (const struct inotify_event *)p;

            if (ev->wd == settings_wd)
            {
                if (ev->len && !strcmp(ev->name, NEXTUI_SETTINGS))
                    post_event(SYS_NEXTUI_SETTINGS_SAVED, 0);
            }
            else if (ev->len)
                add_node(ev->name);
        }
}

/* SDL doesn't report switches.  Headphones pulled from the jack while the
 * codec plays pause playback, as a lost USB or Bluetooth output does; going in,
 * the codec moves the sound to them by itself. */
static void input_switch(const struct input_event *ev)
{
    if (ev->type == EV_SW && ev->code == SW_HEADPHONE_INSERT && !ev->value &&
        trimpod_audio_sink() == TRIMPOD_SINK_SPEAKER)
        post_event(SYS_PHONE_UNPLUGGED, 0);
}

static void node_event(int slot, const struct input_event *ev)
{
    if (!input_headset[slot])
        input_switch(ev);
    else if (ev->type == EV_KEY && ev->value <= 1)  /* not autorepeat */
    {
        sim_enter_irq_handler();
        media_key(ev->code, ev->value);
        sim_exit_irq_handler();
    }
}

/* While a tap sequence is open, wake when it closes. */
static int poll_timeout(void)
{
    int left = (int)(tap.commit_at - SDL_GetTicks());

    return !tap.pending ? 1000 : left > 0 ? left : 0;
}

/* Sleep until input arrives and drain our copies; SDL reads its own when the
 * caller pumps.  The 1s timeout pumps what SDL queues without input (SIGTERM). */
static void wait_for_input(void)
{
    if (poll(input_fds, num_input_fds, poll_timeout()) > 0)
        for (int i = 0; i < num_input_fds; i++)
        {
            struct pollfd *p = &input_fds[i];

            if (p->fd < 0 || !p->revents)
                continue;
            if (i == WATCH_SLOT)
                read_watch();
            else if (p->revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                if (input_headset[i])
                {
                    sim_enter_irq_handler();
                    headset_gone();
                    sim_exit_irq_handler();
                }
                input_headset[i] = false;
                close(p->fd);           /* device gone: stop polling it */
                p->fd = -1;
            }
            else
            {
                struct input_event evs[16];
                ssize_t got;

                while ((got = read(p->fd, evs, sizeof evs)) > 0)
                    if (i >= NODE_SLOT)         /* not the wake pipe */
                        for (int e = 0; e < got / (ssize_t)sizeof evs[0]; e++)
                            node_event(i, &evs[e]);
            }
        }
    if (tap.pending && SDL_TICKS_PASSED(SDL_GetTicks(), tap.commit_at))
    {
        sim_enter_irq_handler();
        commit_taps();
        sim_exit_irq_handler();
    }
}

#endif /* RETRO_HANDHELD */

void gui_message_loop_wake(void)
{
#ifdef RETRO_HANDHELD
    if (wake_pipe[1] >= 0)
    {
        ssize_t n = write(wake_pipe[1], "", 1);
        (void)n;
    }
#endif
}

static bool dispatch_event(SDL_Event *event)
{
    sim_enter_irq_handler();
    int was = btn;
    bool quit = event_handler(event);
    released |= was & ~btn;
    sim_exit_irq_handler();
    return quit;
}

void gui_message_loop(void)
{
    SDL_Event event;
    bool quit = false;

    /* Open the gamepad here, on the same thread that pumps SDL events. */
    open_joysticks();

#ifdef RETRO_HANDHELD
    open_input_fds();
    if (num_input_fds > NODE_SLOT)
    {
        while (!quit)
        {
            while (!quit && SDL_PollEvent(&event))
                quit = dispatch_event(&event);
            if (!quit)
                wait_for_input();
        }
    }
#endif

    while (!quit)
    {
        /* wait for the next event */
        if(SDL_WaitEvent(&event) == 0) {
            printf("SDL_WaitEvent(): %s\n", SDL_GetError());
            break; /* error, out of here */
        }
        quit = dispatch_event(&event);
    }

    close_joysticks();
}


#if defined(HAVE_BUTTON_DATA)
int button_read_device(int* data)
{
    (void) *data;   /* suppress compiler warnings */
#else
int button_read_device(void)
{
#endif
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
#ifdef HAVE_TRIMUI_SAFE_POWEROFF
                trimui_safe_poweroff();     /* the Brick, not just the app */
#endif
                sys_poweroff();
            }
        }
        else if (power_down_tick)           /* released */
        {
            if (current_tick - power_down_tick < POWER_LONG_PRESS_TICKS)
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
     * dim/flash the screen on every toggle.  The volume rocker stays live, so
     * volume works with the switch on (in a pocket). */
    hold_button_state = retrohh_hold_switch();
    if (hold_button_state)
    {
        /* The side switch reports as EV_SW SW_TABLET_MODE on the gamepad node,
         * which SDL's joystick layer ignores (it's not a button), so nothing
         * leaks from the switch itself.  Still discard any other button held
         * across the lock edge so it can't survive into the locked state. */
        btn &= BUTTON_VOL_MASK;
    }

    /* Every release reads as up for two polls, even if the button is already
     * down again: button_read() drops one-poll changes, and a lost release
     * merges fast taps into one hold that auto-repeats. */
    static int up_mask, up_polls;
    if (released)
    {
        up_mask |= released;
        up_polls = 2;
        released = 0;
    }

    /* Volume rocker arrives as joystick buttons 14/13 (folded into btn by
     * event_handler, like every other gamepad key) -- no separate read here. */
    int result = btn & ~up_mask;
    if (up_polls && --up_polls == 0)
        up_mask = 0;

    /* Any button but power and volume wakes the screen (backlight_on() in
     * button_tick), so a manual power-blank is over -- the next power tap
     * blanks again rather than being treated as a wake. */
    if (result & ~BUTTON_VOL_MASK)
        power_blanked = false;

    return result;
}

void button_init_device(void)
{
}
