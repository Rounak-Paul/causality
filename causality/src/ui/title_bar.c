// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* title_bar.c — Custom window title bar for Causality windows.
 *
 * Provides drag-to-move, minimize, maximize/restore, close buttons, and
 * an optional left-aligned menu bar embedded in the title bar strip.
 *
 * Architecture:
 *   win->root         (vertical flex, fills window, system-managed)
 *   ├── win->title_bar_node  (horizontal, 26 px fixed height)
 *   │   ├── ca_menu_bar(...)     (left-aligned menus, if any)
 *   │   ├── drag div             (flex-grow:1, drag-to-move, title text)
 *   │   └── controls div        (min / max / close buttons)
 *   └── win->content_root  (flex-grow: 1, holds user content)
 */

#include "title_bar.h"
#include "ca_theme.h"
#include "node.h"
#include "style.h"
#include "widget.h"
#include "../core/ca_internal.h"
#include "../../include/causality.h"

#include <GLFW/glfw3.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define TITLE_BAR_HEIGHT_PX       26.0f
#define TITLE_BAR_SIDE_PADDING_PX 8.0f

/* Apply layered system and author styles to a system-owned node. */
static void apply_system_style(Ca_Node *node, Ca_ElementType type,
                               const char *classes)
{
    if (!node || !node->window || !node->window->instance || !classes) return;
    Ca_Instance *instance = node->window->instance;
    if (!instance->system_stylesheet && !instance->stylesheet) return;

    if (node->has_base_desc)
        node->desc = node->base_desc;

    node->elem_type = (uint8_t)type;
    snprintf(node->classes, sizeof(node->classes), "%s", classes);
    Ca_ResolvedStyle resolved;
    ca_style_resolve_layers(instance->system_stylesheet, instance->stylesheet,
                            node, type, node->classes, &resolved);
    const float scale = node->window->ui_scale > 0.0f
                            ? node->window->ui_scale
                            : 1.0f;
    resolved.border_width *= scale;
    resolved.border_top_w *= scale;
    resolved.border_right_w *= scale;
    resolved.border_bottom_w *= scale;
    resolved.border_left_w *= scale;
    resolved.border_radius *= scale;
    ca_style_apply_to_node(&resolved, &node->desc, NULL);
}

/* ------------------------------------------------------------------ */
/* Window-drag callbacks                                               */
/* ------------------------------------------------------------------ */

/*
 * Start a compositor-managed interactive move from the title-bar press.
 *
 * ev  Drag-start event containing the owning window.
 * ud  Unused callback data.
 */
static void on_titlebar_drag_start(const Ca_DragEvent *ev, void *ud)
{
    (void)ud;
    glfwStartInteractiveMove(ev->window->glfw);
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
        glfwRestoreWindow(win->glfw);
        win->needs_render = true;
        ca_instance_wake();
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
    tb.height        = TITLE_BAR_HEIGHT_PX * sc_init;
    tb.align_items   = CA_ALIGN_CENTER;
    /* Inset the menu bar on the left and the window controls on the
       right so neither group is flush against the window chrome. */
     tb.padding_left  = TITLE_BAR_SIDE_PADDING_PX * sc_init;
     tb.padding_right = TITLE_BAR_SIDE_PADDING_PX * sc_init;
    tb.overflow_x    = 1; /* hidden */
    tb.overflow_y    = 1;
    Ca_Node *tbnode = ca_node_add(root, &tb);
    assert(tbnode && "ca_title_bar_init: failed to allocate title_bar_node");
    win->title_bar_node = tbnode;
    tbnode->base_desc = tb;
    tbnode->has_base_desc = true;
    apply_system_style(tbnode, CA_ELEM_DIV, "ca-titlebar");

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

    /* Scale factor — all pixel sizes below are based on 26px @ 1x. */
    float sc = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;

    apply_system_style(win->title_bar_node, CA_ELEM_DIV, "ca-titlebar");

    /* Keep the container's own height in sync with the current scale */
    win->title_bar_node->desc.height = TITLE_BAR_HEIGHT_PX * sc;
    /* Keep horizontal padding in lockstep with scale so a DPI change
       at runtime doesn't push the menu/controls back against the edge. */
     win->title_bar_node->desc.padding_left  = TITLE_BAR_SIDE_PADDING_PX * sc;
     win->title_bar_node->desc.padding_right = TITLE_BAR_SIDE_PADDING_PX * sc;
    win->title_bar_node->dirty |= CA_DIRTY_LAYOUT;

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
            .style            = "ca-titlebar-menu",
            .item_style       = "ca-titlebar-menu-item",
        });
    }

    /* ---- Centre: drag zone (invisible, handles window dragging) ---- */
    Ca_Node *drag = (Ca_Node *)ca_div_begin(&(Ca_DivDesc){
        .height        = TITLE_BAR_HEIGHT_PX,
        .style         = "ca-titlebar-drag",
        .on_drag_start = on_titlebar_drag_start,
    });
    drag->desc.flex_grow       = 1.0f;
    drag->desc.overflow_x      = 1;
    drag->desc.overflow_y      = 1;
    drag->dirty |= CA_DIRTY_LAYOUT;

    Ca_Label *ttl = ca_text(&(Ca_TextDesc){
        .text  = win->title,
        .color = 0,
        .style = "ca-titlebar-title",
    });
    ttl->node->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;

    ca_div_end(); /* drag zone */

    /* ---- Right: window control buttons ---- */
    Ca_Node *ctrl = (Ca_Node *)ca_div_begin(&(Ca_DivDesc){
        .height = TITLE_BAR_HEIGHT_PX,
        .style = "ca-titlebar-controls",
    });
    ctrl->dirty |= CA_DIRTY_LAYOUT;

    Ca_Button *min_btn = ca_btn_begin(&(Ca_BtnDesc){
        .text       = CA_ICON_FA_MINUS,
        .width      = 0.0f,
        .height     = 0.0f,
        .text_color = 0,
        .style      = "ca-titlebar-control",
        .on_click   = on_minimize_click,
        .click_data = win,
    });
    min_btn->node->dirty |= CA_DIRTY_CONTENT;
    ca_btn_end(); /* min btn */

    Ca_Button *max_btn = ca_btn_begin(&(Ca_BtnDesc){
        .text       = win->titlebar_maximized
                          ? CA_ICON_FA_WINDOW_RESTORE
                          : CA_ICON_FA_WINDOW_MAXIMIZE,
        .width      = 0.0f,
        .height     = 0.0f,
        .text_color = 0,
        .style      = "ca-titlebar-control",
        .on_click   = on_maximize_click,
        .click_data = win,
    });
    max_btn->node->dirty |= CA_DIRTY_CONTENT;
    ca_btn_end(); /* max btn */

    Ca_Button *cls_btn = ca_btn_begin(&(Ca_BtnDesc){
        .text       = CA_ICON_FA_TIMES,
        .width      = 0.0f,
        .height     = 0.0f,
        .text_color = 0,
        .style      = "ca-titlebar-control ca-titlebar-close",
        .on_click   = on_close_click,
        .click_data = win,
    });
    cls_btn->node->dirty |= CA_DIRTY_CONTENT;
    ca_btn_end(); /* close btn */

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
