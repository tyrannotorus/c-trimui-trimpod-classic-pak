/***************************************************************************
 * Trimpod: the HOLD (pocket-lock) screen -- see trimpod_hold.h.
 *
 * An ordinary trimpod_page, drawn exactly like the "(empty)" / confirm pages:
 * the themed content viewport (trimpod_centered_message) with the skin-owned
 * status-row header, no bespoke theme and no enter slide.  Because input is
 * muted while held, the page can't rely on a button to exit -- its on_action
 * polls the switch directly and returns DONE once it reads "off".
 ****************************************************************************/
#include "config.h"
#include "trimpod_hold.h"


#include <stdbool.h>
#include "action.h"          /* get_action(), CONTEXT_STD */
#include "kernel.h"          /* HZ */
#include "button-target.h"   /* retrohh_hold_switch() */
#include "trimpod_page.h"
#include "trimpod_ui.h"      /* trimpod_centered_message() */
#include "lang.h"            /* str() */

static void hold_draw(struct trimpod_page *self)
{
    (void)self;
    trimpod_centered_message(str(LANG_TRIMPOD_HOLD_HINT), str(LANG_TRIMPOD_HOLD_ON), NULL);
}

/* Input is muted while held, so poll the switch quickly (not the HZ the loop
 * suggests) so we notice the release promptly; SYS events still flow through. */
static int hold_poll(struct trimpod_page *self, int timeout)
{
    (void)timeout;
    return get_action(self->context, HZ / 10);
}

static enum trimpod_page_result hold_on_action(struct trimpod_page *self, int action)
{
    (void)self;
    (void)action;
    return retrohh_hold_switch() ? TRIMPOD_PAGE_STAY : TRIMPOD_PAGE_DONE;
}

static const struct trimpod_page_vtable hold_vtable =
{
    .legend    = NULL,        /* nothing is actionable while held */
    .draw      = hold_draw,
    .poll      = hold_poll,
    .on_action = hold_on_action,
};

void trimpod_hold_run(void)
{
    if (!retrohh_hold_switch())
        return;                 /* switch off at launch -> straight to the menu */

    struct trimpod_page p =
    {
        .vt            = &hold_vtable,
        .context       = CONTEXT_STD,
        .allowed       = NULL,
        .no_enter_anim = true,   /* first screen at launch: no slide */
        .no_arm_back   = true,   /* the main menu draws itself, no back slide */
    };
    trimpod_page_run(&p);
}

