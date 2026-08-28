/***************************************************************************
 * Trimpod: reusable UI helpers.
 *
 * Header button-legend standard: a page sets the legend it wants shown in the
 * status-row header via trimpod_set_header_legend(); the SBS skin reads it with
 * the %Tl token and overlays it.  Pages draw ONLY inside the content viewport
 * and never touch the skin-owned header.
 *
 * trimpod_confirm() is a centered yes/no page (no box/border) drawn inside the
 * content viewport.  Its B/A legend lives in the header (not the body); an
 * optional `detail` line (e.g. the full path being acted on) sits above the
 * question.
 ****************************************************************************/
#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "string-extra.h"       /* strlcpy (context-menu title copy) */
#include "file.h"               /* MAX_PATH */
#include <stdio.h>
#include "kernel.h"
#include "system.h"
#include "action.h"
#include "lang.h"                /* str(): the page titles are localised */
#include "screens.h"
#include "lcd.h"
#include "font.h"
#include "rbpaths.h"             /* FONT_DIR */
#include "settings.h"            /* global_settings: theme fg/bg colours */
#include "gui/viewport.h"
#include "gui/list.h"
#include "icon.h"
#include "statusbar-skinned.h"
#include "misc.h"
#include "trimpod_ui.h"
#include "trimpod_page.h"

/* ---- header legend standard ------------------------------------------- */

static const char *s_header_legend;   /* NULL = no legend */

bool trimpod_russian_ui(void)
{
    return !strcmp((const char *)global_settings.lang_file, "russian");
}

const char *trimpod_resolve_ui_font(const char *name, char *resolved,
                                    size_t resolved_size)
{
    static const char marker[] = "-TrimpodUI";
    const char *mark = strstr(name, marker);
    if (!mark)
        return name;

    size_t prefix_len = (size_t)(mark - name);
    const char *tail = mark + sizeof(marker) - 1;
    bool supported_size =
        (prefix_len == 2 &&
         ((name[0] == '1' && name[1] == '8') ||
          (name[0] == '2' && (name[1] == '0' || name[1] == '4'))));
    if (!supported_size || (*tail && strcmp(tail, ".fnt")))
        return name;

    int written = snprintf(resolved, resolved_size, "%.*s-%s%s",
                           (int)prefix_len, name,
                           trimpod_russian_ui() ? "TrimpodRus" : "ChicagoFLF",
                           tail);
    return (written >= 0 && (size_t)written < resolved_size) ? resolved : name;
}

void trimpod_set_header_legend(const char *legend)
{
    s_header_legend = legend;
}

const char *trimpod_get_header_legend(void)
{
    return s_header_legend;
}

/* The one place the on/off toggle glyph convention lives.  Returned as a list
 * indicator string (drawn right-aligned by the shared list renderer); used by
 * every toggle list -- Settings -> Menu Settings, Settings -> Visualizers, ... */
const char *trimpod_toggle_str(bool on)
{
    return on ? "[x]" : "[ ]";
}

/* Trimpod string tables deliberately use const char * so callers can mix
 * literal UTF-8 text with Rockbox's ID2P() virtual language pointers.  P2STR
 * predates const-correct tables and performs pointer arithmetic on unsigned
 * char *, so normalize the type at this single boundary. */
static const char *trimpod_resolve_string(const char *value)
{
    return P2STR((const unsigned char *)value);
}

/* Re-render the status row so a just-changed legend appears/clears.  Needed
 * because a page runs its own draw loop and does not otherwise drive the skin
 * engine the way the file browser does.  Public: the run loop calls it. */
void trimpod_header_refresh(void)
{
    FOR_NB_SCREENS(i)
        sb_skin_update((enum screen_type)i, true);
}

/* ---- confirm page ----------------------------------------------------- */

/* Draw `detail` (optional) above `question` and `footer` (optional) below it,
 * centered as a block in the content viewport.  The header is left to the skin. */
