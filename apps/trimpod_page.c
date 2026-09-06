/***************************************************************************
 * Trimpod: the page run loop (see trimpod_page.h).
 *
 * One driver for every Trimpod page.  It owns the things every page shares so
 * no page re-implements them:
 *   - publishes the page's button-legend to the header (and restores the
 *     previous one on exit, so nested pages stack cleanly),
 *   - refreshes the skin header so the legend actually appears,
 *   - draws the page content,
 *   - enforces the page's key whitelist (non-allowed buttons do nothing),
 *   - lets system events (USB, poweroff) through to default_event_handler.
 ****************************************************************************/
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "action.h"
#include "misc.h"
#include "screens.h"          /* FOR_NB_SCREENS */
#include "gui/viewport.h"     /* viewportmanager_theme_enable / _undo */
#include "statusbar-skinned.h" /* sb_get_title / sb_set_title_text (%Lt) */
#include "trimpod_ui.h"
#include "trimpod_page.h"
#include "trimpod_transition.h"
#include "trimpod_visualizer.h"   /* idle auto-start while music plays */
#include "audio.h"                /* audio_status: idle return to Now Playing */
#include "backlight.h"            /* is_backlight_on */
#include "button.h"               /* button_last_activity_tick, power_display_off */
#include "settings.h"

bool trimpod_home_pending;   /* see trimpod_page.h: hold-BACK unwinds to root */
bool trimpod_wps_pending;    /* ... and lands on Now Playing */

#define TRIMPOD_IDLE_TO_WPS 10   /* seconds idle in the menus before Now Playing returns */

bool trimpod_idle_to_wps(void)
{
    int status = audio_status();
    int screen_off = global_settings.backlight_timeout;   /* seconds, 0 = Never */
    if (!(status & AUDIO_STATUS_PLAY) || (status & AUDIO_STATUS_PAUSE)
        /* Auto Screen Off at or under our delay wins the race: stay put */
        || (screen_off > 0 && screen_off <= TRIMPOD_IDLE_TO_WPS)
        || power_display_off() || !is_backlight_on(true)
        || !TIME_AFTER(current_tick, button_last_activity_tick()
                                     + TRIMPOD_IDLE_TO_WPS * HZ))
        return false;
    trimpod_home_pending = trimpod_wps_pending = true;
    return true;
}

static bool action_allowed(const int *allowed, int action)
{
    if (!allowed)
        return true;                 /* no whitelist: everything is allowed */
    for (int i = 0; allowed[i] != -1; i++)
        if (allowed[i] == action)
            return true;
    return false;
}

/* draw the page: its header legend + content (no on-screen present is forced
 * here; the caller suppresses updates around this for transitions) */
static void page_render(struct trimpod_page *page)
{
    trimpod_set_header_legend(page->vt->legend ? page->vt->legend(page) : NULL);
    if (!page->no_header_refresh)
        trimpod_header_refresh();
    if (page->vt->draw)
        page->vt->draw(page);
}

/* render callback for the (shared) transition animator */
static void page_render_cb(void *ctx)
{
    page_render((struct trimpod_page *)ctx);
}

