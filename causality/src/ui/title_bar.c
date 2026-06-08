// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* title_bar.c — Custom window title bar for Causality windows.
 *
 * Provides drag-to-move, minimize, maximize/restore, close buttons, and
 * an optional left-aligned menu bar embedded in the title bar strip.
 *
 * Architecture:
 *   win->root         (vertical flex, fills window, system-managed)
 *   ├── win->title_bar_node  (horizontal, 30 px fixed height)
 *   │   ├── ca_menu_bar(...)     (left-aligned menus, if any)
 *   │   ├── drag div             (flex-grow:1, drag-to-move, title text)
 *   │   └── controls div        (min / max / close buttons)
 *   └── win->content_root  (flex-grow: 1, holds user content)
 */

#include "title_bar.h"
#include "node.h"
#include "widget.h"
#include "../core/ca_internal.h"
#include "../../include/causality.h"

#include <GLFW/glfw3.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define TITLE_BAR_SIDE_PADDING_PX 2.0f

/* ------------------------------------------------------------------ */
/* Window-drag callbacks                                               */
/* ------------------------------------------------------------------ */

static void on_titlebar_drag_start(const Ca_DragEvent *ev, void *ud)
{
    (void)ud;
    Ca_Window *win = ev->window;
    int wx, wy;
    double cx, cy;
    glfwGetWindowPos(win->glfw, &wx, &wy);
    /* Use glfwGetCursorPos (real-time OS query) instead of ev->start_x which
       comes from the cached cursor-callback value.  They agree at drag start
       but the real-time variant keeps drag and move consistent. */
    glfwGetCursorPos(win->glfw, &cx, &cy);
    win->titlebar_drag_win_x    = wx;
    win->titlebar_drag_win_y    = wy;
    win->titlebar_drag_screen_x = (double)wx + cx;
    win->titlebar_drag_screen_y = (double)wy + cy;
}

static void on_titlebar_drag(const Ca_DragEvent *ev, void *ud)
{
    (void)ud;
    Ca_Window *win = ev->window;

    /* ev->x/y come from win->mouse_x/y which is only updated when the
       physical cursor moves (cursor-pos callback).  When we call
       glfwSetWindowPos the window moves under a stationary cursor — no
       callback fires — leaving win->mouse_x stale relative to the new
       window pos.  Combining that stale value with a fresh glfwGetWindowPos
       gives a wrong screen-cursor estimate, accumulating drift.
       Fix: query the cursor position directly from the OS via
       glfwGetCursorPos; it is always consistent with the current window pos
       returned by glfwGetWindowPos (same Cocoa/Win32/XQueryPointer frame). */
    int wx, wy;
    double cx, cy;
    glfwGetWindowPos(win->glfw, &wx, &wy);
    glfwGetCursorPos(win->glfw, &cx, &cy);
    double cur_sx = (double)wx + cx;
    double cur_sy = (double)wy + cy;
    double dx = cur_sx - win->titlebar_drag_screen_x;
    double dy = cur_sy - win->titlebar_drag_screen_y;
    glfwSetWindowPos(win->glfw,
                     win->titlebar_drag_win_x + (int)dx,
                     win->titlebar_drag_win_y + (int)dy);
}

static void on_titlebar_drag_end(const Ca_DragEvent *ev, void *ud)
{
    (void)ev; (void)ud;
}

/* ------------------------------------------------------------------ */
/* Window-control button callbacks                                     */
/* ------------------------------------------------------------------ */

static void on_close_click(Ca_Button *btn, void *ud)
{
    (void)btn;
    Ca_Window *win = (Ca_Window *)ud;
    ca_window_close(win);
}

static void on_minimize_click(Ca_Button *btn, void *ud)
{
    (void)btn;
    Ca_Window *win = (Ca_Window *)ud;
    glfwIconifyWindow(win->glfw);
}

static void on_maximize_click(Ca_Button *btn, void *ud)
{
    (void)btn;
    Ca_Window *win = (Ca_Window *)ud;

    if (win->titlebar_maximized) {
        /* Restore to pre-maximize geometry */
        glfwSetWindowPos(win->glfw,
                         win->titlebar_pre_max_x, win->titlebar_pre_max_y);
        glfwSetWindowSize(win->glfw,
                          win->titlebar_pre_max_w, win->titlebar_pre_max_h);
        win->titlebar_maximized     = false;
        win->titlebar_needs_rebuild = true;
    } else {
        ca_window_maximize(win);
    }
}