void trimpod_centered_message(const char *question, const char *detail,
                              const char *footer)
{
    FOR_NB_SCREENS(i)
    {
        struct screen *s = &screens[i];
        struct viewport vp = {0};
        int dw = 0, dh = 0, qw = 0, qh = 0, fw = 0, fh = 0;

        viewport_set_defaults(&vp, s->screen_type);  /* content area, not header */
        s->set_viewport(&vp);
        s->clear_viewport();

        s->getstringsize((const unsigned char *)question, &qw, &qh);
        if (detail)
            s->getstringsize((const unsigned char *)detail, &dw, &dh);
        if (footer)
            s->getstringsize((const unsigned char *)footer, &fw, &fh);

        int gap = qh / 2 + 2;                     /* a little air between lines */
        int total = (detail ? dh + gap : 0) + qh + (footer ? gap + fh : 0);
        int y = (vp.height - total) / 2;
        if (y < 0)
            y = 0;

        if (detail)
        {
            s->putsxy((vp.width - dw) / 2, y, (const unsigned char *)detail);
            y += dh + gap;
        }
        s->putsxy((vp.width - qw) / 2, y, (const unsigned char *)question);
        y += qh;
        if (footer)
        {
            y += gap;
            s->putsxy((vp.width - fw) / 2, y, (const unsigned char *)footer);
        }

        s->update_viewport();
        s->set_viewport(NULL);
    }
}

/* The confirm page, derived from trimpod_page.  The run loop owns the legend,
 * header refresh, key whitelist and input loop; this page only describes its
 * legend, how to draw, and how to react to A/B. */
struct confirm_page
{
    struct trimpod_page base;
    const char *question;
    const char *detail;
    bool result;
};

static void confirm_page_draw(struct trimpod_page *self)
{
    struct confirm_page *p = (struct confirm_page *)self;
    /* the choices live in the page body, not a header legend */
    trimpod_centered_message(p->question, p->detail,
                             str(LANG_TRIMPOD_CONFIRM_BUTTONS));
}

static enum trimpod_page_result confirm_on_action(struct trimpod_page *self,
                                                  int action)
{
    struct confirm_page *p = (struct confirm_page *)self;
    if (action == ACTION_STD_OK)     { p->result = true;  return TRIMPOD_PAGE_DONE; }
    if (action == ACTION_STD_CANCEL) { p->result = false; return TRIMPOD_PAGE_DONE; }
    return TRIMPOD_PAGE_STAY;
}

static const struct trimpod_page_vtable confirm_vtable =
{
    .legend    = NULL,                 /* choices are drawn in the page body */
    .draw      = confirm_page_draw,
    .poll      = NULL,                 /* default: get_action(CONTEXT_STD) */
    .on_action = confirm_on_action,
};

/* only A (OK) and B (Cancel) act on this page */
static const int confirm_allowed[] = { ACTION_STD_OK, ACTION_STD_CANCEL, -1 };

bool trimpod_confirm(const char *question, const char *detail)
{
    struct confirm_page p =
    {
        .base = { .vt = &confirm_vtable, .context = CONTEXT_STD,
                  .allowed = confirm_allowed },
        .question = question,
        .detail   = detail,
        .result   = false,
    };
    trimpod_page_run(&p.base);
    return p.result;
}

/* ---- modal context-menu frame ----------------------------------------- */

#define TP_POPUP_WIDTH       416
#define TP_POPUP_BORDER        2
#define TP_POPUP_TITLE_GAP     6
#define TP_POPUP_MAX_ROWS      8

/* One modal can be open at a time. Keeping the snapshot off the stack avoids
 * a ~576 KiB stack allocation at the logical 512x384 RGB framebuffer size. */
static fb_data popup_background[LCD_FBWIDTH * LCD_FBHEIGHT];
static bool popup_active;
static struct viewport popup_title_vp[NB_SCREENS];

