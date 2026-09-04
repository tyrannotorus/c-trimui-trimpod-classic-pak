/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Robert E. Hak
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
2005 Kevin Ferrare :
 - Multi screen support
 - Rewrote/removed a lot of code now useless with the new gui API
*/
#include <stdbool.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"

#include "appevents.h"
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "menu.h"
#include "button.h"
#include "kernel.h"
#include "debug.h"
#include "usb.h"
#include "panic.h"
#include "settings.h"
#include "settings_list.h"
#include "option_select.h"
#include "trimpod_transition.h"   /* page slide transitions (shared) */
#include "trimpod_page.h"         /* trimpod_home_pending (hold-BACK -> root) */
#include "trimpod_visualizer.h"   /* idle auto-start while music plays */
#include "screens.h"
#include "lang.h"
#include "misc.h"
#include "action.h"
#include "menus/exported_menus.h"
#include "string.h"
#include "root_menu.h"
#include "audio.h"
#include "viewport.h"
#include "shortcuts.h"
#include "statusbar-skinned.h"

#include "icons.h"

/* gui api */
#include "list.h"

#define MAX_MENUS 8
/* used to allow for dynamic menus */
#define MAX_MENU_SUBITEMS 64
static struct menu_item_ex *current_submenus_menu;
static int current_subitems[MAX_MENU_SUBITEMS];
static int current_subitems_count = 0;

struct menu_data_t
{
    const struct menu_item_ex *menu;
    int selected;
};

static int empty_menu_callback(int action, const struct menu_item_ex *this_item, struct gui_synclist *this_list)
{
    return action;
    (void)this_item;
    (void)this_list;
}

static void get_menu_callback(const struct menu_item_ex *m,
                        menu_callback_type *menu_callback)
{
    if (m->flags&(MENU_HAS_DESC|MENU_DYNAMIC_DESC))
        *menu_callback= m->callback_and_desc->menu_callback;
    else
        *menu_callback = m->menu_callback;

    if (!*menu_callback)
        *menu_callback = &empty_menu_callback;
}

static bool query_audio_status(int *old_audio_status)
{
    bool redraw_list = false;
    /* query audio status to see if it changed */
    int new_audio_status = audio_status();
    if (*old_audio_status != new_audio_status)
    {  /* force a redraw if anything changed the audio status from outside */
        *old_audio_status = new_audio_status;
        redraw_list = true;
    }
    return redraw_list;
}

static int get_menu_selection(int selected_item, const struct menu_item_ex *menu)
{
    int type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_MENU || type == MT_RETURN_ID)
        && (selected_item<current_subitems_count))
        return current_subitems[selected_item];
    return selected_item;
}
static int find_menu_selection(int selected)
{
    int i;
    for (i=0; i< current_subitems_count; i++)
        if (current_subitems[i] == selected)
            return i;
    return 0;
}
static const char* get_menu_item_name(int selected_item,
                                      void * data,
                                      char *buffer,
                                      size_t buffer_len)
{
    const char *name;
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    /* only MT_MENU or MT_RETURN_ID is allowed in here */
    if (type == MT_RETURN_ID)
    {
        if (menu->flags&MENU_DYNAMIC_DESC)
        {
            name = menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);
        }
        else
            name = menu->strings[selected_item];

        if (P2ID((unsigned char *)name) > VOICEONLY_DELIMITER)
            name = "";

        return name;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    /* Trimpod: a value row with an empty label renders its value string AS the
     * row name (e.g. the Pause/Play toggle), left-aligned like an action row. */
    if (menu->flags & MENU_VALUE_ITEM)
    {
        const char *d = P2STR(menu->callback_and_desc->desc);
        if (!d || !d[0])
        {
            const struct menu_value_cb *vc = menu->function_param->param;
            return vc->get(vc->ctx, buffer, buffer_len);
        }
    }

    if ((menu->flags&MENU_DYNAMIC_DESC) && (type != MT_SETTING_W_TEXT))
        return menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);

    type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
    {
        const struct settings_list *v = find_setting(menu->variable);
        if (v)
            return str(v->lang_id);
        else return "Not Done yet!";
    }
    return P2STR(menu->callback_and_desc->desc);
}

