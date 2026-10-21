/***************************************************************************
 * Trimpod: the HOLD (pocket-lock) screen.
 *
 * The Brick's side switch mutes all input but the volume rocker while engaged,
 * which to a new user looks like a frozen app --
 * especially when Trimpod is launched with the switch ALREADY on.  So at launch
 * we show a "HOLD is ON" page instead of the main menu, and close it (-> main
 * menu) the moment the switch is released.
 *
 * It is an ordinary trimpod_page (see trimpod_page.h): the shared run loop owns
 * the enter transition, drawing, polling and exit, so it presents and behaves
 * exactly like every other screen -- no bespoke loop.
 ****************************************************************************/
#ifndef TRIMPOD_HOLD_H
#define TRIMPOD_HOLD_H

/* If the HOLD switch is engaged, run the lock page until it is released; a no-op
 * (returns immediately) when the switch is off.  Call before the main menu. */
void trimpod_hold_run(void);

#endif /* TRIMPOD_HOLD_H */