/* ------------------------------------------------------------------ */
/* Initialisation — called once from ca_ui_window_init                */
/* ------------------------------------------------------------------ */

void ca_title_bar_init(Ca_Window *win)
{
    assert(win);

    /* ---- True root: vertical column that fills the whole window ---- */
    Ca_Node *root = ca_node_root(win);
    assert(root && "ca_title_bar_init: failed to allocate root node");
    root->desc.direction  = CA_VERTICAL;
    root->desc.overflow_x = 1; /* hidden */
    root->desc.overflow_y = 1; /* hidden — root itself does not scroll */
    root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;

    /* ---- Title bar node: horizontal strip, height scales with ui_scale ---- */
    float sc_init = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    Ca_NodeDesc tb = {0};
    tb.direction     = CA_HORIZONTAL;
    tb.height        = 22.0f * sc_init;
    tb.align_items   = CA_ALIGN_CENTER;
    /* Inset the menu bar on the left and the window controls on the
       right so neither group is flush against the window chrome. */
     tb.padding_left  = TITLE_BAR_SIDE_PADDING_PX * sc_init;
     tb.padding_right = TITLE_BAR_SIDE_PADDING_PX * sc_init;
    /* Retro theme: raised panel face with top-light / bottom-shadow bevel */
    tb.background       = ca_color(0x2a / 255.0f, 0x2a / 255.0f, 0x32 / 255.0f, 1.0f);
    tb.border_top_w     = 2.0f * sc_init;
    tb.border_top_c     = ca_color(0x4c / 255.0f, 0x4c / 255.0f, 0x58 / 255.0f, 1.0f);
    tb.border_bottom_w  = 2.0f * sc_init;
    tb.border_bottom_c  = ca_color(0x07 / 255.0f, 0x07 / 255.0f, 0x09 / 255.0f, 1.0f);
    tb.overflow_x    = 1; /* hidden */
    tb.overflow_y    = 1;
    Ca_Node *tbnode = ca_node_add(root, &tb);
    assert(tbnode && "ca_title_bar_init: failed to allocate title_bar_node");
    win->title_bar_node = tbnode;

    /* ---- Content root: fills remaining space below title bar ---- */
    Ca_NodeDesc cr = {0};
    cr.direction  = CA_VERTICAL;
    cr.flex_grow  = 1.0f;
    Ca_Node *crnode = ca_node_add(root, &cr);
    assert(crnode && "ca_title_bar_init: failed to allocate content_root");
    win->content_root = crnode;

    /* ---- Status bar node: hidden by default until the user installs a
       builder via ca_window_set_status_bar. Sits as the bottom sibling
       so the user's content_root flex-grows in the middle.         ---- */
    Ca_NodeDesc sb = {0};
    sb.direction  = CA_HORIZONTAL;
    sb.height     = 0.0f;
    sb.hidden     = true;
    sb.overflow_x = 1;
    sb.overflow_y = 1;
    Ca_Node *sbnode = ca_node_add(root, &sb);
    assert(sbnode && "ca_title_bar_init: failed to allocate status_bar_node");
    win->status_bar_node = sbnode;

    win->titlebar_needs_rebuild = true;
}

/* ------------------------------------------------------------------ */
/* Rebuild — called from ca_ui_update inside a ctx_enter/leave pair   */
/* ------------------------------------------------------------------ */