/* Trimpod: a row shows the disclosure arrow if it opens another menu/settings
 * screen -- i.e. it is a submenu (MT_MENU, always navigation) or it carries the
 * explicit MENU_SHOW_CHEVRON flag.  Actions, settings, stringlist and file-list
 * rows (the latter don't use this callback at all) get nothing. */
static bool menu_get_chevron(int selected_item, void * data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    if (type == MT_RETURN_ID)
        return false;
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    return ((menu->flags&MENU_TYPE_MASK) == MT_MENU) ||
           (menu->flags & MENU_SHOW_CHEVRON);
}

/* Trimpod: A on an inline value row cycles its value forward (+1). */
int trimpod_value_item_activate(void *value_cb)
{
    const struct menu_value_cb *vc = (const struct menu_value_cb *)value_cb;
    vc->cycle(vc->ctx, +1);
    return 0;
}

/* Trimpod: a setting / value row shows its current value right-aligned (the iPod
 * look), so the inline LEFT/RIGHT value cycling is visible without opening a
 * separate screen.  Other rows return nothing (NULL). */
static const char *menu_get_indicator(int selected_item, void *data)
{
    static char buf[32];
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    if (type == MT_RETURN_ID)
        return NULL;
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    if (menu->flags & MENU_VALUE_ITEM)
    {
        const char *d = P2STR(menu->callback_and_desc->desc);
        if (!d || !d[0])
            return NULL;   /* label-only value row (Pause/Play): no right value */
        const struct menu_value_cb *vc = menu->function_param->param;
        return vc->get(vc->ctx, buf, sizeof buf);
    }

    type = (menu->flags&MENU_TYPE_MASK);
    if (type == MT_SETTING || type == MT_SETTING_W_TEXT)
    {
        const struct settings_list *s = find_setting(menu->variable);
        if (s)
        {
            int cur = ((s->flags & F_T_MASK) == F_T_BOOL)
                          ? (*(bool *)s->setting ? 1 : 0)
                          : *(int *)s->setting;
            return option_get_valuestring(s, buf, sizeof buf, cur);
        }
    }
    return NULL;
}

static char* init_title(const struct menu_item_ex *menu, int *icon,
                        char* buf, size_t buf_sz)
{
    char *title;

    if (menu->flags&MENU_HAS_DESC)
    {
        *icon = menu->callback_and_desc->icon_id;
        title = P2STR(menu->callback_and_desc->desc);
    }
    else if (menu->flags&MENU_DYNAMIC_DESC)
    {
        *icon = menu->menu_get_name_and_icon->icon_id;
        title = menu->menu_get_name_and_icon->
                      list_get_name(-1, menu->menu_get_name_and_icon->
                                    list_get_name_data, buf, buf_sz);
    }
    else
    {
        *icon = Icon_NOICON;
        title = "";
    }

    if (*icon == Icon_NOICON)
        *icon = Icon_Submenu_Entered;

    return title;
}

static int init_menu_lists(const struct menu_item_ex *menu,
                     struct gui_synclist *lists, int selected, bool callback,
                     struct viewport parent[NB_SCREENS], char* buf, size_t buf_sz)
{
    if (!menu || !lists)
    {
        panicf("init_menu_lists, NULL pointer");
        return 0;
    }

    int i;
    int start_action = ACTION_ENTER_MENUITEM;
    int count = MIN(MENU_GET_COUNT(menu->flags), MAX_MENU_SUBITEMS);
    int type = (menu->flags&MENU_TYPE_MASK);
    menu_callback_type menu_callback = &empty_menu_callback;
    int icon;
    char * title;
    current_subitems_count = 0;

    if (type == MT_RETURN_ID)
        get_menu_callback(menu, &menu_callback);

    for (i=0; i<count; i++)
    {
        if (type != MT_RETURN_ID)
            get_menu_callback(menu->submenus[i],&menu_callback);

        if (menu_callback(ACTION_REQUEST_MENUITEM,
            type==MT_RETURN_ID ? (void*)(intptr_t)i: menu->submenus[i], lists)
                != ACTION_EXIT_MENUITEM)
        {
            current_subitems[current_subitems_count] = i;
            current_subitems_count++;
        }
    }