void trimpod_page_run(struct trimpod_page *page)
{
    const char *prev = trimpod_get_header_legend();
    const char *prev_title[NB_SCREENS] = { NULL };
    enum themable_icons prev_icon[NB_SCREENS] = { Icon_NOICON };

    /* Publish the page's own header title, remembering the one underneath (the
     * parent list's) so it is back in place before the parent redraws.  The
     * stored icon is biased by +2 for the skin engine -- undo that on the way
     * out and it round-trips. */
    if (page->title)
        FOR_NB_SCREENS(i)
        {
            prev_title[i] = sb_get_title(i);
            prev_icon[i]  = (enum themable_icons)(sb_get_icon(i) - 2);
            sb_set_title_text(page->title, Icon_NOICON, i);
        }

    /* swallow the keypress that opened this page */
    action_wait_for_release();

    /* A full-screen page disables the theme (SBS status bar) for its lifetime:
     * the bar is then neither drawn nor periodically repainted over the page,
     * and the slide treats the whole screen as content (header_h()==0) so the
     * enter/exit transitions clear it fully.  Restored before we return, so the
     * parent's back-slide pins its status bar again. */
    if (page->no_theme)
        FOR_NB_SCREENS(i)
            viewportmanager_theme_enable(i, false, NULL);

    /* Slide this page in.  A nested page always enters forward; a re-entrant
     * page (the root menu, driven by an external dispatch loop) honors a
     * pending back so returning from a child slides it back in.  The first
     * screen at app launch renders with no slide. */
    if (page->no_enter_anim)
    {
        trimpod_transition_take_back();   /* consume any stale pending back */
        page_render(page);
    }
    else
    {
        /* animate honors a pending suppress (splash-only abort) by presenting
         * with no motion; otherwise it slides in the chosen direction. */
        enum trimpod_transition_dir enter_dir =
            (page->enter_honor_back && trimpod_transition_take_back())
                ? TRIMPOD_TRANS_BACK : TRIMPOD_TRANS_FORWARD;
        trimpod_transition_animate(enter_dir, page_render_cb, page);
    }

    bool done = false, shutting_down = false;
    while (!done)
    {
        /* A nested page/menu requested home while we were inside it: keep
         * unwinding so the dispatcher lands on the Main Menu. */
        if (trimpod_home_pending)
        {
            page->home = true;
            break;
        }

        int action = page->vt->poll ? page->vt->poll(page, HZ)
                                    : get_action(page->context, HZ);

        /* Hold BACK anywhere = home (the hold is timed globally in
         * global_home_action).  Flag it so every enclosing loop unwinds too,
         * then leave; the dispatcher slides the Main Menu back in one step. */
        if (action == ACTION_TP_HOME)
        {
            trimpod_home_pending = true;
            page->home = true;
            break;
        }

        /* key whitelist: swallow non-allowed buttons; SYS events still pass */
        if (!(action & SYS_EVENT) && !action_allowed(page->allowed, action))
            action = ACTION_NONE;

        if (page->vt->on_action &&
            page->vt->on_action(page, action) == TRIMPOD_PAGE_DONE)
            done = true;
        else
        {
            int handled = default_event_handler(action);
            if (handled == SYS_USB_CONNECTED)
                done = true;
            else if (handled == SYS_POWEROFF || handled == SYS_REBOOT)
                shutting_down = true;   /* exit is deferred; stop drawing */
        }

        if (done)
            break;

        /* Home unwind in progress: don't play this level's back-slide -- loop
         * straight back to the top, which exits.  Only the final landing at the
         * Main Menu animates, so it's one slide from here to home, not a cascade
         * of slides back through every intermediate screen. */
        if (trimpod_home_pending)
        {
            /* idle return that started in a screen nested in the WPS: land
             * here with one back slide */
            if (trimpod_wps_pending && page->context == CONTEXT_WPS)
            {
                trimpod_home_pending = trimpod_wps_pending = false;
                trimpod_transition_arm_back();
            }
            else
                continue;
        }

        /* Once shutdown has begun, never redraw: clean_shutdown drew "Shutting
         * Down" and the real exit is deferred (an SDL event), so a page redraw
         * would clobber it.  Keep looping so get_action keeps pumping events
         * until the exit fires.  (do_menu likewise only redraws on changes.) */
        if (shutting_down)
            continue;

        /* Idle auto-start of the visualizer while music plays.  The WPS page
         * handles this inside its own on_action (skin restore); its run stamps
         * button activity, so it can't double-fire here. */
        if (action == ACTION_NONE && trimpod_visualizer_maybe_autostart())
        {
            page_render(page);
            continue;
        }
        /* idle return to Now Playing: never from the WPS itself, nor from
         * the keyboard (a half-typed name is not a menu left idle) */
        if (action == ACTION_NONE && page->context != CONTEXT_WPS
            && page->context != CONTEXT_KEYBOARD && trimpod_idle_to_wps())
            continue;

        if (trimpod_transition_take_back())
            /* a nested page just exited: slide it out, us back in (L->R) */
            trimpod_transition_animate(TRIMPOD_TRANS_BACK, page_render_cb, page);
        else if (page->animated)   /* static pages redraw on change, not per tick */
            page_render(page);
    }

    /* re-enable the theme a full-screen page disabled, before the parent's
     * back-slide runs (it needs its status bar pinned again). */
    if (page->no_theme)
        FOR_NB_SCREENS(i)
            viewportmanager_theme_undo(i, false);

    /* put the parent's title back */
    if (page->title)
        FOR_NB_SCREENS(i)
            sb_set_title_text(prev_title[i], prev_icon[i], i);

    /* restore the header the parent page had, then (unless told not to) arm a
     * back slide so the screen we return to slides us away and itself back in.
     * A re-entrant page suppresses this: it isn't returning to a parent -- the
     * next screen the dispatch loads will slide itself in. */
    trimpod_set_header_legend(prev);
    if (!page->no_arm_back)
        trimpod_transition_arm_back();
}