void trimpod_popup_begin(struct viewport parent[NB_SCREENS])
{
    FOR_NB_SCREENS(i)
        screens[i].scroll_stop_viewport(&popup_title_vp[i]);
    lcd_set_viewport(NULL);
    memcpy(popup_background, FBADDR(0, 0), FRAMEBUFFER_SIZE);
    popup_active = true;

    FOR_NB_SCREENS(i)
    {
        viewport_set_defaults(&parent[i], screens[i].screen_type);
        parent[i].font = FONT_UI;
    }
}

void trimpod_popup_layout(struct gui_synclist *lists)
{
    FOR_NB_SCREENS(i)
    {
        struct screen *s = &screens[i];
        struct viewport *vp = lists->parent[i];
        int line_h = font_get(vp->font)->height;
        int rows = lists->nb_items;
        if (rows < 1) rows = 1;
        if (rows > TP_POPUP_MAX_ROWS) rows = TP_POPUP_MAX_ROWS;

        int outer_w = MIN(TP_POPUP_WIDTH, s->lcdwidth - 24);
        int title_h = lists->title ? line_h : 0;
        int outer_h = TP_POPUP_BORDER * 2 + rows * line_h;
        if (title_h)
            outer_h += title_h + TP_POPUP_TITLE_GAP;

        int outer_x = (s->lcdwidth - outer_w) / 2;
        int outer_y = (s->lcdheight - outer_h) / 2;

        vp->x = outer_x + TP_POPUP_BORDER;
        vp->y = outer_y + TP_POPUP_BORDER +
                (title_h ? title_h + TP_POPUP_TITLE_GAP : 0);
        vp->width = outer_w - TP_POPUP_BORDER * 2;
        vp->height = rows * line_h;
        vp->font = FONT_UI;
    }
    gui_synclist_select_item(lists, gui_synclist_get_sel_pos(lists));
}

void trimpod_popup_draw(struct gui_synclist *lists)
{
    if (!popup_active)
        return;

    lcd_set_viewport(NULL);
    memcpy(FBADDR(0, 0), popup_background, FRAMEBUFFER_SIZE);
    trimpod_popup_layout(lists);

    FOR_NB_SCREENS(i)
    {
        struct screen *s = &screens[i];
        struct viewport *vp = lists->parent[i];
        const char *title = lists->title;
        int title_h = title ? font_get(vp->font)->height : 0;
        int outer_x = vp->x - TP_POPUP_BORDER;
        int outer_y = vp->y - TP_POPUP_BORDER -
                      (title_h ? title_h + TP_POPUP_TITLE_GAP : 0);
        int outer_w = vp->width + TP_POPUP_BORDER * 2;
        int outer_h = vp->height + TP_POPUP_BORDER * 2 +
                      (title_h ? title_h + TP_POPUP_TITLE_GAP : 0);
        int old_font = lcd_getfont();
        int old_mode = lcd_get_drawmode();
        unsigned old_fg = s->get_foreground();
        unsigned old_bg = s->get_background();

        s->set_viewport(NULL);
        s->set_drawmode(DRMODE_SOLID);
        s->set_background(global_settings.bg_color);
        s->set_foreground(global_settings.bg_color);
        s->fillrect(outer_x, outer_y, outer_w, outer_h);
        s->set_foreground(global_settings.fg_color);
        s->drawrect(outer_x, outer_y, outer_w, outer_h);
        if (title)
        {
            int tw, th;
            s->setfont(vp->font);
            s->getstringsize((const unsigned char *)title, &tw, &th);
            popup_title_vp[i] = *vp;
            popup_title_vp[i].x = outer_x + TP_POPUP_BORDER + 8;
            popup_title_vp[i].y = outer_y + TP_POPUP_BORDER;
            popup_title_vp[i].width = outer_w - TP_POPUP_BORDER * 2 - 16;
            popup_title_vp[i].height = th;
            popup_title_vp[i].font = vp->font;
            popup_title_vp[i].drawmode = DRMODE_SOLID;
            popup_title_vp[i].fg_pattern = global_settings.fg_color;
            popup_title_vp[i].bg_pattern = global_settings.bg_color;
            s->set_viewport(&popup_title_vp[i]);
            if (tw > popup_title_vp[i].width)
                s->puts_scroll(0, 0, (const unsigned char *)title);
            else
            {
                s->scroll_stop_viewport(&popup_title_vp[i]);
                s->putsxy((popup_title_vp[i].width - tw) / 2, 0,
                          (const unsigned char *)title);
            }
        }
        else
            s->scroll_stop_viewport(&popup_title_vp[i]);

        /* The popup owns its title; suppress the list renderer's normal SBS
         * title routing so the title stays inside the modal frame. */
        lists->title = NULL;
        gui_synclist_draw(lists);
        lists->title = title;

        s->set_viewport(NULL);
        s->setfont(old_font);
        s->set_drawmode(old_mode);
        s->set_foreground(old_fg);
        s->set_background(old_bg);
        s->update();
    }
}