void ca_title_bar_rebuild(Ca_Window *win)
{
    assert(win && win->title_bar_node);

    /* ca_div_clear removes all children and pushes title_bar_node onto
       the widget context stack so new children become its children.    */
    ca_div_clear((Ca_Div *)win->title_bar_node);

    /* Scale factor — all pixel sizes below are multiples of 22px @ 1x */
    float sc = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;

    /* Keep the container's own height in sync with the current scale */
    win->title_bar_node->desc.height        = 22.0f * sc;
    /* Keep horizontal padding in lockstep with scale so a DPI change
       at runtime doesn't push the menu/controls back against the edge. */
     win->title_bar_node->desc.padding_left  = TITLE_BAR_SIDE_PADDING_PX * sc;
     win->title_bar_node->desc.padding_right = TITLE_BAR_SIDE_PADDING_PX * sc;
    win->title_bar_node->dirty |= CA_DIRTY_LAYOUT;

    /* ---- Colours (self-contained — no CSS classes needed) ---- */
    /* Retro palette: muted title text, mid-tone icons, soft-red close */
    const uint32_t COL_TEXT_DIM  = ca_color(0x74/255.f, 0x74/255.f, 0x80/255.f, 1.0f);
    const uint32_t COL_BTN       = ca_color(0xa0/255.f, 0xa0/255.f, 0xb0/255.f, 1.0f);
    const uint32_t COL_CLOSE     = ca_color(0xc8/255.f, 0x60/255.f, 0x60/255.f, 1.0f);
    /* Bevel system: raised face, highlight edge, shadow edge */
    const uint32_t COL_FACE      = ca_color(0x2a/255.f, 0x2a/255.f, 0x32/255.f, 1.0f);
    const uint32_t COL_BEVEL_HI  = ca_color(0x4c/255.f, 0x4c/255.f, 0x58/255.f, 1.0f);
    const uint32_t COL_BEVEL_SH  = ca_color(0x07/255.f, 0x07/255.f, 0x09/255.f, 1.0f);
    /* Menu dropdown theme */
    const uint32_t COL_DROP_BG   = ca_color(0x22/255.f, 0x22/255.f, 0x28/255.f, 1.0f);
    const uint32_t COL_DROP_SEL  = ca_color(0x18/255.f, 0x2e/255.f, 0x50/255.f, 1.0f);
    const uint32_t COL_DROP_TEXT = ca_color(0xc8/255.f, 0xc8/255.f, 0xcc/255.f, 1.0f);

    /* ---- Left: optional menu bar ---- */
    if (win->titlebar_menu_count > 0) {
        /* Reconstruct public Ca_MenuDesc arrays on the stack from the
           deep-copied Ca_MenuBarMenu data stored in Ca_Window.         */
        Ca_MenuItemDesc item_bufs[CA_MAX_MENUS_PER_BAR][CA_MAX_ITEMS_PER_MENU];
        Ca_MenuItemDesc sub_bufs[CA_MAX_MENUS_PER_BAR]
                                [CA_MAX_ITEMS_PER_MENU]
                                [CA_MAX_SUB_ITEMS_PER_ITEM];
        Ca_MenuDesc menu_descs[CA_MAX_MENUS_PER_BAR];

        for (int m = 0; m < win->titlebar_menu_count; m++) {
            Ca_MenuBarMenu *mbm = &win->titlebar_menus[m];
            for (int i = 0; i < mbm->item_count; i++) {
                Ca_MenuBarItem *mbi = &mbm->items[i];
                for (int s = 0; s < mbi->sub_item_count; s++) {
                    sub_bufs[m][i][s] = (Ca_MenuItemDesc){
                        .label       = mbi->sub_items[s].label,
                        .action      = mbi->sub_items[s].action,
                        .action_data = mbi->sub_items[s].action_data,
                    };
                }
                item_bufs[m][i] = (Ca_MenuItemDesc){
                    .label          = mbi->label,
                    .action         = mbi->action,
                    .action_data    = mbi->action_data,
                    .separator      = mbi->separator,
                    .sub_items      = mbi->sub_item_count > 0
                                          ? sub_bufs[m][i] : NULL,
                    .sub_item_count = mbi->sub_item_count,
                };
            }
            menu_descs[m] = (Ca_MenuDesc){
                .label      = mbm->label,
                .items      = item_bufs[m],
                .item_count = mbm->item_count,
            };
        }

        ca_menu_bar(&(Ca_MenuBarDesc){
            .menus            = menu_descs,
            .menu_count       = win->titlebar_menu_count,
            .text_color       = COL_BTN,
            .header_highlight = COL_DROP_SEL,
            .dropdown_bg      = COL_DROP_BG,
            .dropdown_border  = COL_BEVEL_HI,
            .dropdown_hover   = COL_DROP_SEL,
            .dropdown_text    = COL_DROP_TEXT,
            .bar_height       = 22.0f * sc,
            .item_padding_lr  = 4.0f  * sc,
            .item_font_size   = 12.0f,
        });
    }

    /* ---- Centre: drag zone (invisible, handles window dragging) ---- */
    Ca_Node *drag = (Ca_Node *)ca_div_begin(&(Ca_DivDesc){
        .height        = 22.0f,
        .on_drag_start = on_titlebar_drag_start,
        .on_drag       = on_titlebar_drag,
        .on_drag_end   = on_titlebar_drag_end,
    });
    drag->desc.flex_grow       = 1.0f;
    drag->desc.align_items     = CA_ALIGN_CENTER;
    drag->desc.justify_content = CA_ALIGN_CENTER;
    drag->desc.overflow_x      = 1;
    drag->desc.overflow_y      = 1;
    drag->dirty |= CA_DIRTY_LAYOUT;

    Ca_Label *ttl = ca_text(&(Ca_TextDesc){
        .text  = win->title,
        .color = COL_TEXT_DIM,
    });
    ttl->node->desc.font_size  = 12.0f;
    ttl->node->desc.text_align = 1;
    ttl->node->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;

    ca_div_end(); /* drag zone */

    /* ---- Right: window control buttons ---- */
    Ca_Node *ctrl = (Ca_Node *)ca_div_begin(&(Ca_DivDesc){ .height = 22.0f });
    ctrl->desc.align_items = CA_ALIGN_CENTER;
    ctrl->dirty |= CA_DIRTY_LAYOUT;

    /* Convenience macro: apply raised bevel to a button node */
#define RETRO_BTN_BEVEL(btn_node) do { \
    (btn_node)->desc.border_top_w    = 2.0f; \
    (btn_node)->desc.border_top_c    = COL_BEVEL_HI; \
    (btn_node)->desc.border_left_w   = 2.0f; \
    (btn_node)->desc.border_left_c   = COL_BEVEL_HI; \
    (btn_node)->desc.border_bottom_w = 2.0f; \
    (btn_node)->desc.border_bottom_c = COL_BEVEL_SH; \
    (btn_node)->desc.border_right_w  = 2.0f; \
    (btn_node)->desc.border_right_c  = COL_BEVEL_SH; \
    (btn_node)->dirty |= CA_DIRTY_LAYOUT; \
} while(0)

    Ca_Button *min_btn = ca_btn_begin(&(Ca_BtnDesc){
        .text       = CA_ICON_FA_MINUS,
        .width      = 18.0f,
        .height     = 16.0f,
        .background = COL_FACE,
        .text_color = COL_BTN,
        .on_click   = on_minimize_click,
        .click_data = win,
    });
    min_btn->node->desc.font_size  = 11.0f;
    min_btn->node->desc.text_align = 1;
    RETRO_BTN_BEVEL(min_btn->node);
    min_btn->node->dirty |= CA_DIRTY_CONTENT;
    ca_btn_end(); /* min btn */

    Ca_Button *max_btn = ca_btn_begin(&(Ca_BtnDesc){
        .text       = win->titlebar_maximized
                          ? CA_ICON_FA_WINDOW_RESTORE
                          : CA_ICON_FA_WINDOW_MAXIMIZE,
        .width      = 18.0f,
        .height     = 16.0f,
        .background = COL_FACE,
        .text_color = COL_BTN,
        .on_click   = on_maximize_click,
        .click_data = win,
    });
    max_btn->node->desc.font_size  = 11.0f;
    max_btn->node->desc.text_align = 1;
    RETRO_BTN_BEVEL(max_btn->node);
    max_btn->node->dirty |= CA_DIRTY_CONTENT;
    ca_btn_end(); /* max btn */

    Ca_Button *cls_btn = ca_btn_begin(&(Ca_BtnDesc){
        .text       = CA_ICON_FA_TIMES,
        .width      = 18.0f,
        .height     = 16.0f,
        .background = COL_FACE,
        .text_color = COL_CLOSE,
        .on_click   = on_close_click,
        .click_data = win,
    });
    cls_btn->node->desc.font_size  = 11.0f;
    cls_btn->node->desc.text_align = 1;
    RETRO_BTN_BEVEL(cls_btn->node);
    cls_btn->node->dirty |= CA_DIRTY_CONTENT;
    ca_btn_end(); /* close btn */