    current_submenus_menu = (struct menu_item_ex *)menu;

    gui_synclist_init(lists,get_menu_item_name,(void*)menu,false,1, parent);
    title = init_title(menu, &icon, buf, buf_sz);
    gui_synclist_set_title(lists, title, icon);
    gui_synclist_set_chevron_callback(lists, menu_get_chevron);
    gui_synclist_set_indicator_callback(lists, menu_get_indicator);
    gui_synclist_set_nb_items(lists,current_subitems_count);
    gui_synclist_select_item(lists, find_menu_selection(selected));

    get_menu_callback(menu,&menu_callback);
    if (callback)
        start_action = menu_callback(start_action, menu, lists);

    return start_action;
}

void do_setting_screen(const struct settings_list *setting, const char * title,
                        struct viewport parent[NB_SCREENS])
{
    char padded_title[MAX_PATH];
    /* Pad the title string by repeating it. This is needed
       so the scroll settings title can actually be used to
       test the setting */
    if (setting->flags&F_PADTITLE)
    {
        int i = 0, len;
        title = P2STR((unsigned char*)title);
        len = strlen(title);
        while (i < MAX_PATH-1)
        {
            int padlen = MIN(len, MAX_PATH-1-i);
            memcpy(&padded_title[i], title, padlen);
            i += padlen;
            if (i<MAX_PATH-1)
                padded_title[i++] = ' ';
        }
        padded_title[i] = '\0';
        title = padded_title;
    }

    option_screen((struct settings_list *)setting, parent,
                  setting->flags&F_TEMPVAR, (char*)title);
}

/* display a menu */
/* render callback for the shared page-slide transition: paint the menu list */
static void menu_transition_render(void *ctx)
{
    gui_synclist_draw((struct gui_synclist *)ctx);
}

int do_menu(const struct menu_item_ex *start_menu, int *start_selected,
            struct viewport parent[NB_SCREENS], bool hide_theme)
{
    int selected = start_selected? *start_selected : 0;
    int ret = 0;
    int action;
    int start_action;
    int icon;
    char buf[80], *title;
    struct gui_synclist lists;
    const struct menu_item_ex *temp = NULL;
    const struct menu_item_ex *menu = start_menu;

    bool in_stringlist, done = false;
    bool redraw_lists;
    /* >=0 -> the next list redraw is an animated page slide in that direction */
    int menu_trans = -1;

    int old_audio_status = audio_status();