void trimpod_popup_end(void)
{
    if (!popup_active)
        return;
    FOR_NB_SCREENS(i)
        screens[i].scroll_stop_viewport(&popup_title_vp[i]);
    lcd_set_viewport(NULL);
    memcpy(FBADDR(0, 0), popup_background, FRAMEBUFFER_SIZE);
    lcd_update();
    popup_active = false;
}

/* ---- context submenu (MENU) ------------------------------------------- */

/* A compact modal list: MENU opens it, A picks a row and B closes it. */
struct ctxmenu_page
{
    struct trimpod_page base;
    struct gui_synclist lists;
    struct viewport     parent[NB_SCREENS];
    const char *const  *items;
    int                 result;        /* chosen row, or -1 if cancelled */
    char                title[MAX_PATH];  /* owned copy: callers pass volatile
                                           * pointers (e.g. a tree_context entry
                                           * name) that the run can invalidate */
};

static const char *ctxmenu_get_name(int sel, void *data, char *buf, size_t len)
{
    (void)buf; (void)len;
    return trimpod_resolve_string(((struct ctxmenu_page *)data)->items[sel]);
}

static void ctxmenu_draw(struct trimpod_page *self)
{
    trimpod_popup_draw(&((struct ctxmenu_page *)self)->lists);
}

static int ctxmenu_poll(struct trimpod_page *self, int timeout)
{
    int action;
    list_do_action(self->context, timeout,
                   &((struct ctxmenu_page *)self)->lists, &action);
    return action;
}

static enum trimpod_page_result ctxmenu_on_action(struct trimpod_page *self,
                                                  int action)
{
    struct ctxmenu_page *p = (struct ctxmenu_page *)self;
    if (action == ACTION_STD_OK)
    {
        p->result = gui_synclist_get_sel_pos(&p->lists);
        return TRIMPOD_PAGE_DONE;
    }
    if (action == ACTION_STD_CANCEL)
        return TRIMPOD_PAGE_DONE;           /* result stays -1 */
    return TRIMPOD_PAGE_STAY;
}

static const struct trimpod_page_vtable ctxmenu_vtable =
{
    .legend    = NULL,
    .draw      = ctxmenu_draw,
    .poll      = ctxmenu_poll,
    .on_action = ctxmenu_on_action,
};

static const int ctxmenu_allowed[] = { ACTION_STD_OK, ACTION_STD_CANCEL, -1 };

