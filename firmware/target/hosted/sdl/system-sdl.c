/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 by Daniel Everton <dan@iocaine.org>
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

#include <SDL.h>
#include <SDL_thread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#ifdef __unix__
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <ucontext.h>
#include <sys/syscall.h>
#endif
#include "system.h"
#include "kernel.h"
#include "thread-sdl.h"
#include "system-sdl.h"
#include "sim-ui-defines.h"
#if SDL_MAJOR_VERSION > 1
#include "window-sdl.h"
#endif
#include "button-sdl.h"
#include "lcd-bitmap.h"
#include "panic.h"
#include "debug.h"


#define SIMULATOR_DEFAULT_ROOT "simdisk"

/* Trimpod: millisecond wall clock for smooth, present-paced animation timing
 * (the 10ms tick granularity is too coarse for a 0.3s tween). */
unsigned long sdl_get_ms(void)
{
    return SDL_GetTicks();
}

#if SDL_MAJOR_VERSION == 1
SDL_Surface *gui_surface;
#endif

bool            background = true;          /* use backgrounds by default */
bool            mapping = false;
const char      *audiodev = NULL;
bool            debug_buttons = false;

bool            sim_alarm_wakeup = false;
const char     *sim_root_dir = SIMULATOR_DEFAULT_ROOT;

static SDL_Thread *evt_thread = NULL;


bool debug_wps = false;
int wps_verbose_level = 3;

#ifndef __APPLE__ /* MacOS requires events to be handled on main thread */
/*
 * This thread will read the buttons in an interrupt like fashion, and
 * also initializes SDL_INIT_VIDEO and the surfaces
 *
 * it must be done in the same thread (at least on windows) because events only
 * work in the thread that called SDL_InitSubSystem(SDL_INIT_VIDEO)
 *
 * This is an SDL thread and relies on preemptive behavoir of the host
 **/
static int sdl_event_thread(void * param)
{
#ifdef __WIN32 /* Fails on Linux and MacOS */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    sdl_window_setup();
#endif

    /* Explicitly disable the cursor on non-touch targets */
    SDL_ShowCursor(SDL_DISABLE);

#if SDL_MAJOR_VERSION == 1
    SDL_InitSubSystem(SDL_INIT_VIDEO);

    SDL_Surface *picture_surface = NULL;
    int depth;
    Uint32 flags;

    depth = LCD_DEPTH;
    if (depth < 8)
        depth = 16;

    flags = SDL_HWSURFACE|SDL_DOUBLEBUF|SDL_FULLSCREEN;

    if ((gui_surface = SDL_SetVideoMode(LCD_WIDTH, LCD_HEIGHT, depth, flags)) == NULL) {
        panicf("%s", SDL_GetError());
    }

    if (background && picture_surface != NULL)
        SDL_BlitSurface(picture_surface, NULL, gui_surface, NULL);
#endif

    /* let system_init proceed */
    SDL_SemPost((SDL_sem *)param);

    /* finally enter the button loop */
    gui_message_loop();

    return 0;
}
#endif

static bool quitting;

void sdl_sys_quit(void)
{
    quitting = true;
    sys_poweroff();
}

void power_off(void)
{
    /* Shut down SDL event loop */
    SDL_Event event;
    memset(&event, 0, sizeof(SDL_Event));
    event.type = SDL_USEREVENT;
    SDL_PushEvent(&event);
    gui_message_loop_wake();
    /* wait for event thread to finish */
    SDL_WaitThread(evt_thread, NULL);


    sim_do_exit();
}

void sim_do_exit()
{

    sim_kernel_shutdown();

#if SDL_MAJOR_VERSION > 1
    SDL_UnlockMutex(window_mutex);
    SDL_DestroyMutex(window_mutex);
#endif

    SDL_Quit();
    exit(EXIT_SUCCESS);
}

/* Trimpod: turn a crash into an answer.  stderr is trimpod.log, so a fault
 * leaves a symbolised stack there instead of a bare "Segmentation fault" that
 * can only be guessed at.  Re-raising with the handler reset gives the normal
 * exit status. */
/* Report the faulting PC and the process map rather than a backtrace: unwinding
 * needs tables this binary does not carry and faulted again when tried, which
 * is why the log kept showing nothing.  PC plus maps says which library died,
 * using only reads that cannot themselves fault. */