    title = init_title(menu, &icon, buf, sizeof buf);
    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(title, icon, i);
        viewportmanager_theme_enable(i, !hide_theme, NULL);
    }
    struct menu_data_t mstack[MAX_MENUS]; /* menu, selected */
    int stack_top = 0;

    struct viewport *vps = NULL;
    menu_callback_type menu_callback = &empty_menu_callback;

    /* if hide_theme is true, assume parent has been fixed before passed into
     * this function, e.g. with viewport_set_defaults(parent, screen)
     * start_action allows an action to be processed
     * by menu logic by bypassing get_action on the initial run */
    start_action = init_menu_lists(menu, &lists, selected, true, parent,
                                   buf, sizeof buf);
    vps = *(lists.parent);
    in_stringlist = ((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID);
    /* load the callback, and only reload it if menu changes */
    get_menu_callback(menu, &menu_callback);

    /* slide the menu in: back if we got here by backing out of a deeper screen
     * (it armed a back slide), otherwise forward.  The very first screen at app
     * launch has no prior frame to slide from, so render it with no slide.  A
     * return from a screen that aborted before rendering (an empty facet that
     * only splashed) is handled inside animate: it presents with no motion. */
    if (trimpod_transition_first_screen())
        gui_synclist_draw(&lists);
    else
        trimpod_transition_animate(
            trimpod_transition_take_back() ? TRIMPOD_TRANS_BACK : TRIMPOD_TRANS_FORWARD,
            menu_transition_render, &lists);

    while (!done)
    {
        /* A nested screen (a submenu, or a page launched from a function item)
         * requested home: unwind this menu too so we land on the Main Menu. */
        if (trimpod_home_pending)
        {
            ret = GO_TO_ROOT;
            break;
        }

        keyclick_set_callback(gui_synclist_keyclick_callback, &lists);

        if (UNLIKELY(start_action != ACTION_ENTER_MENUITEM))
        {
            action = start_action;
            start_action = ACTION_ENTER_MENUITEM;
        }
        else
            action = get_action(CONTEXT_MAINMENU|ALLOW_SOFTLOCK,
                                list_do_action_timeout(&lists, HZ));
            /* HZ so the status bar redraws corectly */

        /* query audio status to see if it changed */
        redraw_lists = query_audio_status(&old_audio_status);

        /* Idle auto-start of the visualizer while music plays (the WPS runs
         * its own inline hook -- it restores the skin around it). */
        if (action == ACTION_NONE && trimpod_visualizer_maybe_autostart())
            redraw_lists = true;

        int new_action = menu_callback(action, menu, &lists);
        if (new_action == ACTION_EXIT_AFTER_THIS_MENUITEM)
            ret = MENU_SELECTED_EXIT; /* exit after return from selection */
        else if (new_action == ACTION_REDRAW)
            redraw_lists = true;
        else
            action = new_action;

        /* Trimpod: LEFT/RIGHT cycle a setting row's value in place and apply it
         * live (the iPod inline adjust) instead of page-scrolling.  Uses the
         * canonical option_select_next_val(); other rows page-scroll normally. */
        if ((action == ACTION_LISTTREE_PGUP || action == ACTION_LISTTREE_PGDOWN)
            && (menu->flags & MENU_TYPE_MASK) == MT_MENU)
        {
            int isel = get_menu_selection(gui_synclist_get_sel_pos(&lists), menu);
            const struct menu_item_ex *iitem = menu->submenus[isel];
            int itype = (iitem->flags & MENU_TYPE_MASK);
            if (iitem->flags & MENU_VALUE_ITEM)        /* Trimpod inline knob */
            {
                /* Label-only toggles (empty desc, e.g. Pause/Play) are A-only;
                 * LEFT/RIGHT must not cycle them. */
                const char *d = P2STR(iitem->callback_and_desc->desc);
                if (d && d[0])
                {
                    const struct menu_value_cb *vc = iitem->function_param->param;
                    vc->cycle(vc->ctx, action == ACTION_LISTTREE_PGUP ? -1 : +1);
                }
                redraw_lists = true;
                action = ACTION_NONE;   /* consume: don't page-scroll the list */
            }
            else if (itype == MT_SETTING || itype == MT_SETTING_W_TEXT)
            {
                const struct settings_list *iset = find_setting(iitem->variable);
                if (iset)
                {
                    option_select_next_val(iset,
                                           action == ACTION_LISTTREE_PGUP, true);
                    redraw_lists = true;
                }
                action = ACTION_NONE;   /* consume: don't page-scroll the list */
            }
        }

        if (LIKELY(gui_synclist_do_button(&lists, &action)))
            continue;
        /* Idle return to Now Playing.  Only after do_button: it disarms the
         * list's redraw callback, which list_do_action_timeout armed with a
         * pointer to `lists` on this stack frame. */
        else if (action == ACTION_NONE && trimpod_idle_to_wps())
        {
            ret = GO_TO_ROOT;
            done = true;
        }
        else if (action == ACTION_TP_HOME)   /* hold BACK: jump to the Main Menu */
        {
            /* already at the Main Menu -> nothing to jump to, so do nothing */
            if (menu != &root_menu_)
            {
                trimpod_home_pending = true;
                ret = GO_TO_ROOT;
                done = true;
            }
        }
        else if (action == ACTION_TREE_WPS)
        {
            ret = GO_TO_PREVIOUS_MUSIC;
            done = true;
        }
        else if (action == ACTION_TREE_STOP)
        {
            redraw_lists = list_stop_handler();
        }
        else if (action == ACTION_STD_CONTEXT)
        {
            /* Trimpod's root menu is fixed, so Hold-A has no context action there
             * -- and the stock GO_TO_ROOTITEM_CONTEXT reorder screen is NOT wired
             * into the dispatch, so returning it spins the dispatch loop (freeze).
             * Only non-root setting rows have a context menu (reset). */
            if (menu != &root_menu_ && !in_stringlist)
            {
                int type = (menu->flags&MENU_TYPE_MASK);
                selected = get_menu_selection(gui_synclist_get_sel_pos(&lists),menu);
                if (type == MT_MENU)
                {
                    temp = menu->submenus[selected];
                    type = (temp->flags&MENU_TYPE_MASK);
                }
                else
                    type = -1;

                if (type == MT_SETTING_W_TEXT || type == MT_SETTING)
                {
                    const struct menu_item_ex *context_menu;
                    const struct settings_list *setting =
                            find_setting(temp->variable);

                    MENUITEM_STRINGLIST(settings_op_menu,
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_RESET_SETTING),
                                       );
                    context_menu = &settings_op_menu;
                    int msel = do_menu(context_menu, NULL, NULL, false);

                    switch (msel)
                    {
                        case GO_TO_PREVIOUS:
                            break;
                        case 0: /* reset setting */
                            reset_setting(setting, setting->setting);
                            settings_save();
                            settings_apply(false);
                            break;
                    } /* switch(do_menu()) */
                    if (menu->flags & MENU_EXITAFTERTHISMENU)
                        done = true; /* in case onplay menu contains setting */
                    redraw_lists = true;
                }
            } /* setting-row context menu (non-root, non-stringlist) */
        }
        else if (action == ACTION_STD_MENU)
        {
            if (menu != &root_menu_)
                ret = GO_TO_ROOT;
            else
                ret = GO_TO_PREVIOUS;
            done = true;
        }
        else if (action == ACTION_STD_CANCEL)
        {
            /* might be leaving list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);

            bool exiting_menu = false;
            in_stringlist = false;

            menu_callback(ACTION_EXIT_MENUITEM, menu, &lists);

            if (menu->flags&MENU_EXITAFTERTHISMENU)
                done = true;
            else if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
                exiting_menu = true;

            if (stack_top > 0)
            {
                stack_top--;
                menu = mstack[stack_top].menu;
                int msel = mstack[stack_top].selected;
                if (!exiting_menu && (menu->flags&MENU_EXITAFTERTHISMENU))
                    done = true;
                else
                    init_menu_lists(menu, &lists, msel, false, vps, buf, sizeof buf);
                redraw_lists = true;
                if (!done)
                    menu_trans = TRIMPOD_TRANS_BACK;   /* slide back to parent */
                /* new menu, so reload the callback */
                get_menu_callback(menu, &menu_callback);
            }
            else if (menu != &root_menu_)
            {
                ret = GO_TO_PREVIOUS;
                done = true;
            }
        }
        else if (action == ACTION_STD_OK)
        {
            /* entering an item that may not be a list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);
            redraw_lists = true;

            int type = (menu->flags&MENU_TYPE_MASK);
            selected = get_menu_selection(gui_synclist_get_sel_pos(&lists), menu);
            if (type == MT_MENU)
                temp = menu->submenus[selected];
            else if (!in_stringlist)
                type = -1;

            if (!in_stringlist && temp)
            {
                type = (temp->flags&MENU_TYPE_MASK);
                get_menu_callback(temp, &menu_callback);
                action = menu_callback(ACTION_ENTER_MENUITEM, temp, &lists);
                if (action == ACTION_EXIT_MENUITEM)
                    break;
            }
            switch (type)
            {
                case MT_MENU:
                    if (stack_top < MAX_MENUS)
                    {
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu, &lists, 0, true, vps, buf, sizeof buf);
                        menu_trans = TRIMPOD_TRANS_FORWARD;  /* slide into submenu */
                    }
                    break;
                case MT_FUNCTION_CALL_W_PARAM:
                case MT_FUNCTION_CALL:
                {
                    int return_value;
                    if (type == MT_FUNCTION_CALL_W_PARAM)
                    {
                        return_value = temp->function_param->function_w_param(
                                    temp->function_param->param);
                    }
                    else
                    {
                        return_value = temp->function->function();
                    }
                    if (!(menu->flags&MENU_EXITAFTERTHISMENU) ||
                            (temp->flags&MENU_EXITAFTERTHISMENU))
                    {
                        /* Reload menu but don't run the calback again FS#8117 */
                        init_menu_lists(menu, &lists, selected, false, vps,
                                        buf, sizeof buf);
                    }
                    /* if the function ran a page that armed a back slide,
                     * play it on the redraw below */
                    if (trimpod_transition_take_back())
                        menu_trans = TRIMPOD_TRANS_BACK;
                    if (temp->flags&MENU_FUNC_CHECK_RETVAL)
                    {
                        if (return_value != 0)
                        {
                            done = true;
                            ret =  return_value;
                        }
                    }
                    break;
                }
                case MT_SETTING:
                case MT_SETTING_W_TEXT:
                {
                    /* Trimpod: A cycles the value forward in place (wrapping),
                     * exactly like RIGHT -- no sub-page.  Only chevron rows
                     * (MT_MENU / MENU_SHOW_CHEVRON) open a new screen. */
                    const struct settings_list *set = find_setting(temp->variable);
                    if (set)
                        option_select_next_val(set, false, true);
                    init_menu_lists(menu, &lists, selected, false, vps, buf, sizeof buf);
                    redraw_lists = true;
                    break;
                }
                case MT_RETURN_ID:
                    if (in_stringlist)
                    {
                        done = true;
                        ret =  selected;
                    }
                    else if (stack_top < MAX_MENUS)
                    {
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu, &lists, 0, false, vps, buf, sizeof buf);
                        in_stringlist = true;
                    }
                    break;
                case MT_RETURN_VALUE:
                    ret = temp->value;
                    done = true;
                    break;

                default:
                    ret = GO_TO_PREVIOUS;
                    done = true;
                    break;
            }
            if (type != MT_MENU)
            {
                menu_callback(ACTION_EXIT_MENUITEM, temp, &lists);
            }
            if (current_submenus_menu != menu)
                init_menu_lists(menu, &lists,selected, true, vps, buf, sizeof buf);
            /* callback was changed, so reload the menu's callback */
            get_menu_callback(menu, &menu_callback);
            if ((menu->flags&MENU_EXITAFTERTHISMENU) &&
                !(temp->flags&MENU_EXITAFTERTHISMENU))
            {
                done = true;
                break;
            }
        }
        else
        {
            if (action == SYS_USB_CONNECTED)
                gui_synclist_scroll_stop(&lists);

            switch(default_event_handler(action))
            {
                case SYS_USB_CONNECTED:
                    ret = MENU_ATTACHED_USB;
                    done = true;
                    break;
                case SYS_CALL_HUNG_UP:
                case BUTTON_MULTIMEDIA_PLAYPAUSE:
                /* remove splash from playlist_resume() */
                    redraw_lists = true;
                    break;
            }
        }

        /* Home unwind in progress: skip this menu's redraw/back-slide and loop
         * to the top, which exits with GO_TO_ROOT.  Keeps home to a single slide
         * at the Main Menu instead of animating each level as it pops. */
        if (trimpod_home_pending)
            continue;

        if (redraw_lists && !done)
        {
            if (menu_callback(ACTION_REDRAW, menu, &lists) != ACTION_REDRAW)
                continue;


            gui_synclist_set_title(&lists, lists.title, lists.title_icon);
            if (menu_trans >= 0)
            {
                trimpod_transition_animate(menu_trans, menu_transition_render, &lists);
                menu_trans = -1;
            }
            else
            gui_synclist_draw(&lists);
        }
    }

    if (start_selected)
    {
        /* make sure the start_selected variable is set to
           the selected item from the menu do_menu() was called from */
        if (stack_top > 0)
        {
            menu = mstack[0].menu;
            init_menu_lists(menu, &lists, mstack[0].selected, true, vps,
                            buf, sizeof buf);
        }
        *start_selected = get_menu_selection(
                            gui_synclist_get_sel_pos(&lists), menu);
    }

    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(lists.title, lists.title_icon, i);
        viewportmanager_theme_undo(i, false);
        skinlist_set_cfg(i, NULL); /* Bugfix dangling reference in skin_draw() */
    }
    /* leaving by backing out -> tell our caller to slide us away */
    if (ret == GO_TO_PREVIOUS)
        trimpod_transition_arm_back();
    return ret;
}