int trimpod_context_menu(const char *title, const char *const *items, int count)
{
    struct ctxmenu_page p =
    {
        .base = { .vt = &ctxmenu_vtable, .context = CONTEXT_LIST,
                  .allowed = ctxmenu_allowed, .no_enter_anim = true,
                  .no_arm_back = true, .no_header_refresh = true },
        .items  = items,
        .result = -1,
    };
    strlcpy(p.title, title ? title : "", sizeof p.title);
    trimpod_popup_begin(p.parent);
    gui_synclist_init(&p.lists, ctxmenu_get_name, &p, false, 1, p.parent);
    gui_synclist_set_title(&p.lists, p.title, Icon_NOICON);
    gui_synclist_set_nb_items(&p.lists, count);
    gui_synclist_select_item(&p.lists, 0);
    trimpod_popup_layout(&p.lists);
    trimpod_page_run(&p.base);
    gui_synclist_scroll_stop(&p.lists);
    trimpod_popup_end();
    return p.result;
}


/* ---- About: an auto-scrolling iPod-style credit reel.  Same chrome as every
 * other page (themed "About" title + slide transition); the body is a centred
 * reel of credits that drifts up and down on its own.  Each credit is a role
 * caption in flanking hairlines, the name, then its URL in the smaller font
 * (long URLs wrap at '/').  B leaves. ----------------------------------- */

#define ABOUT_SCROLL_PXPS 16                          /* drift speed, px/sec */
#define ABOUT_URL_FONT    "18-TrimpodUI.fnt"
#define ABOUT_MAX_RLINES  48                          /* expanded reel capacity */
#define ABOUT_SEG_MAX     48                          /* longest wrapped piece */

static int about_load_url_font(void)
{
    char resolved[MAX_FILENAME + 1];
    char path[MAX_PATH];
    const char *name = trimpod_resolve_ui_font(
        ABOUT_URL_FONT, resolved, sizeof(resolved));
    snprintf(path, sizeof(path), FONT_DIR "/%s", name);
    return font_load(path);
}

enum about_kind { AB_TITLE, AB_SUB, AB_CAP, AB_NAME, AB_URL, AB_GAP };

struct about_line { const char *text; enum about_kind kind; };

static const struct about_line about_lines[] = {
    { "TrimPod(RUS)",        AB_TITLE },
    { "v1.0.7-rus-0.5",      AB_SUB   },
    { NULL,                  AB_GAP   },
    { "FORK BY",             AB_CAP   },
    { "B3L4CQU4",            AB_NAME  },
    { "github.com/B3L4CQU4", AB_URL   },
    { NULL,                  AB_GAP   },
    { "FORK OF",             AB_CAP   },
    { "TrimPod Classic 1.0.7", AB_NAME },
    { "Werewolf Camp",       AB_NAME  },
    { "werewolf.camp",       AB_URL   },
    { NULL,                  AB_GAP   },
    { "BASED ON",            AB_CAP   },
    { "Rockbox",             AB_NAME  },
    { "github.com/Rockbox/rockbox", AB_URL },
    { NULL,                  AB_GAP   },
    { "VISUALS",             AB_CAP   },
    { "projectM",            AB_NAME  },
    { "github.com/projectM-visualizer/projectm", AB_URL },
    { NULL,                  AB_GAP   },
    { "THEME",               AB_CAP   },
    { "1ST_GEN_REMIX",       AB_NAME  },
    { "Monica G.",           AB_NAME  },
    { "themes.rockbox.org/index.php?themeid=3958", AB_URL },
    { NULL,                  AB_GAP   },
    { "PRESETS",             AB_CAP   },
    { "Cream of the Crop",   AB_NAME  },
    { "github.com/projectM-visualizer/presets-cream-of-the-crop", AB_URL },
    { NULL,                  AB_GAP   },
};
#define ABOUT_NLINES ((int)(sizeof(about_lines) / sizeof(about_lines[0])))

/* The reel after URL wrapping: a flat list of physical lines to draw. */
struct about_rline { char text[ABOUT_SEG_MAX]; enum about_kind kind; };

