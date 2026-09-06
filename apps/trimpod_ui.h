/* Trimpod: small reusable UI helpers shared across Trimpod screens. */
#ifndef _TRIMPOD_UI_H
#define _TRIMPOD_UI_H

#include <stdbool.h>

/* ---- Header button-legend (the standard) -----------------------------------
 * A "page" declares the button legend it wants shown in the status-row header
 * (e.g. "B Cancel   A OK").  The SBS skin reads it through the %Tl token and
 * overlays it on the header; pages never draw the header themselves.  Set to
 * NULL to clear.  The string must outlive the period it is set -- pass string
 * literals. */
void        trimpod_set_header_legend(const char *legend);
const char *trimpod_get_header_legend(void);

/* Re-render the status row so a just-set legend appears/clears.  Pages that run
 * their own draw loop need this; the run loop (trimpod_page_run) calls it. */
void        trimpod_header_refresh(void);

/* Draw `detail` (optional) centered above `question`, and `footer` (optional)
 * centered below it, inside the content viewport -- the themed background and
 * skin-owned header are left intact.  Pass NULL to omit detail/footer.  The
 * shared renderer behind trimpod_confirm; a page with its own draw loop (e.g.
 * the HOLD screen) can call it directly. */
void trimpod_centered_message(const char *question, const char *detail,
                              const char *footer);

/* One line centered on the full screen (themed background, no header, no box),
 * presented immediately -- for blocking work that has no page.  `widest`
 * (NULL = msg) is msg's longest form; centering on it keeps the line from
 * shifting as msg changes. */
void trimpod_fullscreen_message(const char *msg, const char *widest);

/* Centered yes/no confirmation drawn inside the content viewport (a Page, not a
 * popup).  The button choices "Cancel (B) / OK (A)" are drawn in the page body.
 *   question : the prompt, e.g. "Remove this folder?"
 *   detail   : optional line drawn just above the question (NULL to omit) --
 *              used to show the subject, e.g. the full folder path.
 *   A = OK -> true, B = Cancel -> false.  Blocking. */
bool trimpod_confirm(const char *question, const char *detail);

/* Context submenu (opened by holding A on a context-aware entry): a small
 * sliding list Page of `count` options titled `title`.  Items are lang ids
 * (ID2P) or strings.  Returns the chosen row index, or -1 if the user backed
 * out with B.  Blocking. */
int trimpod_context_menu(const char *title, const char *const *items, int count);

/* The About screen: a credits reel that drifts up and down on its own (B leaves).
 * Blocking. */
void trimpod_about(void);

/* Pre-fault the About reel's glyphs into the font cache at startup, so the first
 * (possibly mid-playback) About open doesn't stall the codec on disk reads. */
void trimpod_about_prewarm(void);

/* A static text page: `rows` (lang ids via ID2P, or strings) rendered as a
 * centred block under a `title` header (B leaves).  Blocking. */
void trimpod_message_page(const char *title, const char *const *rows, int nrows);

/* The Controls screen: a static list of the app's inputs (B leaves).  Blocking. */
void trimpod_controls(void);

/* The shared on/off toggle glyph ("[x]"/"[ ]") for list indicator callbacks.
 * One convention for every toggle list (Menu Settings, Visualizers, ...). */
const char *trimpod_toggle_str(bool on);

#endif /* _TRIMPOD_UI_H */
