// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* title_bar.c — Custom window title bar for Causality windows.
 *
 * Provides drag-to-move, minimize, maximize/restore, close buttons, and
 * an optional left-aligned menu bar embedded in the title bar strip.
 *
 * Architecture:
 *   win->root         (vertical flex, fills window, system-managed)
 *   ├── win->title_bar_node  (horizontal, `.ca-titlebar` CSS height, 26 px default)
 *   │   ├── ca_menu_bar(...)     (left-aligned menus, if any)
 *   │   ├── drag div             (flex-grow:1, drag-to-move, title text)
 *   │   └── controls div        (min / max / close buttons)
 *   └── win->content_root  (flex-grow: 1, holds user content)
 */

#include "title_bar.h"
#include "menu_storage.h"
#include "node.h"
#include "style.h"
#include "widget.h"
#include "../core/ca_internal.h"
#include "../../include/causality.h"

#include <GLFW/glfw3.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define TITLE_BAR_DEFAULT_HEIGHT_PX 26.0f
#define TITLE_BAR_SIDE_PADDING_PX 8.0f

/* Apply layered system and author styles to a system-owned node. */
static void apply_system_style(Ca_Node *node, Ca_ElementType type,
                               const char *classes)
{
    if (!node || !node->window || !node->window->instance || !classes) return;
    if (node->has_base_desc)
        node->desc = node->base_desc;

    Ca_Instance *instance = node->window->instance;
    if (!instance->system_stylesheet && !instance->stylesheet) return;

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

/* Restyle the title bar strip. Its logical height comes from the
   `.ca-titlebar` height declaration, falling back to the default. */
static void title_bar_apply_style(Ca_Window *win)
{
    Ca_Node *tb = win->title_bar_node;
    const float sc = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    apply_system_style(tb, CA_ELEM_DIV, "ca-titlebar");
    const float height = (tb->desc.height > 0.0f && !tb->desc.height_pct)
                             ? tb->desc.height
                             : TITLE_BAR_DEFAULT_HEIGHT_PX;
    tb->desc.height        = height * sc;
    tb->desc.height_pct    = false;
    tb->desc.padding_left  = TITLE_BAR_SIDE_PADDING_PX * sc;
    tb->desc.padding_right = TITLE_BAR_SIDE_PADDING_PX * sc;
    tb->dirty |= CA_DIRTY_LAYOUT;
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
        ca_window_restore(win);
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

    /* ---- Title bar node: horizontal strip; height and side insets are
       resolved (and scaled) by title_bar_apply_style ---- */
    Ca_NodeDesc tb = {0};
    tb.direction     = CA_HORIZONTAL;
    tb.align_items   = CA_ALIGN_CENTER;
    tb.overflow_x    = 1; /* hidden */
    tb.overflow_y    = 1;
    Ca_Node *tbnode = ca_node_add(root, &tb);
    assert(tbnode && "ca_title_bar_init: failed to allocate title_bar_node");
    win->title_bar_node = tbnode;
    tbnode->base_desc = tb;
    tbnode->has_base_desc = true;
    title_bar_apply_style(win);

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

    title_bar_apply_style(win);

    /* ---- Left: optional menu bar ---- */
    if (win->titlebar_menu_count > 0) {
        size_t total_items = 0;
        size_t total_sub_items = 0;
        for (int m = 0; m < win->titlebar_menu_count; ++m) {
            Ca_MenuBarMenu *menu = &win->titlebar_menus[m];
            if ((size_t)menu->item_count > SIZE_MAX - total_items)
                goto titlebar_menu_done;
            total_items += (size_t)menu->item_count;
            for (int i = 0; i < menu->item_count; ++i) {
                if ((size_t)menu->items[i].sub_item_count >
                    SIZE_MAX - total_sub_items)
                    goto titlebar_menu_done;
                total_sub_items += (size_t)menu->items[i].sub_item_count;
            }
        }
        Ca_DynArray menu_desc_storage = CA_DYN_ARRAY_INIT(Ca_MenuDesc);
        Ca_DynArray item_desc_storage = CA_DYN_ARRAY_INIT(Ca_MenuItemDesc);
        Ca_DynArray sub_desc_storage = CA_DYN_ARRAY_INIT(Ca_MenuItemDesc);
        if (!ca_dyn_array_resize(&menu_desc_storage,
                                 (size_t)win->titlebar_menu_count) ||
            !ca_dyn_array_resize(&item_desc_storage, total_items) ||
            !ca_dyn_array_resize(&sub_desc_storage, total_sub_items)) {
            ca_dyn_array_destroy(&sub_desc_storage);
            ca_dyn_array_destroy(&item_desc_storage);
            ca_dyn_array_destroy(&menu_desc_storage);
            goto titlebar_menu_done;
        }
        Ca_MenuDesc *menu_descs = menu_desc_storage.data;
        Ca_MenuItemDesc *item_descs = item_desc_storage.data;
        Ca_MenuItemDesc *sub_descs = sub_desc_storage.data;
        size_t item_offset = 0;
        size_t sub_offset = 0;

        for (int m = 0; m < win->titlebar_menu_count; m++) {
            Ca_MenuBarMenu *mbm = &win->titlebar_menus[m];
            for (int i = 0; i < mbm->item_count; i++) {
                Ca_MenuBarItem *mbi = &mbm->items[i];
                for (int s = 0; s < mbi->sub_item_count; s++) {
                    sub_descs[sub_offset + (size_t)s] = (Ca_MenuItemDesc){
                        .label       = mbi->sub_items[s].label,
                        .action      = mbi->sub_items[s].action,
                        .action_data = mbi->sub_items[s].action_data,
                    };
                }
                item_descs[item_offset + (size_t)i] = (Ca_MenuItemDesc){
                    .label          = mbi->label,
                    .action         = mbi->action,
                    .action_data    = mbi->action_data,
                    .separator      = mbi->separator,
                    .sub_items      = mbi->sub_item_count > 0
                                          ? &sub_descs[sub_offset] : NULL,
                    .sub_item_count = mbi->sub_item_count,
                };
                sub_offset += (size_t)mbi->sub_item_count;
            }
            menu_descs[m] = (Ca_MenuDesc){
                .label      = mbm->label,
                .items      = &item_descs[item_offset],
                .item_count = mbm->item_count,
            };
            item_offset += (size_t)mbm->item_count;
        }

        ca_menu_bar(&(Ca_MenuBarDesc){
            .menus            = menu_descs,
            .menu_count       = win->titlebar_menu_count,
            .style            = "ca-titlebar-menu",
            .item_style       = "ca-titlebar-menu-item",
        });
        ca_dyn_array_destroy(&sub_desc_storage);
        ca_dyn_array_destroy(&item_desc_storage);
        ca_dyn_array_destroy(&menu_desc_storage);
titlebar_menu_done:
        ;
    }

    /* ---- Centre: drag zone (invisible, handles window dragging) ---- */
    Ca_Node *drag = (Ca_Node *)ca_div_begin(&(Ca_DivDesc){
        .style         = "ca-titlebar-drag",
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
    win->title_drag_node = drag;
    win->title_text_node = ttl->node;

    ca_div_end(); /* drag zone */

    /* ---- Right: window control buttons ---- */
    Ca_Node *ctrl = (Ca_Node *)ca_div_begin(&(Ca_DivDesc){
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
        .skip_keyboard_focus = true,
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
        .skip_keyboard_focus = true,
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
        .skip_keyboard_focus = true,
    });
    cls_btn->node->dirty |= CA_DIRTY_CONTENT;
    ca_btn_end(); /* close btn */

    ca_div_end(); /* controls */

    ca_div_end(); /* title_bar_node */
}

/* Shift a laid-out subtree horizontally by `dx`. */
static void translate_subtree_x(Ca_Node *node, float dx)
{
    node->x += dx;
    for (uint32_t i = 0; i < node->child_count; ++i)
        translate_subtree_x(node->children[i], dx);
}

void ca_title_bar_center_title(Ca_Window *win)
{
    Ca_Node *bar  = win->title_bar_node;
    Ca_Node *drag = win->title_drag_node;
    Ca_Node *text = win->title_text_node;
    if (!bar || !drag || !text || !drag->in_use || !text->in_use ||
        text->parent != drag || drag->parent != bar ||
        drag->desc.hidden || text->desc.hidden)
        return;

    const float lo = drag->x + drag->desc.padding_left;
    const float hi = drag->x + drag->w - drag->desc.padding_right - text->w;
    if (hi < lo) return;

    float x = bar->x + (bar->w - text->w) * 0.5f;
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    if (x != text->x) translate_subtree_x(text, x - text->x);
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
    if (count < 0 || !menus) count = 0;

    Ca_DynArray storage = { 0 };
    Ca_MenuBarMenu *copy = NULL;
    if (!ca_menu_storage_resize(&storage, &copy, (size_t)count)) return;

    for (int m = 0; m < count; m++) {
        Ca_MenuBarMenu    *dst = &copy[m];
        const Ca_MenuDesc *src = &menus[m];

        snprintf(dst->label, sizeof(dst->label), "%s", src->label ? src->label : "");
        dst->active_sub = -1;
        int item_count = src->item_count > 0 && src->items ? src->item_count : 0;
        if (!ca_menu_item_storage_resize(dst, (size_t)item_count)) goto failed;

        for (int i = 0; i < item_count; i++) {
            Ca_MenuBarItem        *ditem = &dst->items[i];
            const Ca_MenuItemDesc *sitem = &src->items[i];

            snprintf(ditem->label, sizeof(ditem->label), "%s", sitem->label ? sitem->label : "");
            ditem->action      = sitem->action;
            ditem->action_data = sitem->action_data;
            ditem->separator   = sitem->separator;

            int nsub = sitem->sub_item_count > 0 && sitem->sub_items
                ? sitem->sub_item_count : 0;
            if (!ca_menu_sub_item_storage_resize(ditem, (size_t)nsub)) goto failed;

            for (int k = 0; k < nsub; k++) {
                Ca_MenuBarSubItem     *dsub = &ditem->sub_items[k];
                const Ca_MenuItemDesc *ssub = &sitem->sub_items[k];
                snprintf(dsub->label, sizeof(dsub->label), "%s", ssub->label ? ssub->label : "");
                dsub->action      = ssub->action;
                dsub->action_data = ssub->action_data;
            }
        }
    }

    ca_menu_storage_destroy(&window->titlebar_menu_storage, &window->titlebar_menus);
    window->titlebar_menu_storage = storage;
    window->titlebar_menus        = copy;
    window->titlebar_menu_count   = count;
    window->titlebar_needs_rebuild = true;
    return;

failed:
    ca_menu_storage_destroy(&storage, &copy);
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

    if (!fn) ca_node_clear(sb);
    window->statusbar_needs_rebuild = true;
}

void ca_window_invalidate_status_bar(Ca_Window *window)
{
    if (!window || !window->in_use) {
        return;
    }
    window->statusbar_needs_rebuild = true;
}