struct about_page
{
    struct trimpod_page base;
    long start_tick;     /* when the reel began -- drives the scroll clock */
    bool header_done;    /* draw the themed header once, then leave it static */
    int  url_fid;        /* loaded compact font for URLs (FONT_UI on failure) */
    int  nr;             /* number of expanded render lines */
    struct about_rline r[ABOUT_MAX_RLINES];
};

/* Greedy-wrap `url` at '/' boundaries so each piece fits `maxw` in the
 * currently-set font; append the pieces as AB_URL render lines. */
static int about_wrap_url(struct screen *s, struct about_rline *out, int n,
                          const char *url, int maxw)
{
    int len = (int)strlen(url), start = 0;
    while (start < len && n < ABOUT_MAX_RLINES)
    {
        int end = start, best = 0;
        char buf[ABOUT_SEG_MAX];
        while (end < len && end - start < ABOUT_SEG_MAX - 1)
        {
            int w, h, seg = end - start + 1;
            memcpy(buf, url + start, seg);
            buf[seg] = '\0';
            s->getstringsize((const unsigned char *)buf, &w, &h);
            if (w > maxw)
                break;
            end++;
            if (url[end - 1] == '/')
                best = end;              /* a clean break point that still fit */
        }
        if (end == start)
            end = start + 1;             /* never stall: take at least one char */
        else if (end < len && best > start)
            end = best;                  /* prefer to break right after a '/' */

        int seg = end - start;
        memcpy(out[n].text, url + start, seg);
        out[n].text[seg] = '\0';
        out[n].kind = AB_URL;
        n++;
        start = end;
    }
    return n;
}

/* Expand the source reel into physical lines once (URLs wrapped to `maxw`). */
static void about_build(struct about_page *p, struct screen *s, int maxw)
{
    int n = 0;
    s->setfont(p->url_fid);              /* URLs measured in the small font */
    for (int i = 0; i < ABOUT_NLINES && n < ABOUT_MAX_RLINES; i++)
    {
        const struct about_line *l = &about_lines[i];
        if (l->kind == AB_URL)
            n = about_wrap_url(s, p->r, n, l->text, maxw);
        else
        {
            snprintf(p->r[n].text, ABOUT_SEG_MAX, "%s", l->text ? l->text : "");
            p->r[n].kind = l->kind;
            n++;
        }
    }
    s->setfont(FONT_UI);
    p->nr = n;
}

static int about_kind_h(enum about_kind k, int ui_h, int url_h, int gap_h)
{
    if (k == AB_GAP) return gap_h;
    if (k == AB_URL) return url_h;
    return ui_h;
}