#undef RETRO_BTN_BEVEL

    ca_div_end(); /* controls */

    ca_div_end(); /* title_bar_node */
}

/* ------------------------------------------------------------------ */
/* Public API implementations                                          */
/* ------------------------------------------------------------------ */

void ca_window_set_title(Ca_Window *window, const char *title)
{
    if (!window || !window->in_use) return;
    snprintf(window->title, sizeof(window->title), "%s", title ? title : "");
    glfwSetWindowTitle(window->glfw, window->title);
    window->titlebar_needs_rebuild = true;
}

void ca_window_set_title_bar_menus(Ca_Window        *window,
                                   const Ca_MenuDesc *menus, int count)
{
    if (!window || !window->in_use) return;
    if (count < 0) count = 0;
    if (count > CA_MAX_MENUS_PER_BAR) count = CA_MAX_MENUS_PER_BAR;

    window->titlebar_menu_count = count;
    memset(window->titlebar_menus, 0,
           sizeof(Ca_MenuBarMenu) * CA_MAX_MENUS_PER_BAR);

    for (int m = 0; m < count; m++) {
        Ca_MenuBarMenu    *dst = &window->titlebar_menus[m];
        const Ca_MenuDesc *src = &menus[m];

        snprintf(dst->label, CA_MENU_LABEL_MAX, "%s",
                 src->label ? src->label : "");
        dst->item_count = src->item_count;
        if (dst->item_count > CA_MAX_ITEMS_PER_MENU)
            dst->item_count = CA_MAX_ITEMS_PER_MENU;
        dst->active_sub = -1;

        for (int i = 0; i < dst->item_count; i++) {
            Ca_MenuBarItem        *ditem = &dst->items[i];
            const Ca_MenuItemDesc *sitem = &src->items[i];

            snprintf(ditem->label, CA_MENU_LABEL_MAX, "%s",
                     sitem->label ? sitem->label : "");
            ditem->action      = sitem->action;
            ditem->action_data = sitem->action_data;
            ditem->separator   = sitem->separator;

            int nsub = sitem->sub_item_count;
            if (nsub > CA_MAX_SUB_ITEMS_PER_ITEM) nsub = CA_MAX_SUB_ITEMS_PER_ITEM;
            ditem->sub_item_count = nsub;

            for (int s = 0; s < nsub; s++) {
                Ca_MenuBarSubItem     *dsub = &ditem->sub_items[s];
                const Ca_MenuItemDesc *ssub = &sitem->sub_items[s];
                snprintf(dsub->label, CA_MENU_LABEL_MAX, "%s",
                         ssub->label ? ssub->label : "");
                dsub->action      = ssub->action;
                dsub->action_data = ssub->action_data;
            }
        }
    }

    window->titlebar_needs_rebuild = true;
}