static void crash_handler(int sig, siginfo_t *info, void *ctx)
{
    unsigned long pc = 0;
    char buf[1024];
    ssize_t n;
    int fd;

#if defined(__aarch64__)
    pc = ((ucontext_t *)ctx)->uc_mcontext.pc;
#elif defined(__arm__)
    pc = ((ucontext_t *)ctx)->uc_mcontext.arm_pc;
#else
    (void)ctx;
#endif

    /* si_code is the one field that says whether this was a real memory fault
     * (SEGV_MAPERR 1 / SEGV_ACCERR 2, si_addr = the bad address) or a signal
     * something sent us (SI_USER 0 / SI_TKILL -6, si_addr aliases si_pid). */
    static volatile sig_atomic_t depth;

    dprintf(STDERR_FILENO,
            "\n*** signal %d (entry #%d)  si_code %d  addr/pid %p  pc 0x%lx  tid %ld ***\n",
            sig, (int)++depth, info->si_code, info->si_addr, pc,
            (long)syscall(SYS_gettid));

    if (depth > 1)              /* the real fault re-firing; maps already dumped */
    {
        signal(sig, SIG_DFL);
        return;
    }

    fd = open("/proc/self/maps", O_RDONLY);
    if (fd >= 0)
    {
        dprintf(STDERR_FILENO, "--- maps ---\n");
        while ((n = read(fd, buf, sizeof buf)) > 0)
            if (write(STDERR_FILENO, buf, n) < 0)
                break;
        close(fd);
        dprintf(STDERR_FILENO, "--- end maps ---\n");
    }

    /* Deliberately no raise().  Re-raising made the log ambiguous: the kernel
     * then reported OUR tgkill (si_code SI_TKILL, pc inside raise) instead of
     * the original fault.  Restoring the default and RETURNING re-executes the
     * faulting instruction, so the kernel logs the true pc/lr/sp -- and if the
     * signal was sent rather than faulted, execution simply carries on, which
     * is itself the answer. */
    signal(sig, SIG_DFL);
}

uintptr_t *stackbegin;
uintptr_t *stackend;
void system_init(void)
{
    SDL_sem *s;
    /* fake stack, OS manages size (and growth) */
    stackbegin = stackend = (uintptr_t*)&s;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);


    if (SDL_InitSubSystem(SDL_INIT_TIMER))
        panicf("%s", SDL_GetError());


#ifndef __WIN32  /* Fails on Windows */
    SDL_InitSubSystem(SDL_INIT_VIDEO);

#if SDL_MAJOR_VERSION > 1
    sdl_window_setup();
#endif
#endif

#ifndef __APPLE__ /* MacOS requires events to be handled on main thread */
    s = SDL_CreateSemaphore(0); /* 0-count so it blocks */

    #if SDL_MAJOR_VERSION > 1
        evt_thread = SDL_CreateThread(sdl_event_thread, NULL, s);
    #else
        evt_thread = SDL_CreateThread(sdl_event_thread, s);
    #endif /* SDL_MAJOR_VERSION */

    SDL_SemWait(s);
    /* cleanup */
    SDL_DestroySemaphore(s);
#else
    SDL_AddEventWatch(sdl_event_filter, NULL);
#endif
}


void system_reboot(void)
{

    sim_do_exit();
}

void system_exception_wait(void)
{
    if (evt_thread)
    {
        while (!quitting)
            SDL_Delay(10);
    }
    system_reboot();
}

int hostfs_init(void)
{
    /* stub */
    return 0;
}

#ifdef HAVE_STORAGE_FLUSH
int hostfs_flush(void)
{
#ifdef __unix__
    sync();
#endif
    return 0;
}
#endif /* HAVE_STORAGE_FLUSH */

void sys_handle_argv(int argc, char *argv[])
{
    if (argc >= 1)
    {
        int x;
        for (x = 1; x < argc; x++)
        {
                if (!strcmp("--debugwps", argv[x]))
            {
                debug_wps = true;
                printf("WPS debug mode enabled.\n");
            }
            else if (!strcmp("--nobackground", argv[x]))
            {
                background = false;
                printf("Disabling background image.\n");
            }
#if SDL_MAJOR_VERSION > 1
            else if (!strcmp("--zoom", argv[x]))
            {
                x++;
                if(x < argc)
                    display_zoom=atof(argv[x]);
                else
                    display_zoom = 2;
                printf("Window zoom is %f\n", display_zoom);
            }
#endif
            else if (!strcmp("--alarm", argv[x]))
            {
                sim_alarm_wakeup = true;
                printf("Simulating alarm wakeup.\n");
            }
            else if (!strcmp("--root", argv[x]))
            {
                x++;
                if (x < argc)
                {
                    sim_root_dir = argv[x];
                    printf("Root directory: %s\n", sim_root_dir);
                }
            }
            else if (!strcmp("--mapping", argv[x]))
            {
                    mapping = true;
                    printf("Printing click coords with drag radii.\n");
            }
            else if (!strcmp("--debugbuttons", argv[x]))
            {
                    debug_buttons = true;
                    printf("Printing background button clicks.\n");
            }
            else if (!strcmp("--audiodev", argv[x]))
            {
                x++;
                if (x < argc)
                {
                    audiodev = argv[x];
                    printf("Audio device: '%s'\n", audiodev);
                }
            }
            else
            {
                printf("rockboxui\n");
                printf("Arguments:\n");
                printf("  --debugwps \t Print advanced WPS debug info\n");
                printf("  --nobackground \t Disable the background image\n");
                printf("  --zoom [VAL]\t Window zoom (will disable backgrounds)\n");
                printf("  --alarm \t Simulate a wake-up on alarm\n");
                printf("  --root [DIR]\t Set root directory\n");
                printf("  --mapping \t Output coordinates and radius for mapping backgrounds\n");
                printf("  --audiodev [NAME] \t Audio device name to use\n");
                exit(0);
            }
        }
    }
#if SDL_MAJOR_VERSION > 1
    if (display_zoom != 1) {
        background = false;
    }
#endif
}