static void about_draw(struct trimpod_page *self)
{
    struct about_page *p = (struct about_page *)self;
    struct screen *s = &screens[SCREEN_MAIN];
    struct viewport vp = {0};
    const unsigned fg = global_settings.fg_color;
    const unsigned bg = global_settings.bg_color;
    const int margin = 10;

    /* draw the themed header once; it then persists in the framebuffer while
     * the reel below repaints (the no_header_refresh animated-page pattern). */
    if (!p->header_done) { trimpod_header_refresh(); p->header_done = true; }

    viewport_set_defaults(&vp, s->screen_type);   /* content area, not header */
    s->set_viewport(&vp);
    s->set_background(bg);
    s->clear_viewport();

    int aw, ui_fh, url_fh;
    s->setfont(FONT_UI);
    s->getstringsize((const unsigned char *)"Ag", &aw, &ui_fh);
    s->setfont(p->url_fid);
    s->getstringsize((const unsigned char *)"Ag", &aw, &url_fh);

    const int ui_line  = ui_fh + 4;
    const int url_line = url_fh + 2;
    const int gap_h    = ui_fh / 2;
    const int W = vp.width, H = vp.height;
    const int pad = H / 3;        /* top/bottom air so first/last lines breathe */

    int total = 2 * pad;
    for (int i = 0; i < p->nr; i++)
        total += about_kind_h(p->r[i].kind, ui_line, url_line, gap_h);

    int range = total - H;
    if (range < 0) range = 0;

    /* ping-pong offset from the wall clock: a triangle wave 0 -> range -> 0 */
    int off = 0;
    if (range)
    {
        long travelled = ((current_tick - p->start_tick) * ABOUT_SCROLL_PXPS) / HZ;
        long period = 2L * range;
        long pos = travelled % period;
        off = (pos <= range) ? (int)pos : (int)(period - pos);
    }

    int y = pad - off;
    for (int i = 0; i < p->nr; i++)
    {
        const struct about_rline *l = &p->r[i];
        const int lh = about_kind_h(l->kind, ui_line, url_line, gap_h);

        if (l->kind != AB_GAP && y + lh > 0 && y < H)   /* visible band only */
        {
            s->setfont(l->kind == AB_URL ? p->url_fid : FONT_UI);
            int tw, th;
            s->getstringsize((const unsigned char *)l->text, &tw, &th);
            const int tx = (W - tw) / 2;

            if (l->kind == AB_CAP)                /* flanking hairlines */
            {
                const int ly = y + lh / 2;
                s->set_foreground(fg);
                lcd_set_drawmode(DRMODE_SOLID);
                if (tx - 8 > margin)
                    s->fillrect(margin, ly, tx - 8 - margin, 1);
                if (tx + tw + 8 < W - margin)
                    s->fillrect(tx + tw + 8, ly, (W - margin) - (tx + tw + 8), 1);
            }

            s->set_foreground(fg);
            lcd_set_drawmode(DRMODE_FG);
            s->putsxy(tx, y + (lh - th) / 2, (const unsigned char *)l->text);
        }
        y += lh;
    }

    s->setfont(FONT_UI);
    lcd_set_drawmode(DRMODE_SOLID);
    s->update_viewport();
    s->set_viewport(NULL);
}

static int about_poll(struct trimpod_page *self, int timeout)
{
    (void)timeout;
    return get_action(self->context, HZ / 25);    /* ~25 fps animation tick */
}

static enum trimpod_page_result about_on_action(struct trimpod_page *self,
                                                int action)
{
    (void)self;
    return (action == ACTION_STD_CANCEL) ? TRIMPOD_PAGE_DONE : TRIMPOD_PAGE_STAY;
}

static const struct trimpod_page_vtable about_vtable =
{
    .legend    = NULL,            /* just the "About" title, no status-bar legend */
    .draw      = about_draw,
    .poll      = about_poll,
    .on_action = about_on_action,
};

/* only B leaves */
static const int about_allowed[] = { ACTION_STD_CANCEL, -1 };

void trimpod_about(void)
{
    struct screen *s = &screens[SCREEN_MAIN];
    struct viewport vp = {0};
    /* The only resource the page holds. font_load is refcounted, so this is
     * the skin's already-loaded language-aware 18px font with one more ref.
     * Everything else lives in the stack-allocated `p` (no heap). */
    int fid = about_load_url_font();
    if (fid < 0)
        fid = FONT_UI;

    struct about_page p =
    {
        .base = { .vt = &about_vtable, .context = CONTEXT_STD,
                  .allowed = about_allowed,
                  .title = (const char *)str(LANG_TRIMPOD_ABOUT),
                  .animated = true, .no_header_refresh = true },
        .start_tick  = current_tick,
        .header_done = false,
        .url_fid     = fid,
    };

    /* Expand the reel once: wrap URLs in the compact language-aware font. */
    viewport_set_defaults(&vp, s->screen_type);
    about_build(&p, s, vp.width - 2 * 10);
    s->setfont(FONT_UI);

    trimpod_page_run(&p.base);   /* always returns (B / USB); no early exit */

    /* balance the font_load above; restore the UI font first so the screen is
     * not left pointing at the font we unload. */
    s->setfont(FONT_UI);
    if (fid != FONT_UI)
        font_unload(fid);
}