/* ------------------------------------------------------------------ */
/* Status bar — public + internal                                      */
/* ------------------------------------------------------------------ */

void ca_status_bar_rebuild(Ca_Window *win)
{
    assert(win && win->status_bar_node);

    /* Reset to empty children regardless of whether a builder is set;
       this lets ca_window_set_status_bar(NULL,...) cleanly clear it. */
    ca_div_clear((Ca_Div *)win->status_bar_node);

    if (win->status_bar_fn) {
        win->status_bar_fn(win, win->status_bar_data);
    }

    ca_div_end(); /* status_bar_node — auto-pops widget context */
}

void ca_window_set_status_bar(Ca_Window      *window,
                              Ca_StatusBarFn  fn,
                              void           *user_data,
                              float           height)
{
    if (!window || !window->in_use || !window->status_bar_node) {
        return;
    }

    window->status_bar_fn     = fn;
    window->status_bar_data   = user_data;
    {
        float sc = window->ui_scale > 0.0f ? window->ui_scale : 1.0f;
        window->status_bar_raw_height = (height > 0.0f && fn) ? height : 0.0f;
        window->status_bar_height = (height > 0.0f && fn) ? (height * sc) : 0.0f;
    }

    Ca_Node *sb = window->status_bar_node;
    sb->desc.height = window->status_bar_height;
    sb->desc.hidden = (fn == NULL || window->status_bar_height <= 0.0f);
    sb->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;

    /* Mark root layout-dirty so the content_root resizes to absorb /
       release the bar's height in the same frame. */
    if (window->root) {
        window->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN;
    }

    window->statusbar_needs_rebuild = true;
}

void ca_window_invalidate_status_bar(Ca_Window *window)
{
    if (!window || !window->in_use) {
        return;
    }
    window->statusbar_needs_rebuild = true;
}