/* ---- Message page: a static block of text rows.  Same chrome as every other
 * page (title header + slide); the body is a left-aligned list, the block
 * itself centred on the widest row.  B leaves.  The Controls reference card
 * and the empty-playlist hint are both this page with different rows. ------- */

struct message_page
{
    struct trimpod_page base;
    const char *const *rows;
    int nrows;
};

static void message_draw(struct trimpod_page *self)
{
    struct message_page *p = (struct message_page *)self;
    struct screen *s = &screens[SCREEN_MAIN];
    struct viewport vp = {0};

    viewport_set_defaults(&vp, s->screen_type);   /* content area, not header */
    s->set_viewport(&vp);
    s->clear_viewport();

    int w, fh = 0, block = 0;
    for (int i = 0; i < p->nrows; i++)
    {
        const char *row = trimpod_resolve_string(p->rows[i]);
        s->getstringsize((const unsigned char *)row, &w, &fh);
        if (w > block) block = w;                 /* the widest row sets the left edge */
    }

    const int line = fh + 6;
    int x = (vp.width - block) / 2;
    int y = (vp.height - p->nrows * line) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    for (int i = 0; i < p->nrows; i++, y += line)
        s->putsxy(x, y, (const unsigned char *)trimpod_resolve_string(p->rows[i]));

    s->update_viewport();
    s->set_viewport(NULL);
}

static enum trimpod_page_result message_on_action(struct trimpod_page *self,
                                                  int action)
{
    (void)self;
    return (action == ACTION_STD_CANCEL) ? TRIMPOD_PAGE_DONE : TRIMPOD_PAGE_STAY;
}

static const struct trimpod_page_vtable message_vtable =
{
    .legend    = NULL,            /* just the title, no status-bar legend */
    .draw      = message_draw,
    .poll      = NULL,            /* default: get_action(CONTEXT_STD) */
    .on_action = message_on_action,
};

/* only B leaves */
static const int message_allowed[] = { ACTION_STD_CANCEL, -1 };

void trimpod_message_page(const char *title, const char *const *rows, int nrows)
{
    struct message_page p =
    {
        .base = { .vt = &message_vtable, .context = CONTEXT_STD,
                  .allowed = message_allowed, .title = title },
        .rows = rows, .nrows = nrows,
    };
    trimpod_page_run(&p.base);
}

/* Controls: a static reference card for the main inputs. */
static const char *const controls_rows[] = {
    ID2P(LANG_TRIMPOD_CONTROL_BACK),
    ID2P(LANG_TRIMPOD_CONTROL_SELECT),
    ID2P(LANG_TRIMPOD_CONTROL_CONTEXT),
    ID2P(LANG_TRIMPOD_CONTROL_LYRICS),
    ID2P(LANG_TRIMPOD_CONTROL_LOCK),
};
#define CONTROLS_NROWS ((int)(sizeof(controls_rows) / sizeof(controls_rows[0])))

void trimpod_controls(void)
{
    trimpod_message_page((const char *)str(LANG_TRIMPOD_CONTROLS),
                         controls_rows, CONTROLS_NROWS);
}

/* Measure every reel line once to fault its glyphs into the cache now (startup),
 * sparing the first About open the synchronous .fnt reads.  font_load refcounts
 * the .sbs-resident compact font, so the warmed cache survives the unload. */
void trimpod_about_prewarm(void)
{
    struct screen *s = &screens[SCREEN_MAIN];
    int fid = about_load_url_font();
    for (int i = 0; i < ABOUT_NLINES; i++)
    {
        const struct about_line *l = &about_lines[i];
        if (!l->text)
            continue;
        s->setfont(l->kind == AB_URL && fid >= 0 ? fid : FONT_UI);
        int w, h;
        s->getstringsize((const unsigned char *)l->text, &w, &h);
    }
    s->setfont(FONT_UI);
    if (fid >= 0 && fid != FONT_UI)
        font_unload(fid);
}
