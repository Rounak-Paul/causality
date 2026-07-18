// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* widget.c — HTML-like declarative UI elements
 *
 * Every element can nest children, just like HTML.
 * An implicit parent stack tracks hierarchy automatically.
 *
 *     ca_ui_begin(window, &(Ca_DivDesc){ ... });
 *       ca_text(&(Ca_TextDesc){ .text = "Hello" });
 *       ca_btn_begin(&(Ca_BtnDesc){ .on_click = fn });
 *         ca_text(&(Ca_TextDesc){ .text = "Click me" });
 *       ca_btn_end();
 *       ca_list_begin(NULL);
 *         ca_li_begin(NULL);
 *           ca_text(&(Ca_TextDesc){ .text = "Item 1" });
 *         ca_li_end();
 *       ca_list_end();
 *     ca_ui_end();
 */
#include "widget.h"
#include "node.h"
#include "style.h"
#include "ca_theme.h"
#include "font.h"
#include "scrollbar.h"
#include "../../include/ca_reactive.h"
#include "viewport.h"
#include "../platform/window.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <GLFW/glfw3.h>

static float glyph_adv(Ca_FontTier *tier, uint32_t cp,
                       float cs, float desired_size)
{
    Ca_FontTier *glyph_tier = tier;
    Ca_Glyph *g = ca_font_glyph_from_tier(tier, cp, &glyph_tier);
    if (!g) return 0.0f;
    float cs_eff = ca_font_glyph_cs_eff(glyph_tier, desired_size, cs);
    return g->xadvance / cs_eff;
}

/* Measure the pixel width of a text string using the instance font.
   Returns 0 if no font is available. */
static float measure_text_px(Ca_Window *win, const char *text)
{
    if (!text || text[0] == '\0') return 0.0f;
    Ca_Font *font = win->instance->font;
    if (!font) return 0.0f;
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float cs   = font->content_scale / ui_s;
    Ca_FontTier *tier = ca_font_tier(font, font->default_size * ui_s);
    float w = 0.0f;
    const char *p = text;
    while (*p) {
        uint32_t cp = ca_utf8_decode(&p);
        w += glyph_adv(tier, cp, cs, font->default_size);
    }
    return w;
}

/* Public variant of measure_text_px: same logic but lets the caller
   pick the font size, so widget code that renders text at a non-default
   point size (e.g. a 12 px editor buffer when the UI default is 14) can
   still get an accurate pixel width.

   Icons and text use the same dynamic font page, so each glyph computes its
   advance from the tier that actually supplied it. */
float ca_measure_text_px(Ca_Window *win, const char *text, float font_size)
{
    if (!win || !text || text[0] == '\0') return 0.0f;
    Ca_Font *font = win->instance->font;
    if (!font) return 0.0f;
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float cs   = font->content_scale / ui_s;
    float desired = font_size > 0.0f ? font_size : font->default_size;
    Ca_FontTier *tier = ca_font_tier(font, desired * ui_s);
    float w = 0.0f;
    const char *p = text;
    while (*p) {
        uint32_t cp = ca_utf8_decode(&p);
        w += glyph_adv(tier, cp, cs, desired);
    }
    return w;
}

bool ca_button_get_click_pos(const Ca_Button *button,
                             float *out_x, float *out_y)
{
    if (!button || !button->last_click_valid) return false;
    if (out_x) *out_x = button->last_click_x;
    if (out_y) *out_y = button->last_click_y;
    return true;
}

bool ca_font_line_metrics(Ca_Window *win, float font_size,
                          float *out_ascent, float *out_descent)
{
    if (!win) return false;
    Ca_Font *font = win->instance->font;
    if (!font) return false;
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float desired = font_size > 0.0f ? font_size : font->default_size;
    /* Select the best atlas tier for the visual size, return metrics in
       layout space (CSS px * ui_scale) so callers can use them directly
       as Ca_DivDesc positional values. */
    Ca_FontTier *tier = ca_font_tier(font, desired * ui_s);
    if (!tier) return false;
    float font_scale = desired / tier->logical_px;
    if (out_ascent)  *out_ascent  = tier->ascent  * font_scale * ui_s;
    if (out_descent) *out_descent = tier->descent * font_scale * ui_s;
    return true;
}

/* ============================================================
   INTERNAL — convert descriptors to Ca_NodeDesc (scaled)
   ============================================================ */

static Ca_Direction dir_from_int(int direction)
{
    return (direction == CA_VERTICAL) ? CA_DIR_COLUMN : CA_DIR_ROW;
}

/* Forward-declared; defined after g_ctx. */
static Ca_NodeDesc div_to_nd(const Ca_DivDesc *d);
static float s(float v);

/* ============================================================
   INTERNAL — pool allocators (label + button only)
   ============================================================ */

static Ca_Label *alloc_label(Ca_Window *win)
{
    for (uint32_t i = 0; i < CA_MAX_LABELS_PER_WINDOW; ++i) {
        if (!win->label_pool[i].in_use)
            return &win->label_pool[i];
    }
    fprintf(stderr, "[causality] label pool exhausted (max %d)\n", CA_MAX_LABELS_PER_WINDOW);
    return NULL;
}

static Ca_Button *alloc_button(Ca_Window *win)
{
    for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i) {
        if (!win->button_pool[i].in_use)
            return &win->button_pool[i];
    }
    fprintf(stderr, "[causality] button pool exhausted (max %d)\n", CA_MAX_BUTTONS_PER_WINDOW);
    return NULL;
}

static Ca_TextInput *alloc_input(Ca_Window *win)
{
    for (uint32_t i = 0; i < CA_MAX_INPUTS_PER_WINDOW; ++i) {
        if (!win->input_pool[i].in_use)
            return &win->input_pool[i];
    }
    fprintf(stderr, "[causality] input pool exhausted (max %d)\n", CA_MAX_INPUTS_PER_WINDOW);
    return NULL;
}

#define ALLOC_POOL_FN(name, type, pool, max_const)              \
static type *alloc_##name(Ca_Window *win) {                     \
    for (uint32_t i = 0; i < max_const; ++i) {                 \
        if (!win->pool[i].in_use) return &win->pool[i];        \
    }                                                           \
    fprintf(stderr, "[causality] " #name " pool exhausted\n"); \
    return NULL;                                                \
}
ALLOC_POOL_FN(checkbox,  Ca_Checkbox,  checkbox_pool,  CA_MAX_CHECKBOXES_PER_WINDOW)
ALLOC_POOL_FN(radio,     Ca_Radio,     radio_pool,     CA_MAX_RADIOS_PER_WINDOW)
ALLOC_POOL_FN(slider,    Ca_Slider,    slider_pool,    CA_MAX_SLIDERS_PER_WINDOW)
ALLOC_POOL_FN(toggle,    Ca_Toggle,    toggle_pool,    CA_MAX_TOGGLES_PER_WINDOW)
ALLOC_POOL_FN(progress,  Ca_Progress,  progress_pool,  CA_MAX_PROGRESS_PER_WINDOW)
ALLOC_POOL_FN(select,    Ca_Select,    select_pool,    CA_MAX_SELECTS_PER_WINDOW)
ALLOC_POOL_FN(tabbar,    Ca_TabBar,    tabbar_pool,    CA_MAX_TABBARS_PER_WINDOW)
ALLOC_POOL_FN(treenode,  Ca_TreeNode,  treenode_pool,  CA_MAX_TREENODES_PER_WINDOW)
ALLOC_POOL_FN(table,     Ca_Table,     table_pool,     CA_MAX_TABLES_PER_WINDOW)
ALLOC_POOL_FN(tooltip,   Ca_Tooltip,   tooltip_pool,   CA_MAX_TOOLTIPS_PER_WINDOW)
ALLOC_POOL_FN(ctxmenu,   Ca_CtxMenu,   ctxmenu_pool,   CA_MAX_CTXMENUS_PER_WINDOW)
ALLOC_POOL_FN(modal,     Ca_Modal,     modal_pool,     CA_MAX_MODALS_PER_WINDOW)
ALLOC_POOL_FN(splitter,  Ca_Splitter,  splitter_pool,  CA_MAX_SPLITTERS_PER_WINDOW)
ALLOC_POOL_FN(menubar,   Ca_MenuBar,   menubar_pool,   CA_MAX_MENUBARS_PER_WINDOW)

/* ============================================================
   INTERNAL — reactive field helpers

   Every widget that uses claim_child MUST update its stored value via these
   macros instead of bare assignments.  They compare the incoming value against
   whatever is already stored: if it changed AND the node was reused, they mark
   CA_DIRTY_CONTENT so the paint pass re-renders that node automatically.

   This is the single, consistent pattern used by every built-in widget.  When
   adding a new custom widget that calls claim_child, follow the same pattern.

   WIDGET_SET_TEXT  – for const char * / char-buffer text fields (uses strcmp)
   WIDGET_SET       – for scalar fields (bool, int, float, uint32_t …)
   ============================================================ */

/* Compare-write-dirty for a char-buffer text field. */
#define WIDGET_SET_TEXT(node, reused, buf, bufsize, new_text)                  \
    do {                                                                        \
        const char *_wt = (new_text) ? (new_text) : "";                        \
        if (!(reused) || strcmp((buf), _wt) != 0) {                            \
            snprintf((buf), (bufsize), "%s", _wt);                             \
            if (reused) {                                                       \
                (node)->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;           \
                if ((node)->parent)                                             \
                    (node)->parent->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT; \
            }                                                                   \
        }                                                                       \
    } while (0)

/* Compare-write-dirty for any scalar field. */
#define WIDGET_SET(node, reused, field, new_val)                               \
    do {                                                                        \
        if (!(reused) || (field) != (new_val)) {                               \
            (field) = (new_val);                                                \
            if (reused) (node)->dirty |= CA_DIRTY_CONTENT;                     \
        }                                                                       \
    } while (0)

/* ============================================================
   INTERNAL — node creation helpers
   ============================================================ */

/* Create a container node (used by div, span, ul, ol, li, btn_begin) */
static Ca_Node *add_container(Ca_Node *parent, const Ca_NodeDesc *nd)
{
    return ca_node_add(parent, nd);
}

/* Create a label under a parent node */
static Ca_Label *add_label(Ca_Window *win, Ca_Node *parent, const Ca_TextDesc *desc)
{
    Ca_Label *slot = alloc_label(win);
    if (!slot) return NULL;

    Ca_NodeDesc nd = {0};
    nd.width     = s(desc->width);
    nd.height    = s(desc->height);  /* 0 if not user-set; CSS or default fills later */
    nd.text_wrap = desc->wrap ? 1 : 0;

    Ca_Node *node = ca_node_add(parent, &nd);
    if (!node) return NULL;

    slot->node   = node;
    slot->in_use = true;
    slot->color  = desc->color;
    node->widget_type = CA_WIDGET_LABEL;
    node->widget      = slot;
    if (desc->text)
        snprintf(slot->text, CA_LABEL_TEXT_MAX, "%s", desc->text);
    return slot;
}

/* Create a button under a parent node */
static Ca_Button *add_button(Ca_Window *win, Ca_Node *parent, const Ca_BtnDesc *desc)
{
    Ca_Button *slot = alloc_button(win);
    if (!slot) return NULL;

    Ca_NodeDesc nd   = {0};
    nd.width         = s(desc->width);   /* 0 if not user-set; CSS or default fills later */
    nd.height        = s(desc->height);
    nd.background    = desc->background;
    nd.corner_radius = s(desc->corner_radius);
    nd.padding_top   = s(desc->padding[0]);
    nd.padding_right = s(desc->padding[1]);
    nd.padding_bottom= s(desc->padding[2]);
    nd.padding_left  = s(desc->padding[3]);
    nd.gap           = s(desc->gap);
    nd.direction     = dir_from_int(desc->direction);

    Ca_Node *node = ca_node_add(parent, &nd);
    if (!node) return NULL;

    slot->node       = node;
    slot->in_use     = true;
    slot->text_color = desc->text_color;
    node->widget_type = CA_WIDGET_BUTTON;
    node->widget      = slot;
    if (desc->text)
        snprintf(slot->text, CA_BUTTON_TEXT_MAX, "%s", desc->text);
    if (desc->on_click) {
        slot->on_click   = desc->on_click;
        slot->click_data = desc->click_data;
    }
    return slot;
}

/* ============================================================
   IMPLICIT PARENT STACK — stores Ca_Node* directly
   ============================================================ */

#define CA_STACK_MAX 64

typedef struct Ca_BuildContext {
    Ca_Window *window;
    Ca_Node   *stack[CA_STACK_MAX];
    uint16_t   child_cursor[CA_STACK_MAX];
    bool       reconcile[CA_STACK_MAX];
    char       next_key[CA_NODE_ID_MAX];
    int        depth;    /* index of top; -1 = empty */
    bool       active;
    bool       auto_ctx; /* true when ca_div_clear auto-entered the context */
} Ca_BuildContext;

static Ca_BuildContext g_ctx;

/* Pending pre-CSS desc snapshot: set in claim_child/ca_ui_begin just before
   node->desc is overwritten with the (sparse, pre-CSS) new descriptor.
   Consumed inside apply_css to diff old CSS-resolved vs new CSS-resolved desc
   and set dirty flags only when something actually changed. */
static Ca_NodeDesc  s_pre_css_desc;
static Ca_Node     *s_pre_css_node;

static Ca_Node *ctx_top(void)
{
    assert(g_ctx.active && g_ctx.depth >= 0);
    return g_ctx.stack[g_ctx.depth];
}

static bool ctx_top_reconcile(void)
{
    return g_ctx.active && g_ctx.depth >= 0 && g_ctx.reconcile[g_ctx.depth];
}

static uint16_t *ctx_top_cursor(void)
{
    assert(g_ctx.active && g_ctx.depth >= 0);
    return &g_ctx.child_cursor[g_ctx.depth];
}

static void ctx_push(Ca_Node *node)
{
    assert(g_ctx.depth + 1 < CA_STACK_MAX);
    ++g_ctx.depth;
    g_ctx.stack[g_ctx.depth] = node;
    g_ctx.child_cursor[g_ctx.depth] = 0;
    g_ctx.reconcile[g_ctx.depth] = false;
}

static void ctx_push_mode(Ca_Node *node, bool reconcile)
{
    assert(g_ctx.depth + 1 < CA_STACK_MAX);
    ++g_ctx.depth;
    g_ctx.stack[g_ctx.depth] = node;
    g_ctx.child_cursor[g_ctx.depth] = 0;
    g_ctx.reconcile[g_ctx.depth] = reconcile;
}

static void ctx_pop(void)
{
    assert(g_ctx.depth >= 0);
    if (g_ctx.reconcile[g_ctx.depth]) {
        Ca_Node *node = g_ctx.stack[g_ctx.depth];
        ca_node_trim_children(node, g_ctx.child_cursor[g_ctx.depth]);
    }
    --g_ctx.depth;
}

static const char *consume_next_key(void)
{
    if (!g_ctx.next_key[0]) return NULL;
    static char key_buf[CA_NODE_ID_MAX];
    snprintf(key_buf, sizeof(key_buf), "%s", g_ctx.next_key);
    g_ctx.next_key[0] = '\0';
    return key_buf;
}

static void reorder_child_to_cursor(Ca_Node *parent, uint32_t from, uint32_t to)
{
    if (from == to || from >= parent->child_count || to >= parent->child_count) return;
    Ca_Node *picked = parent->children[from];
    if (from > to) {
        for (uint32_t i = from; i > to; --i)
            parent->children[i] = parent->children[i - 1];
        parent->children[to] = picked;
    }
}

static bool node_key_eq(const Ca_Node *node, const char *key)
{
    if (!key || !key[0]) return false;
    return node->id[0] && strcmp(node->id, key) == 0;
}

static bool node_kind_match(const Ca_Node *node, uint8_t widget_type,
                            Ca_ElementType elem_type)
{
    if (widget_type != CA_WIDGET_NONE && node->widget_type != widget_type)
        return false;
    /* CA_ELEM_DIV == 0, so the old "!= 0" shortcut incorrectly skipped
       the elem_type check for divs, letting a SPLITTER node be claimed
       in place of a div when the split layout is replaced by a plain
       buffer area.  Always check elem_type — no caller uses 0 as a
       "don't care" sentinel. */
    if (node->elem_type != (uint8_t)elem_type)
        return false;
    return true;
}

static Ca_Node *claim_child(const Ca_NodeDesc *nd, uint8_t widget_type,
                            Ca_ElementType elem_type, const char *key,
                            bool *out_reused)
{
    Ca_Node *parent = ctx_top();
    uint16_t *cursor = ctx_top_cursor();
    uint32_t idx = *cursor;
    Ca_Node *node = NULL;
    *out_reused = false;

    if (!ctx_top_reconcile()) {
        node = ca_node_add(parent, nd);
        if (!node) return NULL;
        *cursor = (uint16_t)(*cursor + 1);
        return node;
    }

    if (idx < parent->child_count) {
        Ca_Node *cand = parent->children[idx];
        bool key_ok = key ? node_key_eq(cand, key) : !cand->id[0];
        if (key_ok && node_kind_match(cand, widget_type, elem_type))
            node = cand;
    }

    if (!node && key && key[0]) {
        for (uint32_t i = idx + 1; i < parent->child_count; ++i) {
            Ca_Node *cand = parent->children[i];
            if (!node_key_eq(cand, key)) continue;
            if (!node_kind_match(cand, widget_type, elem_type)) continue;
            reorder_child_to_cursor(parent, i, idx);
            node = parent->children[idx];
            break;
        }
    }

    if (node) {
        /* Snapshot the old CSS-resolved desc BEFORE overwriting with the sparse
           incoming nd.  apply_css will compare post-CSS nd against this snapshot
           and set dirty flags only when visuals or layout actually changed. */
        s_pre_css_desc = node->desc;
        s_pre_css_node = node;
        node->desc     = *nd;   /* bare assign – no dirty, CSS hasn't run yet */
        *out_reused = true;
    } else {
        node = ca_node_add(parent, nd);
        if (!node) return NULL;
        reorder_child_to_cursor(parent, parent->child_count - 1, idx);
    }

    *cursor = (uint16_t)(*cursor + 1);
    return node;
}

/* Scale a value by the window's UI scale factor */
static float s(float v) { return v * g_ctx.window->ui_scale; }

/* Scale every pixel-valued resolved CSS field for a target window. */
static void scale_resolved_style(Ca_ResolvedStyle *style, float scale)
{
    if (!style) return;
    if (!style->width_pct) style->width *= scale;
    if (!style->height_pct) style->height *= scale;
    style->min_width *= scale; style->max_width *= scale;
    style->min_height *= scale; style->max_height *= scale;
    for (size_t i = 0u; i < 4u; ++i) {
        style->padding[i] *= scale;
        style->margin[i] *= scale;
    }
    style->gap *= scale;
    style->row_gap *= scale;
    style->column_gap *= scale;
    style->border_radius *= scale;
    style->border_radius_tl *= scale;
    style->border_radius_tr *= scale;
    style->border_radius_br *= scale;
    style->border_radius_bl *= scale;
    style->border_width *= scale;
    style->border_top_w *= scale;
    style->border_right_w *= scale;
    style->border_bottom_w *= scale;
    style->border_left_w *= scale;
    style->outline_width *= scale;
    style->outline_offset *= scale;
    style->shadow_offset_x *= scale;
    style->shadow_offset_y *= scale;
    style->shadow_blur *= scale;
    style->font_size *= scale;
    style->line_height *= scale;
    style->letter_spacing *= scale;
    style->word_spacing *= scale;
    style->flex_basis *= scale;
    style->scrollbar_width *= scale;
    style->scrollbar_radius *= scale;
}

/* Store a resolved foreground color in widgets that own text paint state. */
static void apply_widget_text_color(Ca_Node *node, uint32_t color)
{
    if (!node) return;
    if (color != 0u && node->parent &&
        node->parent->widget_type == CA_WIDGET_MENUBAR) {
        for (uint32_t i = 0; i < node->child_count; ++i) {
            Ca_Node *child = node->children[i];
            if (child && child->widget_type == CA_WIDGET_LABEL && child->widget)
                ((Ca_Label *)child->widget)->color = color;
        }
    }
    if (!node->widget) return;
    if (node->widget_type == CA_WIDGET_SPLITTER)
        ((Ca_Splitter *)node->widget)->bar_color = node->desc.background;
    if (color == 0u) return;
    switch (node->widget_type) {
    case CA_WIDGET_LABEL: ((Ca_Label *)node->widget)->color = color; break;
    case CA_WIDGET_BUTTON: ((Ca_Button *)node->widget)->text_color = color; break;
    case CA_WIDGET_TEXT_INPUT: ((Ca_TextInput *)node->widget)->text_color = color; break;
    case CA_WIDGET_CHECKBOX: ((Ca_Checkbox *)node->widget)->text_color = color; break;
    case CA_WIDGET_RADIO: ((Ca_Radio *)node->widget)->text_color = color; break;
    case CA_WIDGET_TREENODE: ((Ca_TreeNode *)node->widget)->text_color = color; break;
    case CA_WIDGET_PROGRESS: ((Ca_Progress *)node->widget)->bar_color = color; break;
    case CA_WIDGET_SPLITTER: ((Ca_Splitter *)node->widget)->bar_hover_color = color; break;
    default: break;
    }
}

/* Resolve CSS styles and apply to a node descriptor + set node metadata.
   Inline (nonzero) descriptor values take precedence over CSS. */
static void apply_css(Ca_Node *node, Ca_NodeDesc *nd,
                      Ca_ElementType elem_type, const char *classes,
                      const char *id, uint32_t *out_color)
{
    node->elem_type = (uint8_t)elem_type;
    if (classes)
        snprintf(node->classes, CA_NODE_CLASS_MAX, "%s", classes);
    else
        node->classes[0] = '\0';
    if (id)
        snprintf(node->id, CA_NODE_ID_MAX, "%s", id);
    else
        node->id[0] = '\0';

    /* Snapshot the fully-formed sparse pre-CSS desc.  Widgets routinely
       mutate node->desc between claim_child and this call (setting
       hidden / disabled / position / etc.), so this is the only place
       where we know the COMPLETE pre-CSS desc.  ca_widget_reapply_css
       uses this snapshot to re-resolve CSS in-place on pseudo-state
       changes without losing those widget-specific writes. */
    node->base_desc     = *nd;
    node->has_base_desc = true;

    Ca_Instance *instance = g_ctx.window->instance;
    if (!instance->system_stylesheet && !instance->stylesheet) return;

    Ca_ResolvedStyle rs;
    ca_style_resolve_layers(instance->system_stylesheet, instance->stylesheet,
                            node, elem_type, node->classes, &rs);

    scale_resolved_style(&rs, g_ctx.window->ui_scale);

    ca_style_apply_to_node(&rs, nd, out_color);
    apply_widget_text_color(node, out_color ? *out_color : 0u);

    /* Store transition config on the node */
    node->transition_duration = rs.transition_duration;
    node->transition_props    = rs.transition_props;

    /* Post-CSS dirty detection: compare the fully-resolved *nd against the
       snapshot saved before claim_child/ca_ui_begin wrote the sparse desc.
       This fires only when a node was reused (s_pre_css_node != NULL).
       New nodes are always dirty via ca_node_add, so no extra check needed. */
    if (s_pre_css_node == node) {
        if (content_desc_changed(&s_pre_css_desc, nd)) node->dirty |= CA_DIRTY_CONTENT;
        if (layout_desc_changed(&s_pre_css_desc, nd))  node->dirty |= CA_DIRTY_LAYOUT;
        s_pre_css_node = NULL;
    }
}

void ca_widget_reapply_css(Ca_Node *node)
{
    if (!node || !node->in_use || !node->window) return;
    if (!node->has_base_desc) return; /* never ran apply_css — nothing to reapply */
    Ca_Instance *instance = node->window->instance;
    if (!instance || (!instance->system_stylesheet && !instance->stylesheet)) return;

    Ca_NodeDesc old = node->desc;

    /* Conservative reset: only zero out the *visual* fields that pseudo-state
       CSS rules (:hover / :focus / :active) commonly drive.  Layout fields
       (width / height / padding / margin / font_size / direction / overflow /
       position / flex_*) are LEFT ALONE because:
         - apply_css's zero-field-write semantics mean these are already
           composited from inline + CSS, and
         - several call sites (title_bar.c, ca_node_graph.c, etc.) mutate
           layout fields on node->desc AFTER apply_css ran; those writes
           are not tracked in base_desc and must be preserved.
       Pseudo-state layout changes (e.g. :hover { padding: 8px } when base
       had padding: 4px) are NOT supported here — they'd require subtree
       relayout anyway. That tradeoff is acceptable for a pure-visual hover
       feedback path. */
    Ca_NodeDesc *nd = &node->desc;
    const Ca_NodeDesc *bd = &node->base_desc;
    nd->background      = bd->background;
    nd->border_color    = bd->border_color;
    nd->border_width    = bd->border_width;
    nd->corner_radius   = bd->corner_radius;
    nd->shadow_color    = bd->shadow_color;
    nd->shadow_blur     = bd->shadow_blur;
    nd->shadow_offset_x = bd->shadow_offset_x;
    nd->shadow_offset_y = bd->shadow_offset_y;
    nd->opacity         = bd->opacity;

    Ca_ResolvedStyle rs;
    ca_style_resolve_layers(instance->system_stylesheet, instance->stylesheet,
                            node, (Ca_ElementType)node->elem_type,
                            node->classes, &rs);

    /* Scale CSS-resolved pixel values (matches apply_css). Use the node's
       own window scale directly since g_ctx may not be active here. */
    scale_resolved_style(&rs, node->window->ui_scale);

    /* ca_style_apply_to_node uses zero-fill semantics — it writes layout
       fields (width/height/padding/margin/gap) whenever the current value
       is <= 0.  Several widgets (notably ca_tree_node_begin) deliberately
       zero out layout fields AFTER initial apply_css to override CSS-
       driven sizing (e.g. lifting CSS height onto a child header so the
       container auto-sizes).  On reapply we must preserve those post-CSS
       layout writes; otherwise pseudo-state CSS reapplication (triggered
       e.g. by hover) will resurrect the CSS height and collapse the
       container back to its CSS size, causing siblings to overlap. */
    float saved_w    = nd->width,   saved_h    = nd->height;
    uint8_t saved_wp = nd->width_pct, saved_hp = nd->height_pct;
    float saved_minw = nd->min_w, saved_maxw = nd->max_w;
    float saved_minh = nd->min_h, saved_maxh = nd->max_h;
    float saved_pt = nd->padding_top,    saved_pr = nd->padding_right;
    float saved_pb = nd->padding_bottom, saved_pl = nd->padding_left;
    float saved_mt = nd->margin_top,     saved_mr = nd->margin_right;
    float saved_mb = nd->margin_bottom,  saved_ml = nd->margin_left;
    float saved_gap = nd->gap, saved_rg = nd->row_gap, saved_cg = nd->column_gap;

    uint32_t out_color = 0;
    ca_style_apply_to_node(&rs, nd, &out_color);

    nd->width = saved_w;    nd->height = saved_h;
    nd->width_pct = saved_wp; nd->height_pct = saved_hp;
    nd->min_w = saved_minw; nd->max_w = saved_maxw;
    nd->min_h = saved_minh; nd->max_h = saved_maxh;
    nd->padding_top = saved_pt;    nd->padding_right  = saved_pr;
    nd->padding_bottom = saved_pb; nd->padding_left   = saved_pl;
    nd->margin_top = saved_mt;     nd->margin_right   = saved_mr;
    nd->margin_bottom = saved_mb;  nd->margin_left    = saved_ml;
    nd->gap = saved_gap; nd->row_gap = saved_rg; nd->column_gap = saved_cg;

    /* Propagate text color for widget types that store it separately. */
    apply_widget_text_color(node, out_color);

    /* Diff against pre-reapply desc — only dirty when CSS resolution
       actually changed a visual field, so a hover-cross between siblings
       doesn't dirty unrelated ancestors. */
    if (content_desc_changed(&old, nd)) node->dirty |= CA_DIRTY_CONTENT;
    /* Note: we intentionally do NOT check layout_desc_changed here because
       this code path doesn't touch layout fields. */
}

void ca_widget_refresh_css(Ca_Node *node)
{
    if (!node || !node->in_use || !node->window || !node->has_base_desc) return;
    Ca_Instance *instance = node->window->instance;
    if (!instance || (!instance->system_stylesheet && !instance->stylesheet)) return;

    const Ca_NodeDesc old = node->desc;
    node->desc = node->base_desc;

    Ca_ResolvedStyle resolved;
    ca_style_resolve_layers(instance->system_stylesheet, instance->stylesheet,
                            node, (Ca_ElementType)node->elem_type,
                            node->classes, &resolved);
    scale_resolved_style(&resolved, node->window->ui_scale);

    uint32_t text_color = 0u;
    ca_style_apply_to_node(&resolved, &node->desc, &text_color);
    apply_widget_text_color(node, text_color);
    node->transition_duration = resolved.transition_duration;
    node->transition_props = resolved.transition_props;

    node->desc.hidden = old.hidden;
    node->desc.disabled = old.disabled;
    if (node->widget_type == CA_WIDGET_TREENODE)
        node->desc.height = old.height;

    const float scale = node->window->ui_scale;
    if (node->widget_type == CA_WIDGET_LABEL && node->desc.height <= 0.0f &&
        !node->desc.text_wrap) {
        float default_height = 16.0f;
        if (node->elem_type == CA_ELEM_H1) default_height = 36.0f;
        else if (node->elem_type == CA_ELEM_H2) default_height = 28.0f;
        else if (node->elem_type == CA_ELEM_H3) default_height = 24.0f;
        else if (node->elem_type == CA_ELEM_H4) default_height = 20.0f;
        else if (node->elem_type == CA_ELEM_H5 || node->elem_type == CA_ELEM_H6)
            default_height = 18.0f;
        node->desc.height = default_height * scale +
                            node->desc.padding_top + node->desc.padding_bottom;
    }
    if (node->widget_type == CA_WIDGET_TEXT_INPUT) {
        if (node->desc.width <= 0.0f) node->desc.width = 160.0f * scale;
        if (node->desc.height <= 0.0f) node->desc.height = 24.0f * scale;
        if (node->desc.padding_left <= 0.0f) node->desc.padding_left = 4.0f * scale;
        if (node->desc.padding_right <= 0.0f) node->desc.padding_right = 4.0f * scale;
    }
    if (node->elem_type == CA_ELEM_HR && node->desc.height <= 0.0f)
        node->desc.height = scale;

    if (content_desc_changed(&old, &node->desc)) node->dirty |= CA_DIRTY_CONTENT;
    if (layout_desc_changed(&old, &node->desc)) node->dirty |= CA_DIRTY_LAYOUT;
}

static Ca_NodeDesc div_to_nd(const Ca_DivDesc *d)
{
    Ca_NodeDesc nd = {0};
    if (!d) return nd;
    nd.width          = s(d->width);
    nd.height         = s(d->height);
    nd.padding_top    = s(d->padding[0]);
    nd.padding_right  = s(d->padding[1]);
    nd.padding_bottom = s(d->padding[2]);
    nd.padding_left   = s(d->padding[3]);
    nd.gap            = s(d->gap);
    nd.direction      = dir_from_int(d->direction);
    nd.background     = d->background;
    nd.corner_radius  = s(d->corner_radius);
    nd.position       = (uint8_t)d->position;
    nd.pos_x          = s(d->pos_x);
    nd.pos_y          = s(d->pos_y);
    nd.border_width   = s(d->border_width);
    nd.border_color   = d->border_color;
    nd.shadow_offset_x = d->shadow_offset_x;
    nd.shadow_offset_y = d->shadow_offset_y;
    nd.shadow_blur    = d->shadow_blur;
    nd.shadow_color   = d->shadow_color;
    nd.z_index        = (int16_t)d->z_index;
    nd.hidden         = d->hidden;
    nd.disabled       = d->disabled;
    nd.no_hover       = d->no_hover;
    return nd;
}

/* ============================================================
   PUBLIC — tree root
   ============================================================ */

void ca_ui_begin(Ca_Window *window, const Ca_DivDesc *root_desc)
{
    assert(window);
    assert(!g_ctx.active && "ca_ui_begin called without matching ca_ui_end");

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.window = window;
    g_ctx.depth  = -1;
    g_ctx.active = true;
    g_ctx.next_key[0] = '\0';

    Ca_NodeDesc nd = div_to_nd(root_desc);
    Ca_Node *root  = window->content_root;
    assert(root && "ca_ui_begin: content_root not initialised (ca_ui_window_init not called?)");

    /* Apply same pending-desc pattern as claim_child so root is only dirtied
       when its CSS-resolved desc actually changes between frames. */
    s_pre_css_desc = root->desc;
    s_pre_css_node = root;
    root->desc     = nd;

    /* Apply CSS to root */
    uint32_t dummy_color = 0;
    apply_css(root, &root->desc, CA_ELEM_DIV,
              root_desc ? root_desc->style : NULL,
              root_desc ? root_desc->id : NULL, &dummy_color);

    /* The window's content_root is a system-managed flex slot between
       the title bar and (optional) status bar. User-supplied width/
       height/percent values would hijack the strip layout, so force
       flex-grow=1 along the window's main axis and clear any explicit
       main-axis sizing the user (or their CSS) just installed. The
       cross axis is left untouched so width:100% / stretch still works. */
    root->desc.flex_grow  = 1.0f;
    root->desc.height     = 0.0f;
    root->desc.height_pct = false;

    /* Default: root is vertically scrollable (like HTML body).
       Users can override via CSS (e.g. overflow: hidden). */
    if (root->desc.overflow_y == 0)
        root->desc.overflow_y = 2; /* scroll */

    ctx_push(root);

}

void ca_ui_end(void)
{
    assert(g_ctx.active && "ca_ui_end called without ca_ui_begin");
    assert(g_ctx.depth == 0 && "mismatched begin / end calls");
    g_ctx.active = false;
    g_ctx.depth  = -1;
}

/* ============================================================
   INTERNAL — build context enter / leave (called by ui.c)
   ============================================================ */

void ca_widget_ctx_enter(Ca_Window *win)
{
    assert(win);
    g_ctx.window = win;
    g_ctx.depth  = -1;
    g_ctx.active = true;
    g_ctx.next_key[0] = '\0';
}

void ca_widget_ctx_leave(void)
{
    assert(g_ctx.active);
    assert(g_ctx.depth == -1 && "unclosed ca_div_begin / ca_div_clear");
    g_ctx.active = false;
}

void ca_reconcile_key(const char *key)
{
    assert(g_ctx.active);
    if (!key || !key[0]) {
        g_ctx.next_key[0] = '\0';
        return;
    }
    snprintf(g_ctx.next_key, sizeof(g_ctx.next_key), "%s", key);
}

void ca_reconcile_begin(Ca_Div *div)
{
    assert(g_ctx.active);
    assert(div);
    ctx_push_mode((Ca_Node *)div, true);
}

/* ============================================================
   PUBLIC — <div>
   ============================================================ */

Ca_Div *ca_div_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : (desc ? desc->id : NULL);
    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_NONE, CA_ELEM_DIV, id, &reused);
    assert(node);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_DIV,
              desc ? desc->style : NULL,
              id, &dummy);

    /* Store drag callbacks on the node */
    if (desc) {
        node->drag_fn_start = (void *)desc->on_drag_start;
        node->drag_fn_move  = (void *)desc->on_drag;
        node->drag_fn_end   = (void *)desc->on_drag_end;
        node->drag_data     = desc->drag_data;
        node->scroll_fn     = (void *)desc->on_scroll;
        node->scroll_data   = desc->scroll_data;
    }

    ctx_push_mode(node, ctx_top_reconcile());
    return (Ca_Div *)node;
}

void ca_div_end(void)
{
    assert(g_ctx.active && g_ctx.depth >= 0);
    ctx_pop();

    /* Auto-leave when a ca_div_clear-initiated context pops back to root. */
    if (g_ctx.auto_ctx && g_ctx.depth == -1) {
        g_ctx.active   = false;
        g_ctx.auto_ctx = false;
    }
}

void ca_btn_end(void)        { assert(g_ctx.active && g_ctx.depth > 0); ctx_pop(); }
void ca_list_end(void)       { assert(g_ctx.active && g_ctx.depth > 0); ctx_pop(); }
void ca_li_end(void)         { assert(g_ctx.active && g_ctx.depth > 0); ctx_pop(); }
void ca_tree_end(void)       { assert(g_ctx.active && g_ctx.depth > 0); ctx_pop(); }
void ca_table_end(void)      { assert(g_ctx.active && g_ctx.depth > 0); ctx_pop(); }
void ca_table_row_end(void)  { assert(g_ctx.active && g_ctx.depth > 0); ctx_pop(); }
void ca_modal_end(void)      { assert(g_ctx.active && g_ctx.depth > 0); ctx_pop(); }
void ca_split_end(void)      { assert(g_ctx.active && g_ctx.depth > 0); ctx_pop(); }

void ca_tree_node_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    /* If a collapsed tree node is being closed, hide all children except
       the header row. */
    Ca_Node *node = ctx_top();
    if (node && node->widget_type == CA_WIDGET_TREENODE) {
        Ca_TreeNode *tn = (Ca_TreeNode *)node->widget;
        if (tn && !tn->expanded) {
            for (uint32_t i = 1; i < node->child_count; ++i)
                node->children[i]->desc.hidden = true;
        }
    }
    ctx_pop();
}

/* ============================================================
   PUBLIC — <p> / <text>  (self-closing leaf)
   ============================================================ */

Ca_Label *ca_text(const Ca_TextDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.width     = s(desc->width);
    nd.height    = s(desc->height);
    nd.text_wrap = desc->wrap ? 1 : 0;

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_LABEL, CA_ELEM_TEXT, id, &reused);
    if (!node) return NULL;

    Ca_Label *lbl = NULL;
    if (reused && node->widget_type == CA_WIDGET_LABEL && node->widget)
        lbl = (Ca_Label *)node->widget;
    if (!lbl) {
        lbl = alloc_label(g_ctx.window);
        if (!lbl) return NULL;
        memset(lbl, 0, sizeof(*lbl));
        lbl->in_use = true;
        node->widget_type = CA_WIDGET_LABEL;
        node->widget = lbl;
    }

    lbl->node = node;
    lbl->in_use = true;
    WIDGET_SET_TEXT(node, reused, lbl->text, CA_LABEL_TEXT_MAX, desc->text);

    if (lbl && lbl->node) {
        if (desc->hidden) lbl->node->desc.hidden = true;
        /* Snapshot old color, reset to 0 so CSS is always re-resolved from
           the current classes, then dirty the node if the color changed.
           lbl->color is NOT part of node->desc so content_desc_changed() never
           catches color-only changes — we must do it manually here. */
        uint32_t old_color = lbl->color;
        lbl->color = 0;
        apply_css(lbl->node, &lbl->node->desc, CA_ELEM_TEXT,
                  desc->style, id, &lbl->color);
        /* Inline color overrides CSS — lets callers set per-instance colors. */
        if (desc->color) lbl->color = desc->color;
        if (reused && lbl->color != old_color)
            lbl->node->dirty |= CA_DIRTY_CONTENT;
        /* Default height if neither user nor CSS set it.
           Skip for wrapped labels — their height is computed at layout time
           from the actual wrapped line count. */
        if (lbl->node->desc.height <= 0.0f && !lbl->node->desc.text_wrap) {
            float pad_v = lbl->node->desc.padding_top
                        + lbl->node->desc.padding_bottom;
            lbl->node->desc.height = s(16.0f) + pad_v;
        }
    }
    return lbl;
}

/* ============================================================
   PUBLIC — <button>  (nestable; close with ca_btn_end)
   ============================================================ */

/* Nestable: ca_btn pushes the button onto the stack so children
   (text, icons, other elements) are laid out inside the button rect. */
Ca_Button *ca_btn_begin(const Ca_BtnDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd   = {0};
    nd.width         = s(desc->width);
    nd.height        = s(desc->height);
    nd.background    = desc->background;
    nd.corner_radius = s(desc->corner_radius);
    nd.padding_top   = s(desc->padding[0]);
    nd.padding_right = s(desc->padding[1]);
    nd.padding_bottom= s(desc->padding[2]);
    nd.padding_left  = s(desc->padding[3]);
    nd.gap           = s(desc->gap);
    nd.direction     = dir_from_int(desc->direction);
    nd.text_align    = 1; /* center, matching HTML <button> user-agent default */

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_BUTTON, CA_ELEM_BUTTON, id, &reused);
    assert(node);

    Ca_Button *btn = NULL;
    if (reused && node->widget_type == CA_WIDGET_BUTTON && node->widget)
        btn = (Ca_Button *)node->widget;
    if (!btn) {
        btn = alloc_button(g_ctx.window);
        if (!btn) return NULL;
        memset(btn, 0, sizeof(*btn));
        btn->in_use = true;
        node->widget_type = CA_WIDGET_BUTTON;
        node->widget = btn;
    }

    btn->node = node;
    btn->in_use = true;
    WIDGET_SET_TEXT(node, reused, btn->text, CA_BUTTON_TEXT_MAX, desc->text);
    btn->text_color = desc->text_color;
    btn->on_click = desc->on_click;
    btn->click_data = desc->click_data;
    btn->keyboard_focusable = !desc->skip_keyboard_focus;

    if (desc->hidden)   btn->node->desc.hidden   = true;
    if (desc->disabled) btn->node->desc.disabled = true;
    apply_css(btn->node, &btn->node->desc, CA_ELEM_BUTTON,
              desc->style, id, &btn->text_color);
    /* Nestable buttons auto-size from children; only apply fallback
       if no CSS sets the dimension either. */
    ctx_push_mode(btn->node, ctx_top_reconcile());
    return btn;
}


/* ============================================================
   PUBLIC — <list>  (vertical container, default gap 4)
   ============================================================ */

void ca_list_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    nd.direction = CA_DIR_COLUMN;
    if (!desc || nd.gap <= 0.0f) nd.gap = s(2.0f);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : (desc ? desc->id : NULL);
    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_NONE, CA_ELEM_LIST, id, &reused);
    assert(node);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_LIST,
              desc ? desc->style : NULL,
              id, &dummy);

    ctx_push_mode(node, ctx_top_reconcile());
}


/* ============================================================
   PUBLIC — <li>  (list item — horizontal container, default gap 8)
   ============================================================ */

void ca_li_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    nd.direction = CA_DIR_ROW;
    if (!desc || nd.gap <= 0.0f) nd.gap = s(4.0f);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : (desc ? desc->id : NULL);
    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_NONE, CA_ELEM_LI, id, &reused);
    assert(node);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_LI,
              desc ? desc->style : NULL,
              id, &dummy);

    ctx_push_mode(node, ctx_top_reconcile());
}


/* ============================================================
   PUBLIC — <hr>  (horizontal rule — self-closing)
   ============================================================ */

void ca_hr(const Ca_HrDesc *desc)
{
    assert(g_ctx.active);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : (desc ? desc->id : NULL);
    Ca_NodeDesc nd = {0};
    /* Only set user-specified values; leave zeros for CSS to fill */
    if (desc && desc->thickness > 0.0f) nd.height = s(desc->thickness);
    if (desc && desc->color) nd.background = desc->color;
    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_NONE, CA_ELEM_HR, id, &reused);
    if (node) {
        uint32_t dummy = 0;
        apply_css(node, &node->desc, CA_ELEM_HR,
                  desc ? desc->style : NULL,
                  id, &dummy);
        /* Defaults after CSS */
        if (node->desc.height <= 0.0f) node->desc.height = s(1.0f);
        if (node->desc.background == 0)
            node->desc.background = CA_THEME_BG_SURFACE;
    }
}

/* ============================================================
   PUBLIC — spacer  (invisible spacing element)
   ============================================================ */

void ca_spacer(const Ca_SpacerDesc *desc)
{
    assert(g_ctx.active);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : (desc ? desc->id : NULL);
    Ca_NodeDesc nd = {0};
    if (desc) {
        nd.width  = s(desc->width);
        nd.height = s(desc->height);
    }
    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_NONE, CA_ELEM_SPACER, id, &reused);
    if (node) {
        uint32_t dummy = 0;
        apply_css(node, &node->desc, CA_ELEM_SPACER,
                  desc ? desc->style : NULL,
                  id, &dummy);
    }
}

/* ============================================================
   PUBLIC — <input>  (text input field — self-closing leaf)
   ============================================================ */

Ca_TextInput *ca_input(const Ca_InputDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.width          = s(desc->width);
    nd.height         = s(desc->height);
    nd.background     = desc->background;
    nd.corner_radius  = s(desc->corner_radius);
    nd.padding_top    = s(desc->padding[0]);
    nd.padding_right  = s(desc->padding[1]);
    nd.padding_bottom = s(desc->padding[2]);
    nd.padding_left   = s(desc->padding[3]);

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_TEXT_INPUT, CA_ELEM_INPUT, id, &reused);
    if (!node) return NULL;

    Ca_TextInput *inp = NULL;
    if (reused && node->widget_type == CA_WIDGET_TEXT_INPUT && node->widget)
        inp = (Ca_TextInput *)node->widget;
    if (!inp) {
        inp = alloc_input(g_ctx.window);
        if (!inp) return NULL;
        memset(inp, 0, sizeof(*inp));
        inp->in_use = true;
        node->widget_type = CA_WIDGET_TEXT_INPUT;
        node->widget = inp;
    }

    inp->node       = node;
    inp->in_use     = true;
    inp->text_color = desc->text_color;
    inp->placeholder_color = CA_THEME_TEXT_DIM;
    inp->input_mode = desc->input_mode;
    inp->drag_speed = desc->drag_speed > 0.0f
        ? desc->drag_speed
        : (desc->input_mode == CA_INPUT_FLOAT ? 0.1f : 1.0f);
    WIDGET_SET_TEXT(node, reused, inp->text, CA_INPUT_TEXT_MAX, desc->text);

    if (desc->placeholder)
        snprintf(inp->placeholder, CA_INPUT_TEXT_MAX, "%s", desc->placeholder);
    else
        inp->placeholder[0] = '\0';

    /* Only reset cursor/selection on first creation, not on every rebuild.
     * Resetting each frame would snap the cursor to end-of-text every tick,
     * making it impossible to position the cursor or see it blink. */
    if (!reused) {
        inp->cursor    = (int)strlen(inp->text);
        inp->sel_start = -1;
    }

    if (desc->on_change) {
        inp->on_change   = desc->on_change;
        inp->change_data = desc->change_data;
    }

    if (desc->hidden)   node->desc.hidden   = true;
    if (desc->disabled) node->desc.disabled = true;

    apply_css(node, &node->desc, CA_ELEM_INPUT,
              desc->style, id, &inp->text_color);

    /* Default size if neither user nor CSS set it */
    if (node->desc.width  <= 0.0f) node->desc.width  = s(160.0f);
    if (node->desc.height <= 0.0f) node->desc.height = s(24.0f);
    /* Default padding if none */
    if (node->desc.padding_left <= 0.0f)  node->desc.padding_left  = s(4.0f);
    if (node->desc.padding_right <= 0.0f) node->desc.padding_right = s(4.0f);

    return inp;
}

/* ============================================================
   PUBLIC — headings  (convenience wrappers around ca_text)
   ============================================================ */

static Ca_Label *heading(const Ca_TextDesc *desc, float default_height,
                         float default_font_size, Ca_ElementType elem_type)
{
    assert(g_ctx.active && desc);
    Ca_Label *lbl = add_label(g_ctx.window, ctx_top(), desc);
    if (lbl && lbl->node) {
        apply_css(lbl->node, &lbl->node->desc, elem_type,
                  desc->style, desc->id, &lbl->color);
        /* Default heading height if neither user nor CSS set it */
        if (lbl->node->desc.height <= 0.0f)
            lbl->node->desc.height = s(default_height);
        /* Default heading font size if neither user nor CSS set it */
        if (lbl->node->desc.font_size <= 0.0f)
            lbl->node->desc.font_size = default_font_size;
    }
    return lbl;
}

Ca_Label *ca_h1(const Ca_TextDesc *desc) { return heading(desc, 36.0f, 28.0f, CA_ELEM_H1); }
Ca_Label *ca_h2(const Ca_TextDesc *desc) { return heading(desc, 28.0f, 22.0f, CA_ELEM_H2); }
Ca_Label *ca_h3(const Ca_TextDesc *desc) { return heading(desc, 24.0f, 18.0f, CA_ELEM_H3); }
Ca_Label *ca_h4(const Ca_TextDesc *desc) { return heading(desc, 20.0f, 16.0f, CA_ELEM_H4); }
Ca_Label *ca_h5(const Ca_TextDesc *desc) { return heading(desc, 18.0f, 14.0f, CA_ELEM_H5); }
Ca_Label *ca_h6(const Ca_TextDesc *desc) { return heading(desc, 16.0f, 12.0f, CA_ELEM_H6); }

/* ============================================================
   PUBLIC — runtime setters
   ============================================================ */

/* Helper: start a transition on a node if it has transition config for the prop */
static void maybe_transition(Ca_Node *node, Ca_CssPropId prop,
                             float old_f, float new_f,
                             uint32_t old_color, uint32_t new_color)
{
    if (node->transition_duration <= 0.0f) return;
    if (prop < 64 && !(node->transition_props & (1ULL << prop))) return;

    /* Find or allocate a transition slot */
    Ca_Transition *slot = NULL;
    for (int i = 0; i < CA_MAX_TRANSITIONS_PER_NODE; ++i) {
        if (node->transitions[i].active && node->transitions[i].prop == (uint8_t)prop) {
            slot = &node->transitions[i]; break;
        }
    }
    if (!slot) {
        for (int i = 0; i < CA_MAX_TRANSITIONS_PER_NODE; ++i) {
            if (!node->transitions[i].active) { slot = &node->transitions[i]; break; }
        }
    }
    if (!slot) return;

    slot->prop       = (uint8_t)prop;
    slot->active     = true;
    slot->from_f     = old_f;
    slot->to_f       = new_f;
    slot->from_color = old_color;
    slot->to_color   = new_color;
    slot->start_time = glfwGetTime();
    slot->duration   = node->transition_duration;

    /* A transition can begin during the input pass, after ca_ui_update has
       already scanned active transitions for this frame. Wake an idle event
       loop now so the next tick observes and advances the new transition. */
    glfwPostEmptyEvent();
}

/* Old per-widget setters removed — see unified ca__set_text / ca__set_color /
   ca__set_background / ca__get_text below. */

/* ============================================================
   RUNTIME SETTERS — unified ca_set_style / ca_set_hidden / ca_set_disabled
   ============================================================ */

/* Internal helpers */

/* Re-resolve CSS classes on a node at runtime.
   out_text_color is optional — pass the widget's text_color field address
   for widgets that carry their own text colour (button, checkbox, etc.),
   or NULL for widgets that don't (div, slider, progress, etc.). */
static void node_set_style(Ca_Node *node, const char *style,
                            uint32_t *out_text_color)
{
    const char *new_classes = style ? style : "";
    if (strcmp(node->classes, new_classes) == 0) return;

    /* Snapshot pre-CSS desc for dirty detection */
    Ca_NodeDesc old_desc = node->desc;

    /* Reset desc so CSS values aren't blocked by stale inline values.
       Preserve hidden/disabled which are orthogonal to CSS classes. */
    bool was_hidden   = node->desc.hidden;
    bool was_disabled = node->desc.disabled;
    memset(&node->desc, 0, sizeof(node->desc));
    node->desc.hidden   = was_hidden;
    node->desc.disabled = was_disabled;

    /* Reset text color so CSS can re-apply it */
    uint32_t old_text_color = out_text_color ? *out_text_color : 0;
    if (out_text_color) *out_text_color = 0;

    /* Update classes and re-resolve CSS */
    snprintf(node->classes, CA_NODE_CLASS_MAX, "%s", new_classes);

    Ca_Instance *instance = node->window->instance;
    if (instance && (instance->system_stylesheet || instance->stylesheet)) {
        Ca_ResolvedStyle rs;
        float scale = node->window->ui_scale;
        ca_style_resolve_layers(instance->system_stylesheet, instance->stylesheet,
                                node, (Ca_ElementType)node->elem_type,
                                node->classes, &rs);
        /* Scale CSS pixel values */
        if (!rs.width_pct)  rs.width  *= scale;
        if (!rs.height_pct) rs.height *= scale;
        rs.min_width         *= scale;  rs.max_width    *= scale;
        rs.min_height        *= scale;  rs.max_height   *= scale;
        rs.padding[0]        *= scale;  rs.padding[1]   *= scale;
        rs.padding[2]        *= scale;  rs.padding[3]   *= scale;
        rs.margin[0]         *= scale;  rs.margin[1]    *= scale;
        rs.margin[2]         *= scale;  rs.margin[3]    *= scale;
        rs.gap               *= scale;
        rs.row_gap           *= scale;
        rs.column_gap        *= scale;
        rs.border_radius     *= scale;
        rs.border_radius_tl  *= scale;
        rs.border_radius_tr  *= scale;
        rs.border_radius_br  *= scale;
        rs.border_radius_bl  *= scale;
        rs.border_width      *= scale;
        rs.border_top_w      *= scale;
        rs.border_right_w    *= scale;
        rs.border_bottom_w   *= scale;
        rs.border_left_w     *= scale;
        rs.outline_width     *= scale;
        rs.outline_offset    *= scale;
        rs.shadow_offset_x   *= scale;
        rs.shadow_offset_y   *= scale;
        rs.shadow_blur       *= scale;
        rs.font_size         *= scale;
        rs.line_height       *= scale;
        rs.letter_spacing    *= scale;
        rs.word_spacing      *= scale;
        rs.flex_basis        *= scale;

        uint32_t dummy_color = 0;
        ca_style_apply_to_node(&rs, &node->desc,
                               out_text_color ? out_text_color : &dummy_color);

        node->transition_duration = rs.transition_duration;
        node->transition_props    = rs.transition_props;

        if ((rs.set_mask & (1ULL << CA_CSS_PROP_DISPLAY)) &&
            rs.display == CA_CSS_DISPLAY_NONE)
            node->desc.hidden = true;
    }

    /* Dirty detection against old state */
    if (content_desc_changed(&old_desc, &node->desc))
        node->dirty |= CA_DIRTY_CONTENT;
    if (layout_desc_changed(&old_desc, &node->desc))
        node->dirty |= CA_DIRTY_LAYOUT;

    /* Text color changed — mark content dirty for repaint */
    if (out_text_color && *out_text_color != old_text_color)
        node->dirty |= CA_DIRTY_CONTENT;

    /* Fire transitions for animated properties */
    if (old_desc.background != node->desc.background)
        maybe_transition(node, CA_CSS_PROP_BACKGROUND_COLOR,
                         0, 0, old_desc.background, node->desc.background);
}

static void node_set_hidden(Ca_Node *node, bool hidden)
{
    if (node->desc.hidden != hidden) {
        node->desc.hidden = hidden;
        node->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
        if (node->parent)
            node->parent->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
    }
}

static void node_set_disabled(Ca_Node *node, bool disabled)
{
    if (node->desc.disabled != disabled) {
        node->desc.disabled = disabled;
        node->dirty |= CA_DIRTY_CONTENT;
    }
}

/* Resolve the text_color field pointer from the node's widget back-pointer.
   Returns NULL for widget types that don't carry a text colour. */
static uint32_t *resolve_text_color(Ca_Node *node)
{
    if (!node->widget) return NULL;
    switch (node->widget_type) {
    case CA_WIDGET_LABEL:      return &((Ca_Label *)node->widget)->color;
    case CA_WIDGET_BUTTON:     return &((Ca_Button *)node->widget)->text_color;
    case CA_WIDGET_TEXT_INPUT:  return &((Ca_TextInput *)node->widget)->text_color;
    case CA_WIDGET_CHECKBOX:   return &((Ca_Checkbox *)node->widget)->text_color;
    case CA_WIDGET_RADIO:      return &((Ca_Radio *)node->widget)->text_color;
    case CA_WIDGET_TREENODE:   return &((Ca_TreeNode *)node->widget)->text_color;
    case CA_WIDGET_MENUBAR:    return &((Ca_MenuBar *)node->widget)->text_color;
    default:                   return NULL;
    }
}

/* ---- Unified public API (backing functions for _Generic macros) ---- */

void ca__set_style_node(Ca_Div *div, const char *style)
{
    Ca_Node *n = (Ca_Node *)div;
    assert(n);
    node_set_style(n, style, resolve_text_color(n));
}

void ca__set_style_widget(void *widget, const char *style)
{
    Ca_Node *n = *(Ca_Node **)widget;
    assert(n);
    node_set_style(n, style, resolve_text_color(n));
}

void ca__set_hidden_node(Ca_Div *div, bool hidden)
{
    assert(div);
    node_set_hidden((Ca_Node *)div, hidden);
}

void ca__set_hidden_widget(void *widget, bool hidden)
{
    Ca_Node *n = *(Ca_Node **)widget;
    assert(n);
    node_set_hidden(n, hidden);
}

void ca__set_disabled_node(Ca_Div *div, bool disabled)
{
    assert(div);
    node_set_disabled((Ca_Node *)div, disabled);
}

void ca__set_disabled_widget(void *widget, bool disabled)
{
    Ca_Node *n = *(Ca_Node **)widget;
    assert(n);
    node_set_disabled(n, disabled);
}

const char *ca_input_text(const Ca_TextInput *input)
{
    return input ? input->text : "";
}

/* ---- Focus query ---- */

bool ca_input_is_focused(const Ca_TextInput *input)
{
    if (!input || !input->node) return false;
    Ca_Window *win = input->node->window;
    return win && win->focused_node == input->node;
}

void ca_input_focus(Ca_TextInput *input)
{
    if (!input || !input->node) return;
    Ca_Window *win = input->node->window;
    if (!win) return;
    Ca_Node *old = win->focused_node;
    win->focused_node = input->node;
    if (old && old != input->node) old->dirty |= CA_DIRTY_CONTENT;
    input->node->dirty |= CA_DIRTY_CONTENT;
    /* Select all text for convenient overwrite */
    input->sel_start = 0;
    input->cursor    = (int)strlen(input->text);
}

/* Returns true if the given key was pressed (or held) this frame on the
   window that owns this input.  Useful for callers that want to react to
   Enter / Escape without needing access to Ca_Window internals. */
bool ca_input_key_pressed(const Ca_TextInput *input, Ca_Key key)
{
    if (!input || !input->node) return false;
    Ca_Window *win = input->node->window;
    if (!win) return false;
    for (uint32_t i = 0; i < win->key_count; ++i) {
        if (win->key_buf[i] == (int)key && win->key_action_buf[i] != 0)
            return true;
    }
    return false;
}

/* ---- Unified ca_set_text / ca_get_text ---- */

void ca__set_text(void *widget, const char *text)
{
    Ca_Node *n = *(Ca_Node **)widget;
    assert(n);
    if (!text) text = "";
    switch (n->widget_type) {
    case CA_WIDGET_LABEL: {
        Ca_Label *lbl = (Ca_Label *)widget;
        const char *cur = lbl->dyn_text ? lbl->dyn_text : lbl->text;
        if (strcmp(cur, text) == 0) return;
        size_t len = strlen(text);
        if (len < CA_LABEL_TEXT_MAX) {
            memcpy(lbl->text, text, len + 1);
            CA_FREE(lbl->dyn_text);
            lbl->dyn_text = NULL;
        } else {
            char *buf = (char *)CA_REALLOC(lbl->dyn_text, len + 1);
            if (buf) { memcpy(buf, text, len + 1); lbl->dyn_text = buf; }
        }
        n->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        if (n->parent)
            n->parent->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        break;
    }
    case CA_WIDGET_BUTTON: {
        Ca_Button *btn = (Ca_Button *)widget;
        if (strcmp(btn->text, text) == 0) return;
        snprintf(btn->text, CA_BUTTON_TEXT_MAX, "%s", text);
        n->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        if (n->parent)
            n->parent->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        break;
    }
    case CA_WIDGET_TEXT_INPUT: {
        Ca_TextInput *inp = (Ca_TextInput *)widget;
        if (strcmp(inp->text, text) == 0) return;
        snprintf(inp->text, CA_INPUT_TEXT_MAX, "%s", text);
        inp->cursor = (int)strlen(inp->text);
        inp->sel_start = -1;
        n->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        if (n->parent)
            n->parent->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        break;
    }
    case CA_WIDGET_CHECKBOX: {
        Ca_Checkbox *cb = (Ca_Checkbox *)widget;
        if (strcmp(cb->text, text) == 0) return;
        snprintf(cb->text, CA_LABEL_TEXT_MAX, "%s", text);
        n->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        if (n->parent)
            n->parent->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        break;
    }
    case CA_WIDGET_TREENODE: {
        Ca_TreeNode *tn = (Ca_TreeNode *)widget;
        if (strcmp(tn->text, text) == 0) return;
        snprintf(tn->text, CA_LABEL_TEXT_MAX, "%s", text);
        n->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        if (n->parent)
            n->parent->dirty |= CA_DIRTY_CONTENT | CA_DIRTY_LAYOUT;
        break;
    }
    default:
        assert(!"ca_set_text: widget type does not support text");
        break;
    }
}

const char *ca__get_text(const void *widget)
{
    const Ca_Node *n = *(const Ca_Node *const *)widget;
    assert(n);
    switch (n->widget_type) {
    case CA_WIDGET_LABEL: {
        const Ca_Label *lbl = (const Ca_Label *)widget;
        return lbl->dyn_text ? lbl->dyn_text : lbl->text;
    }
    case CA_WIDGET_BUTTON:
        return ((const Ca_Button *)widget)->text;
    case CA_WIDGET_TEXT_INPUT:
        return ((const Ca_TextInput *)widget)->text;
    case CA_WIDGET_CHECKBOX:
        return ((const Ca_Checkbox *)widget)->text;
    case CA_WIDGET_TREENODE:
        return ((const Ca_TreeNode *)widget)->text;
    default:
        assert(!"ca_get_text: widget type does not support text");
        return "";
    }
}

/* ---- Unified ca_set_color ---- */

void ca__set_color(void *widget, uint32_t color)
{
    Ca_Node *n = *(Ca_Node **)widget;
    assert(n);
    uint32_t *tc = resolve_text_color(n);
    if (!tc) {
        assert(!"ca_set_color: widget type does not have text color");
        return;
    }
    if (*tc != color) {
        *tc = color;
        n->dirty |= CA_DIRTY_CONTENT;
    }
}

/* ---- Unified ca_set_background ---- */

static void node_set_background(Ca_Node *n, uint32_t color)
{
    uint32_t old_color = n->desc.background;
    if (old_color != color) {
        maybe_transition(n, CA_CSS_PROP_BACKGROUND_COLOR,
                         0, 0, old_color, color);
        n->desc.background = color;
        n->dirty |= CA_DIRTY_CONTENT;
    }
}

void ca__set_background_node(Ca_Div *div, uint32_t color)
{
    assert(div);
    node_set_background((Ca_Node *)div, color);
}

void ca_div_set_width(Ca_Div *div, float width)
{
    assert(div);
    Ca_Node *n = (Ca_Node *)div;
    if (n->desc.width != width) {
        n->desc.width = width;
        n->dirty |= CA_DIRTY_LAYOUT;
    }
}

float ca_div_get_layout_width(const Ca_Div *div)
{
    assert(div);
    return ((const Ca_Node *)div)->w;
}

float ca_div_get_layout_height(const Ca_Div *div)
{
    assert(div);
    return ((const Ca_Node *)div)->h;
}

void ca_btn_get_layout_inner_size(const Ca_Button *btn, float *out_w, float *out_h)
{
    assert(btn);
    const Ca_Node *n = btn->node;
    if (!n) {
        if (out_w) *out_w = 0.0f;
        if (out_h) *out_h = 0.0f;
        return;
    }
    if (out_w) {
        float w = n->w - n->desc.padding_left - n->desc.padding_right;
        *out_w = w < 0.0f ? 0.0f : w;
    }
    if (out_h) {
        float h = n->h - n->desc.padding_top - n->desc.padding_bottom;
        *out_h = h < 0.0f ? 0.0f : h;
    }
}

void ca__set_background_widget(void *widget, uint32_t color)
{
    Ca_Node *n = *(Ca_Node **)widget;
    assert(n);
    node_set_background(n, color);
}

void ca_div_clear(Ca_Div *div)
{
    Ca_Node *node = (Ca_Node *)div;
    assert(node);

    /* Auto-enter a build context if not inside ca_ui_begin / ca_ui_end. */
    if (!g_ctx.active) {
        assert(node->window);
        g_ctx.window   = node->window;
        g_ctx.depth    = -1;
        g_ctx.active   = true;
        g_ctx.auto_ctx = true;
        g_ctx.next_key[0] = '\0';
    }

    ca_node_clear(node);
    ctx_push(node);
}

/* Effect body installed by ca_div_set_builder. Re-runs whenever any
   signal the user's builder reads via ca_signal_get changes, or when
   the effect is manually invalidated via ca_effect_invalidate.

   Uses reconcile mode (not ca_div_clear) so that existing keyed
   children are *reused* across re-runs instead of being freed and
   re-allocated. This preserves child Ca_Node pointers, which is
   critical because:
     - win->hovered_node / focused_node / drag_node point into the
       subtree. If the builder is invalidated while a child is hovered
       (e.g. by the chain dirty-walk in ca_ui_update on hover-state
       change), freeing the children would NULL those pointers and
       drop the pseudo-state mid-frame.
     - apply_css runs for every reused child during the rebuild,
       picking up the *current* :hover / :focus / :active state — so
       a builder-driven panel (toolbar, project settings) now responds
       to pseudo-state changes the same way immediate-mode panels do.
     - ctx_pop in reconcile mode auto-trims unused children, so the
       reactive rebuild still drops elements removed by the user's
       builder logic. */
static void div_builder_effect_fn(void *user)
{
    Ca_Node *node = (Ca_Node *)user;
    if (!node || !node->in_use || !node->builder_fn) return;

    /* Signal writes flush effects synchronously, including while another
       window's frame callback owns the global build context. Rebuild in an
       isolated context for this node's window so newly allocated widgets
       enter the same window pools as their nodes, then resume the interrupted
       build exactly where it was. */
    assert(node->window);
    const Ca_BuildContext suspended_ctx = g_ctx;
    const Ca_NodeDesc suspended_pre_css_desc = s_pre_css_desc;
    Ca_Node *const suspended_pre_css_node = s_pre_css_node;

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.window = node->window;
    g_ctx.depth = -1;
    g_ctx.active = true;

    ca_reconcile_begin((Ca_Div *)node);
    node->builder_fn((Ca_Div *)node, node->builder_data);
    ca_div_end();
    assert(g_ctx.depth == -1);

    g_ctx = suspended_ctx;
    s_pre_css_desc = suspended_pre_css_desc;
    s_pre_css_node = suspended_pre_css_node;
}

Ca_Effect *ca_div_set_builder(Ca_Div *div,
                              void (*fn)(Ca_Div *div, void *user_data),
                              void *user_data)
{
    Ca_Node *node = (Ca_Node *)div;
    assert(node && node->in_use && node->window && node->window->instance);
    node->builder_fn   = fn;
    node->builder_data = user_data;
    if (node->builder_effect) {
        ca_effect_destroy(node->builder_effect);
        node->builder_effect = NULL;
    }
    if (!fn) return NULL;
    node->builder_effect =
        ca_effect(node->window->instance, div_builder_effect_fn, node);
    return node->builder_effect;
}

void ca_div_invalidate(Ca_Div *div)
{
    Ca_Node *node = (Ca_Node *)div;
    assert(node && node->in_use);
    if (node->builder_effect)
        ca_effect_invalidate(node->builder_effect);
}

/* ---- Scroll container helpers (look up node by CSS id) ---- */

static Ca_Node *find_node_by_id(Ca_Window *window, const char *id)
{
    if (!window || !id || !window->node_pool) return NULL;
    for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
        Ca_Node *n = &window->node_pool[i];
        if (n->in_use && n->id[0] && strcmp(n->id, id) == 0)
            return n;
    }
    return NULL;
}

/* Mirrors n->scroll_y into its lazily-created scroll_y_signal, if one has
   ever been requested via ca_get_scroll_y_signal. No-op (and no signal
   allocation) for nodes nothing has ever asked to observe reactively —
   the overwhelming majority of scroll containers in a typical UI. */
void ca_node_sync_scroll_y_signal(Ca_Node *n)
{
    if (n && n->scroll_y_signal)
        ca_signal_set_float(n->scroll_y_signal, n->scroll_y);
}

void ca_scroll_to_top(Ca_Window *window, const char *id)
{
    Ca_Node *n = find_node_by_id(window, id);
    if (!n) return;
    n->scroll_y = 0.0f;
    n->dirty |= CA_DIRTY_LAYOUT;
    ca_node_sync_scroll_y_signal(n);
}

void ca_scroll_to_bottom(Ca_Window *window, const char *id)
{
    Ca_Node *n = find_node_by_id(window, id);
    if (!n) return;
    n->scroll_y = ca_scrollbar_max_y(n);
    n->dirty |= CA_DIRTY_LAYOUT;
    ca_node_sync_scroll_y_signal(n);
}

float ca_get_scroll_y(Ca_Window *window, const char *id)
{
    Ca_Node *n = find_node_by_id(window, id);
    return n ? n->scroll_y : 0.0f;
}

void ca_set_scroll_y(Ca_Window *window, const char *id, float y)
{
    Ca_Node *n = find_node_by_id(window, id);
    if (!n) return;
    float max_scroll = ca_scrollbar_max_y(n);
    if (y < 0.0f)          y = 0.0f;
    if (y > max_scroll)    y = max_scroll;
    n->scroll_y    = y;
    n->dirty      |= CA_DIRTY_LAYOUT;
    ca_node_sync_scroll_y_signal(n);
}

Ca_Signal *ca_get_scroll_y_signal(Ca_Window *window, const char *id)
{
    Ca_Node *n = find_node_by_id(window, id);
    if (!n) return NULL;
    if (!n->scroll_y_signal)
        n->scroll_y_signal = ca_signal_float(window->instance, n->scroll_y);
    return n->scroll_y_signal;
}

/*
 * Set the callback invoked on each scheduled frame for a window.
 *
 * window     Target window.
 * fn         Callback to install, or NULL to clear it.
 * user_data  Opaque callback data.
 */
void ca_window_set_on_frame(Ca_Window *window, void (*fn)(void *), void *user_data)
{
    if (!window) return;
    window->on_frame_fn   = fn;
    window->on_frame_data = user_data;
    if (window->instance) ca_instance_wake();
}

/* Old per-widget setters removed — see unified API below. */

/* ============================================================
   PUBLIC — Checkbox
   ============================================================ */

Ca_Checkbox *ca_checkbox(const Ca_CheckboxDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_ROW;
    nd.height = s(20.0f);

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_CHECKBOX, CA_ELEM_CHECKBOX, id, &reused);
    if (!node) return NULL;

    Ca_Checkbox *cb = NULL;
    if (reused && node->widget_type == CA_WIDGET_CHECKBOX && node->widget)
        cb = (Ca_Checkbox *)node->widget;
    if (!cb) {
        cb = alloc_checkbox(g_ctx.window);
        if (!cb) return NULL;
        memset(cb, 0, sizeof(*cb));
        cb->in_use = true;
        node->widget_type = CA_WIDGET_CHECKBOX;
        node->widget = cb;
    }

    cb->node = node;
    cb->in_use = true;
    WIDGET_SET(node, reused, cb->checked, desc->checked);
    cb->text_color = 0; /* default white */
    WIDGET_SET_TEXT(node, reused, cb->text, CA_LABEL_TEXT_MAX, desc->text);
    cb->on_change = desc->on_change;
    cb->change_data = desc->change_data;

    if (desc->hidden)   node->desc.hidden   = true;
    if (desc->disabled) node->desc.disabled = true;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_CHECKBOX, desc->style, id, &cb->text_color);

    /* Auto-width from text */
    if (node->desc.width <= 0.0f) {
        float tw = measure_text_px(g_ctx.window, desc->text);
        node->desc.width = s(20.0f) + s(6.0f) + (tw > 0 ? tw : 0);
    }
    return cb;
}

void ca_checkbox_set(Ca_Checkbox *cb, bool checked)
{
    assert(cb && cb->in_use);
    cb->checked = checked;
    cb->node->dirty |= CA_DIRTY_CONTENT;
}

bool ca_checkbox_get(const Ca_Checkbox *cb)
{
    assert(cb && cb->in_use);
    return cb->checked;
}

/* ============================================================
   PUBLIC — Radio button
   ============================================================ */

Ca_Radio *ca_radio(const Ca_RadioDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Radio *r = alloc_radio(g_ctx.window);
    if (!r) return NULL;

    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_ROW;
    nd.height = s(20.0f);

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    r->node = node;
    r->in_use = true;
    r->group = desc->group;
    r->value = desc->value;
    node->widget_type = CA_WIDGET_RADIO;
    node->widget      = r;
    r->text_color = 0;
    if (desc->text) snprintf(r->text, CA_LABEL_TEXT_MAX, "%s", desc->text);
    else r->text[0] = '\0';
    r->on_change = desc->on_change;
    r->change_data = desc->change_data;

    if (desc->hidden)   node->desc.hidden   = true;
    if (desc->disabled) node->desc.disabled = true;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_RADIO, desc->style, desc->id, &r->text_color);

    if (node->desc.width <= 0.0f) {
        float tw = measure_text_px(g_ctx.window, desc->text);
        node->desc.width = s(20.0f) + s(6.0f) + (tw > 0 ? tw : 0);
    }
    return r;
}

int ca_radio_group_get(Ca_Window *win, int group)
{
    if (!win || !win->radio_pool) return -1;
    for (uint32_t i = 0; i < CA_MAX_RADIOS_PER_WINDOW; ++i) {
        Ca_Radio *r = &win->radio_pool[i];
        if (r->in_use && r->group == group) {
            /* Find the selected one — we check all in paint pass, but return
               the first marked selected during last click pass */
        }
    }
    /* Walk and find the currently-active radio in this group.
       The "selected" state is that exactly one radio per group is checked.
       We store this by checking which radio's node is flagged. For simplicity,
       we track selection per-radio and return the value of the first checked. */
    for (uint32_t i = 0; i < CA_MAX_RADIOS_PER_WINDOW; ++i) {
        Ca_Radio *r = &win->radio_pool[i];
        if (r->in_use && r->group == group && r->value == 1)
            return (int)i;
    }
    return -1;
}

/* ============================================================
   PUBLIC — Slider
   ============================================================ */

Ca_Slider *ca_slider(const Ca_SliderDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.width  = desc->width > 0 ? s(desc->width) : s(160.0f);
    nd.height = s(20.0f);

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_SLIDER, CA_ELEM_SLIDER, id, &reused);
    if (!node) return NULL;

    Ca_Slider *sl = NULL;
    if (reused && node->widget_type == CA_WIDGET_SLIDER && node->widget)
        sl = (Ca_Slider *)node->widget;
    if (!sl) {
        sl = alloc_slider(g_ctx.window);
        if (!sl) return NULL;
        memset(sl, 0, sizeof(*sl));
        sl->in_use = true;
        node->widget_type = CA_WIDGET_SLIDER;
        node->widget = sl;
    }

    sl->node = node;
    sl->in_use = true;
    WIDGET_SET(node, reused, sl->min_val, desc->min);
    WIDGET_SET(node, reused, sl->max_val, desc->max);
    WIDGET_SET(node, reused, sl->value, desc->value);
    sl->on_change = desc->on_change;
    sl->change_data = desc->change_data;

    if (desc->hidden)   node->desc.hidden   = true;
    if (desc->disabled) node->desc.disabled = true;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_SLIDER, desc->style, id, &dummy);
    return sl;
}

void ca_slider_set(Ca_Slider *s, float value)
{
    assert(s && s->in_use);
    if (value < s->min_val) value = s->min_val;
    if (value > s->max_val) value = s->max_val;
    s->value = value;
    s->node->dirty |= CA_DIRTY_CONTENT;
}

float ca_slider_get(const Ca_Slider *s)
{
    assert(s && s->in_use);
    return s->value;
}

/* ============================================================
   PUBLIC — Toggle switch
   ============================================================ */

Ca_Toggle *ca_toggle(const Ca_ToggleDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.width  = s(40.0f);
    nd.height = s(22.0f);

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_TOGGLE, CA_ELEM_TOGGLE, id, &reused);
    if (!node) return NULL;

    Ca_Toggle *t = NULL;
    if (reused && node->widget_type == CA_WIDGET_TOGGLE && node->widget)
        t = (Ca_Toggle *)node->widget;
    if (!t) {
        t = alloc_toggle(g_ctx.window);
        if (!t) return NULL;
        memset(t, 0, sizeof(*t));
        t->in_use = true;
        node->widget_type = CA_WIDGET_TOGGLE;
        node->widget = t;
    }

    t->node = node;
    t->in_use = true;
    WIDGET_SET(node, reused, t->on, desc->on);
    t->on_change = desc->on_change;
    t->change_data = desc->change_data;

    if (desc->hidden)   node->desc.hidden   = true;
    if (desc->disabled) node->desc.disabled = true;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TOGGLE, desc->style, id, &dummy);
    return t;
}

void ca_toggle_set(Ca_Toggle *t, bool on)
{
    assert(t && t->in_use);
    t->on = on;
    t->node->dirty |= CA_DIRTY_CONTENT;
}

bool ca_toggle_get(const Ca_Toggle *t)
{
    assert(t && t->in_use);
    return t->on;
}

/* ============================================================
   PUBLIC — Progress bar
   ============================================================ */

Ca_Progress *ca_progress(const Ca_ProgressDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.width  = desc->width > 0 ? s(desc->width) : s(200.0f);
    nd.height = desc->height > 0 ? s(desc->height) : s(8.0f);

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_PROGRESS, CA_ELEM_PROGRESS, id, &reused);
    if (!node) return NULL;

    Ca_Progress *p = NULL;
    if (reused && node->widget_type == CA_WIDGET_PROGRESS && node->widget)
        p = (Ca_Progress *)node->widget;
    if (!p) {
        p = alloc_progress(g_ctx.window);
        if (!p) return NULL;
        memset(p, 0, sizeof(*p));
        p->in_use = true;
        node->widget_type = CA_WIDGET_PROGRESS;
        node->widget = p;
    }

    p->node = node;
    p->in_use = true;
    WIDGET_SET(node, reused, p->value, desc->value);
    WIDGET_SET(node, reused, p->bar_color,
               desc->bar_color ? desc->bar_color : CA_THEME_ACCENT);

    if (desc->hidden) node->desc.hidden = true;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_PROGRESS, desc->style, id, &dummy);
    return p;
}

void ca_progress_set(Ca_Progress *p, float value)
{
    assert(p && p->in_use);
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    p->value = value;
    p->node->dirty |= CA_DIRTY_CONTENT;
}

void ca_progress_set_color(Ca_Progress *p, uint32_t color)
{
    assert(p && p->in_use);
    if (p->bar_color == color) return;
    p->bar_color = color;
    p->node->dirty |= CA_DIRTY_CONTENT;
}

/* ============================================================
   PUBLIC — Select / Dropdown
   ============================================================ */

Ca_Select *ca_select(const Ca_SelectDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.width  = desc->width > 0 ? s(desc->width) : 0.0f;
    nd.height = 0.0f; /* deferred to CSS; defaults to 26px below if unset */
    nd.corner_radius = s(3.0f);
    nd.background = CA_THEME_BG_BASE;

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_SELECT, CA_ELEM_SELECT, id, &reused);
    if (!node) return NULL;

    Ca_Select *sel = NULL;
    if (reused && node->widget_type == CA_WIDGET_SELECT && node->widget)
        sel = (Ca_Select *)node->widget;
    if (!sel) {
        sel = alloc_select(g_ctx.window);
        if (!sel) return NULL;
        memset(sel, 0, sizeof(*sel));
        sel->in_use = true;
        node->widget_type = CA_WIDGET_SELECT;
        node->widget = sel;
    }

    sel->node = node;
    sel->in_use = true;
    WIDGET_SET(node, reused, sel->selected, desc->selected);
    if (!reused)
        sel->open = false;
    {
        int new_count = desc->option_count;
        if (new_count > CA_MAX_SELECT_OPTIONS) new_count = CA_MAX_SELECT_OPTIONS;
        WIDGET_SET(node, reused, sel->option_count, new_count);
        for (int i = 0; i < sel->option_count; ++i)
            WIDGET_SET_TEXT(node, reused, sel->options[i], CA_OPTION_TEXT_MAX, desc->options[i]);
    }
    sel->on_change  = desc->on_change;
    sel->change_data = desc->change_data;
    sel->on_hover   = desc->on_hover;
    sel->hover_data  = desc->hover_data;
    if (!reused) sel->hover_item = -1;

    if (desc->hidden)   node->desc.hidden   = true;
    if (desc->disabled) node->desc.disabled = true;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_SELECT, desc->style, id, &dummy);

    /* Sensible fallbacks if neither desc nor CSS supplied a size */
    if (node->desc.width  <= 0.0f && !node->desc.width_pct)
        node->desc.width  = s(140.0f);
    if (node->desc.height <= 0.0f && !node->desc.height_pct)
        node->desc.height = s(26.0f);

    return sel;
}

void ca_select_set(Ca_Select *s, int index)
{
    assert(s && s->in_use);
    if (index >= 0 && index < s->option_count) {
        s->selected = index;
        s->node->dirty |= CA_DIRTY_CONTENT;
    }
}

int ca_select_get(const Ca_Select *s)
{
    assert(s && s->in_use);
    return s->selected;
}

int ca_select_get_hover(const Ca_Select *s)
{
    assert(s && s->in_use);
    return s->hover_item;
}

/* ============================================================
   PUBLIC — Tab bar
   ============================================================ */

Ca_TabBar *ca_tabs(const Ca_TabBarDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_ROW;

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_TABBAR, CA_ELEM_TABBAR, id, &reused);
    if (!node) return NULL;

    Ca_TabBar *tb = NULL;
    if (reused && node->widget_type == CA_WIDGET_TABBAR && node->widget)
        tb = (Ca_TabBar *)node->widget;
    if (!tb) {
        tb = alloc_tabbar(g_ctx.window);
        if (!tb) return NULL;
        memset(tb, 0, sizeof(*tb));
        tb->in_use = true;
        node->widget_type = CA_WIDGET_TABBAR;
        node->widget = tb;
    }

    tb->node = node;
    tb->in_use = true;
    WIDGET_SET(node, reused, tb->active, desc->active);
    {
        int new_count = desc->count;
        if (new_count > CA_MAX_TAB_LABELS) new_count = CA_MAX_TAB_LABELS;
        WIDGET_SET(node, reused, tb->count, new_count);
        for (int i = 0; i < tb->count; ++i) {
            WIDGET_SET_TEXT(node, reused, tb->labels[i], CA_OPTION_TEXT_MAX, desc->labels[i]);
            tb->tab_nodes[i] = NULL;
        }
    }
    tb->on_change = desc->on_change;
    tb->change_data = desc->change_data;
    tb->active_bg     = desc->active_bg     ? desc->active_bg     : CA_THEME_BG_OVERLAY;
    tb->inactive_bg   = desc->inactive_bg   ? desc->inactive_bg   : CA_THEME_TRANSPARENT;
    tb->active_text   = desc->active_text   ? desc->active_text   : CA_THEME_ACCENT;
    tb->inactive_text = desc->inactive_text ? desc->inactive_text : CA_THEME_TEXT_DIM;

    if (desc->hidden)   node->desc.hidden   = true;
    if (desc->disabled) node->desc.disabled = true;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TABBAR, desc->style, id, &dummy);

    float item_fs = node->desc.font_size; /* inherit font-size from CSS (e.g. panel-tab-bar) */
    float tab_pad_x = s(desc->tab_padding_x > 0.0f ? desc->tab_padding_x : 8.0f);
    bool tabs_fill = desc->tabs_fill;
    bool tabs_left_align = desc->tabs_left_align;

    ca_node_trim_children(node, 0);

    /* Create child nodes for each tab header */
    for (int i = 0; i < tb->count; ++i) {
        Ca_NodeDesc tnd = {0};
        float tw = ca_measure_text_px(g_ctx.window, tb->labels[i], item_fs);
        /* Add a small safety margin so glyph overhang/subpixel placement
           never clips the final character even when text measurement is tight. */
        float min_w = (tw > 0.0f ? tw : s(40.0f)) + tab_pad_x * 2.0f + s(8.0f);
        if (tabs_fill) {
            tnd.width = 0.0f;
            tnd.min_w = min_w;
            tnd.flex_grow = 1.0f;
            tnd.flex_shrink = 1.0f;
        } else {
            tnd.width = min_w;
        }
        /* height = 0: layout stretches the node to fill the full bar cross-axis so
           the active background covers the entire tab-bar height (no gap). */
        tnd.background = (i == tb->active) ? tb->active_bg : tb->inactive_bg;
        tnd.font_size  = item_fs;
        tnd.padding_left = tab_pad_x;
        tnd.padding_right = tab_pad_x;
        tnd.text_align = tabs_left_align ? 0 : 1;
        tnd.corner_radius = 0.0f;
        Ca_Node *tab_node = ca_node_add(node, &tnd);
        if (tab_node) {
            tab_node->elem_type = CA_ELEM_TAB;
            tab_node->widget_type = CA_WIDGET_TABBAR;
            tab_node->widget      = tb;
            tb->tab_nodes[i] = tab_node;
        }
    }
    return tb;
}

int ca_tabs_active(const Ca_TabBar *t)
{
    assert(t && t->in_use);
    return t->active;
}

/* ============================================================
   PUBLIC — Tree view
   ============================================================ */

void ca_tree_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    nd.direction = CA_DIR_COLUMN;
    if (nd.gap <= 0.0f) nd.gap = s(1.0f);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : (desc ? desc->id : NULL);
    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_NONE, CA_ELEM_TREE, id, &reused);
    assert(node);
    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TREE,
              desc ? desc->style : NULL, id, &dummy);
    ctx_push_mode(node, ctx_top_reconcile());
}


Ca_TreeNode *ca_tree_node_begin(const Ca_TreeNodeDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_COLUMN;

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_TREENODE, CA_ELEM_TREENODE, id, &reused);
    if (!node) return NULL;

    Ca_TreeNode *tn = NULL;
    if (reused && node->widget_type == CA_WIDGET_TREENODE && node->widget)
        tn = (Ca_TreeNode *)node->widget;
    if (!tn) {
        tn = alloc_treenode(g_ctx.window);
        if (!tn) return NULL;
        memset(tn, 0, sizeof(*tn));
        tn->in_use = true;
        node->widget_type = CA_WIDGET_TREENODE;
        node->widget = tn;
    }

    tn->node = node;
    tn->in_use = true;
    if (!reused) tn->expanded = desc->expanded;
    tn->text_color = 0;
    WIDGET_SET_TEXT(node, reused, tn->text, CA_LABEL_TEXT_MAX, desc->text);
    tn->on_toggle = desc->on_toggle;
    tn->toggle_data = desc->toggle_data;
    WIDGET_SET(node, reused, tn->is_leaf, desc->is_leaf);
    WIDGET_SET_TEXT(node, reused, tn->icon, sizeof(tn->icon), desc->icon);
    WIDGET_SET(node, reused, tn->icon_color, desc->icon_color);

    if (desc->hidden) node->desc.hidden = true;

    /* Inherit hidden state from a collapsed ancestor tree-node.  The click
       handler updates desc.hidden directly on children for immediate
       feedback, but claim_child resets node->desc on every rebuild — without
       this propagation, any builder re-run (reactive flush, hover-induced
       repaint with rebuild, etc.) would silently un-hide nested rows of a
       collapsed parent and lay them out on top of sibling rows. */
    {
        Ca_Node *pp = node->parent;
        while (pp) {
            if (pp->widget_type == CA_WIDGET_TREENODE && pp->widget) {
                Ca_TreeNode *ptn = (Ca_TreeNode *)pp->widget;
                if (!ptn->expanded) { node->desc.hidden = true; break; }
            }
            pp = pp->parent;
        }
    }

    /* Compute depth from parent chain (count tree node ancestors) */
    tn->depth = 0;
    Ca_Node *p = node->parent;
    while (p) {
        if (p->elem_type == CA_ELEM_TREENODE) tn->depth++;
        p = p->parent;
    }

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TREENODE, desc->style, id, &tn->text_color);

    /* Create a header row node for the clickable label */
    Ca_NodeDesc hdr = {0};
    hdr.direction    = CA_DIR_ROW;
    /* The CSS height is meant for the header row, not the container.
       Lift it off the container so it auto-sizes when expanded. */
    hdr.height       = node->desc.height > 0.0f ? node->desc.height : s(20.0f);
    node->desc.height = 0.0f;
    hdr.font_size    = node->desc.font_size;  /* inherit from CSS */
    hdr.font_bold    = node->desc.font_bold;  /* inherit from CSS */
    hdr.padding_left = s(4.0f) * (float)tn->depth;
    hdr.text_align   = 0; /* left-aligned */
    /* Sensible defaults — CSS can override via the tree node style */
    hdr.corner_radius = 0.0f;
    Ca_Node *hdr_node = NULL;
    if (reused && node->child_count > 0)
        hdr_node = node->children[0];
    if (!hdr_node)
        hdr_node = ca_node_add(node, &hdr);

    /* Tag the header row with a built-in "tree-row" class so user CSS can
       give every tree row uniform hover/focus/active feedback without
       knowing the container's class name. The user may append additional
       row classes via desc->row_style. Re-resolve CSS on the header so
       :hover etc. actually paint on the correct (innermost) node. */
    if (hdr_node) {
        char row_classes[CA_NODE_CLASS_MAX];
        if (desc->row_style && desc->row_style[0])
            snprintf(row_classes, sizeof(row_classes), "tree-row %s", desc->row_style);
        else
            snprintf(row_classes, sizeof(row_classes), "tree-row");

        /* Snapshot the OLD CSS-resolved desc (from previous frame), then
           bare-assign the sparse inline desc.  apply_css resolves CSS for
           the current frame (including :hover / :focus / :active state)
           and diffs the post-CSS desc against the snapshot, marking
           CONTENT/LAYOUT dirty only when visuals actually changed.
           For newly added nodes the snapshot equals the just-assigned
           sparse desc, which is fine — new nodes are dirty via
           ca_node_add. */
        s_pre_css_desc = hdr_node->desc;
        s_pre_css_node = hdr_node;
        hdr_node->desc = hdr;
        uint32_t dummy = 0;
        apply_css(hdr_node, &hdr_node->desc, CA_ELEM_DIV, row_classes, NULL, &dummy);
    }

    ctx_push_mode(node, ctx_top_reconcile());
    if (ctx_top_reconcile())
        *ctx_top_cursor() = 1;
    return tn;
}


bool ca_tree_node_expanded(const Ca_TreeNode *n)
{
    assert(n && n->in_use);
    return n->expanded;
}

void ca_tree_node_set_expanded(Ca_TreeNode *n, bool expanded)
{
    assert(n && n->in_use && n->node);
    if (n->expanded == expanded) return;
    n->expanded = expanded;
    /* Mirror the hide/show logic from the click handler */
    for (uint32_t ci = 1; ci < n->node->child_count; ++ci) {
        n->node->children[ci]->desc.hidden = !expanded;
        n->node->children[ci]->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
    }
    n->node->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
}

/* ============================================================
   PUBLIC — Table
   ============================================================ */

void ca_table_begin(const Ca_TableDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_COLUMN;

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_TABLE, CA_ELEM_TABLE, id, &reused);
    if (!node) return;

    Ca_Table *tbl = NULL;
    if (reused && node->widget_type == CA_WIDGET_TABLE && node->widget)
        tbl = (Ca_Table *)node->widget;
    if (!tbl) {
        tbl = alloc_table(g_ctx.window);
        if (!tbl) return;
        memset(tbl, 0, sizeof(*tbl));
        tbl->in_use = true;
        node->widget_type = CA_WIDGET_TABLE;
        node->widget = tbl;
    }

    tbl->node = node;
    tbl->in_use = true;
    tbl->column_count = desc->column_count;
    if (tbl->column_count > 16) tbl->column_count = 16;
    for (int i = 0; i < tbl->column_count; ++i)
        tbl->column_widths[i] = desc->column_widths ? s(desc->column_widths[i]) : s(80.0f);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TABLE, desc->style, id, &dummy);
    ctx_push_mode(node, ctx_top_reconcile());
}


void ca_table_row_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    nd.direction = CA_DIR_ROW;
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : (desc ? desc->id : NULL);
    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_NONE, CA_ELEM_TABLE_ROW, id, &reused);
    assert(node);
    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TABLE_ROW,
              desc ? desc->style : NULL, id, &dummy);

    /* Apply column widths from the table ancestor */
    Ca_Node *p = node->parent;
    while (p) {
        if (p->elem_type == CA_ELEM_TABLE) break;
        p = p->parent;
    }

    ctx_push_mode(node, ctx_top_reconcile());
}


void ca_table_cell(const Ca_TextDesc *desc)
{
    assert(g_ctx.active && desc);

    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    /* Find the table ancestor and look up column width */
    Ca_Node *row = ctx_top();
    Ca_Node *tbl_node = row->parent;
    float cell_w = s(80.0f);
    if (tbl_node && tbl_node->elem_type == CA_ELEM_TABLE) {
        /* Find this table's pool entry to get column widths */
        Ca_Window *win = g_ctx.window;
        for (uint32_t i = 0; i < CA_MAX_TABLES_PER_WINDOW; ++i) {
            Ca_Table *t = &win->table_pool[i];
            if (t->in_use && t->node == tbl_node) {
                int col = (int)row->child_count;
                if (col < t->column_count) cell_w = t->column_widths[col];
                break;
            }
        }
    }

    Ca_NodeDesc nd = {0};
    nd.width = cell_w;
    nd.height = s(24.0f);
    nd.padding_left = s(4.0f);
    nd.padding_right = s(4.0f);

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_LABEL, CA_ELEM_TABLE_CELL, id, &reused);
    if (!node) return;

    Ca_Label *lbl = NULL;
    if (reused && node->widget_type == CA_WIDGET_LABEL && node->widget)
        lbl = (Ca_Label *)node->widget;
    if (!lbl) {
        lbl = alloc_label(g_ctx.window);
        if (!lbl) return;
        memset(lbl, 0, sizeof(*lbl));
        lbl->in_use = true;
        node->widget_type = CA_WIDGET_LABEL;
        node->widget = lbl;
    }

    lbl->node = node;
    lbl->in_use = true;
    lbl->color = desc->color;
    WIDGET_SET_TEXT(node, reused, lbl->text, CA_LABEL_TEXT_MAX, desc->text);

    apply_css(node, &node->desc, CA_ELEM_TABLE_CELL, desc->style, id, &lbl->color);
}

/* ============================================================
   PUBLIC — Tooltip (attach to previously created element)
   ============================================================ */

static Ca_Tooltip *tooltip_for_node(Ca_Node *target, const Ca_TooltipDesc *desc)
{
    assert(g_ctx.active && desc);
    if (!target) return NULL;

    /* Reuse an existing tooltip already bound to this node so that
       reconcile rebuilds don't exhaust the pool each frame. */
    Ca_Tooltip *tt = NULL;
    Ca_Window  *win = g_ctx.window;
    if (win && win->tooltip_pool) {
        for (uint32_t i = 0; i < CA_MAX_TOOLTIPS_PER_WINDOW; ++i) {
            Ca_Tooltip *t = &win->tooltip_pool[i];
            if (t->in_use && t->node == target) { tt = t; break; }
        }
    }
    if (!tt) tt = alloc_tooltip(win);
    if (!tt) return NULL;

    tt->node = target;
    tt->in_use = true;
    if (desc->text)  snprintf(tt->text,  CA_LABEL_TEXT_MAX, "%s", desc->text);
    else tt->text[0] = '\0';
    if (desc->style) snprintf(tt->style, CA_NODE_CLASS_MAX, "%s", desc->style);
    else tt->style[0] = '\0';

    /* Resolve CSS font-size once here (widget build time) rather than every
       frame in the renderer.  A one-element proxy node is used so that the
       same ca_style_resolve path used by every other widget applies. */
    tt->font_size = 0.0f;
    Ca_Instance *instance = win ? win->instance : NULL;
    if (instance && (instance->system_stylesheet || instance->stylesheet)) {
        Ca_Node proxy;
        memset(&proxy, 0, sizeof(proxy));
        proxy.in_use = true;
        proxy.window = win;
        if (tt->style[0])
            snprintf(proxy.classes, CA_NODE_CLASS_MAX, "%s", tt->style);
        Ca_ResolvedStyle rs;
        memset(&rs, 0, sizeof(rs));
        ca_style_resolve_layers(instance->system_stylesheet, instance->stylesheet,
                                &proxy, CA_ELEM_TOOLTIP, proxy.classes, &rs);
        tt->font_size = rs.font_size; /* 0 if no rule matched */
    }

    return tt;
}

Ca_Tooltip *ca_tooltip(const Ca_TooltipDesc *desc)
{
    assert(g_ctx.active && desc);

    Ca_Node *parent = ctx_top();
    if (parent->child_count == 0) return NULL;

    /* When called inside a ca_tree_node_begin/end block, attach to the header
       row (children[0]) — same convention as ca_context_menu. */
    Ca_Node *target;
    if (parent->widget_type == CA_WIDGET_TREENODE)
        target = parent->children[0];
    else
        target = parent->children[parent->child_count - 1];

    return tooltip_for_node(target, desc);
}

Ca_Tooltip *ca_tooltip_for_widget(void *widget, const Ca_TooltipDesc *desc)
{
    assert(g_ctx.active && desc);
    if (!widget) return NULL;
    Ca_Node *target = *(Ca_Node **)widget;
    return tooltip_for_node(target, desc);
}

void ca_tooltip_set_text(Ca_Tooltip *tooltip, const char *text)
{
    if (!tooltip || !tooltip->in_use) return;
    const char *next_text = text ? text : "";
    if (strcmp(tooltip->text, next_text) == 0) return;
    snprintf(tooltip->text, CA_LABEL_TEXT_MAX, "%s", next_text);
    if (tooltip->node)
        tooltip->node->dirty |= CA_DIRTY_CONTENT;
}

/* ============================================================
   PUBLIC — Menu bar
   ============================================================ */

void ca_menubar_dropdown_geometry(Ca_Window *win,
                                  const Ca_Node *header,
                                  float menu_w,
                                  float menu_h,
                                  float *out_x,
                                  float *out_y)
{
    float x = header ? header->x : 0.0f;
    float y = header ? header->y + header->h : 0.0f;
    float window_w = win && win->root ? win->root->w : 0.0f;
    float window_h = win && win->root ? win->root->h : 0.0f;
    if (window_w > 0.0f && x + menu_w > window_w) x = window_w - menu_w;
    if (window_h > 0.0f && y + menu_h > window_h) y = window_h - menu_h;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

void ca_menubar_submenu_geometry(Ca_Window *win,
                                 float drop_x,
                                 float drop_y,
                                 float menu_w,
                                 float sub_w,
                                 float sub_h,
                                 float parent_offset_y,
                                 float *out_x,
                                 float *out_y)
{
    float window_w = win && win->root ? win->root->w : 0.0f;
    float window_h = win && win->root ? win->root->h : 0.0f;
    float x = drop_x + menu_w;
    float y = drop_y + parent_offset_y;
    if (window_w > 0.0f && x + sub_w > window_w) x = drop_x - sub_w;
    if (window_h > 0.0f && y + sub_h > window_h) y = window_h - sub_h;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

Ca_MenuBar *ca_menu_bar(const Ca_MenuBarDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    /* Bar container — dimensions and styling driven by CSS */
    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_ROW;

    bool reused = false;
    Ca_Node *bar = claim_child(&nd, CA_WIDGET_MENUBAR, CA_ELEM_DIV, id, &reused);
    if (!bar) return NULL;

    Ca_MenuBar *mb = NULL;
    if (reused && bar->widget_type == CA_WIDGET_MENUBAR && bar->widget)
        mb = (Ca_MenuBar *)bar->widget;
    if (!mb) {
        mb = alloc_menubar(g_ctx.window);
        if (!mb) return NULL;
        memset(mb, 0, sizeof(*mb));
        mb->in_use = true;
        bar->widget_type = CA_WIDGET_MENUBAR;
        bar->widget = mb;
    }

    mb->node = bar;
    mb->in_use = true;
    mb->active_menu = -1;
    mb->hover_item = -1;
    mb->hover_sub_item = -1;
    mb->menu_count = desc->menu_count;
    if (mb->menu_count > CA_MAX_MENUS_PER_BAR)
        mb->menu_count = CA_MAX_MENUS_PER_BAR;

    /* Theme colors — use caller-provided or sensible defaults */
    mb->header_highlight = desc->header_highlight ? desc->header_highlight : CA_THEME_BG_OVERLAY;
    mb->dropdown_bg      = desc->dropdown_bg      ? desc->dropdown_bg      : CA_THEME_POPUP_BG;
    mb->dropdown_border  = desc->dropdown_border  ? desc->dropdown_border  : CA_THEME_POPUP_BORDER;
    mb->dropdown_hover   = desc->dropdown_hover   ? desc->dropdown_hover   : CA_THEME_BG_OVERLAY;
    mb->dropdown_text    = desc->dropdown_text    ? desc->dropdown_text    : CA_THEME_POPUP_TEXT;
    mb->text_color       = desc->text_color       ? desc->text_color       : CA_THEME_TEXT_MUTED;

    uint32_t dummy = 0;
    apply_css(bar, &bar->desc, CA_ELEM_DIV, desc->style, id, &dummy);
    /* Inline fallback: apply bar_height when no CSS class governs it */
    if (!desc->style && desc->bar_height > 0.0f) {
        bar->desc.height      = desc->bar_height;
        bar->desc.align_items = CA_ALIGN_CENTER;
    }

    ca_node_trim_children(bar, 0);

    for (int mi = 0; mi < mb->menu_count; ++mi) {
        const Ca_MenuDesc *mdesc = &desc->menus[mi];
        Ca_MenuBarMenu *menu = &mb->menus[mi];

        if (mdesc->label)
            snprintf(menu->label, CA_MENU_LABEL_MAX, "%s", mdesc->label);
        menu->item_count = mdesc->item_count;
        if (menu->item_count > CA_MAX_ITEMS_PER_MENU)
            menu->item_count = CA_MAX_ITEMS_PER_MENU;
        menu->active_sub = -1;
        for (int ii = 0; ii < menu->item_count; ++ii) {
            if (mdesc->items[ii].label)
                snprintf(menu->items[ii].label, CA_MENU_LABEL_MAX, "%s",
                         mdesc->items[ii].label);
            menu->items[ii].action      = mdesc->items[ii].action;
            menu->items[ii].action_data = mdesc->items[ii].action_data;
            menu->items[ii].separator   = mdesc->items[ii].separator;
            int nsub = mdesc->items[ii].sub_item_count;
            if (nsub > CA_MAX_SUB_ITEMS_PER_ITEM) nsub = CA_MAX_SUB_ITEMS_PER_ITEM;
            menu->items[ii].sub_item_count = nsub;
            if (nsub > 0 && mdesc->items[ii].sub_items) {
                for (int si = 0; si < nsub; si++) {
                    const Ca_MenuItemDesc *sd = &mdesc->items[ii].sub_items[si];
                    if (sd->label)
                        snprintf(menu->items[ii].sub_items[si].label,
                                 CA_MENU_LABEL_MAX, "%s", sd->label);
                    menu->items[ii].sub_items[si].action      = sd->action;
                    menu->items[ii].sub_items[si].action_data = sd->action_data;
                }
            }
        }

        /* Header node — padding & alignment via caller-supplied item_style */
        Ca_NodeDesc hnd = {0};

        Ca_Node *hdr = ca_node_add(bar, &hnd);
        if (!hdr) continue;

        uint32_t header_color = 0u;
        apply_css(hdr, &hdr->desc, CA_ELEM_DIV, desc->item_style, NULL,
                  &header_color);
        /* Inline fallback: apply padding/font_size when no CSS class governs it */
        if (!desc->item_style) {
            if (desc->item_padding_lr > 0.0f) {
                hdr->desc.padding_left  = desc->item_padding_lr;
                hdr->desc.padding_right = desc->item_padding_lr;
            }
            if (desc->item_font_size > 0.0f)
                hdr->desc.font_size = desc->item_font_size;
        }

        /* Measure label at the font size the header will actually render at,
           not at the font atlas default size.  Measuring at the wrong size
           produces a header that is wider than the rendered text, which makes
           labels appear left-aligned instead of centred. */
        float render_fs = hdr->desc.font_size > 0.0f
                              ? hdr->desc.font_size
                              : (g_ctx.window->instance->font
                                     ? g_ctx.window->instance->font->default_size
                                     : 12.0f);
        float tw = ca_measure_text_px(g_ctx.window, menu->label, render_fs);

        hdr->desc.width = tw + hdr->desc.padding_left + hdr->desc.padding_right;
        /* Header should fill the bar's full vertical extent so that
           `align_items: center` actually has space to centre the label
           in. Without this the header collapses to label height (=
           font_size) and the menu label sits flush against the bar's
           top edge. Only override when neither caller CSS nor an
           explicit desc height has set one. */
        if (hdr->desc.height <= 0.0f && bar->desc.height > 0.0f) {
            hdr->desc.height = bar->desc.height;
        }

        /* Derive dropdown item font size from the header node's CSS font size */
        if (mb->item_font_size <= 0.0f && hdr->desc.font_size > 0.0f)
            mb->item_font_size = hdr->desc.font_size;

        menu->header_node = hdr;

        /* Label inside header.

           lnd.height is set explicitly to the font size so the label
           node hugs the glyph extent rather than auto-filling the
           header's full height. With auto-fill, the text inside the
           label paints at its bbox top, so labels look stuck to the
           top of the bar instead of vertically centred. Sizing the
           label to font_size lets the parent header's
           `align_items: center` line it up on the cross axis the way
           the user expects. */
        Ca_Label *lbl = alloc_label(g_ctx.window);
        if (lbl) {
            Ca_NodeDesc lnd = {0};
            lnd.width     = tw;
            lnd.height    = hdr->desc.font_size > 0.0f
                                ? hdr->desc.font_size : 12.0f;
            lnd.font_size = hdr->desc.font_size; /* render label at CSS-specified size */
            Ca_Node *ln = ca_node_add(hdr, &lnd);
            if (ln) {
                ln->widget_type = CA_WIDGET_LABEL;
                ln->widget = lbl;
                lbl->node = ln;
                lbl->in_use = true;
                lbl->color = header_color ? header_color : mb->text_color;
                snprintf(lbl->text, CA_LABEL_TEXT_MAX, "%s", menu->label);
            }
        }
    }

    return mb;
}

/* ============================================================
   PUBLIC — Context menu (attach to previously created element)
   ============================================================ */

void ca_context_menu(const Ca_CtxMenuDesc *desc)
{
    assert(g_ctx.active && desc);

    Ca_Node *parent = ctx_top();

    /* When called inside a ca_tree_node_begin/end block the context stack
       top is the tree node container.  Its children[0] is always the header
       row (the actual visible/clickable row), regardless of how many child
       entity nodes were added in previous frames.  Using children[child_count-1]
       would target the last expanded child — wrong.  For every other container
       the last child is correct (it was just created).  If the container has
       no children at all (e.g. a button whose label is its own .text field,
       with no nested content nodes), attach to the container itself — the
       hit-test below (point_in_node) works on any node, leaf or not, so this
       is not a special case at read time, just at attach time. */
    Ca_Node *target;
    if (parent->child_count == 0)
        target = parent;
    else if (parent->widget_type == CA_WIDGET_TREENODE)
        target = parent->children[0];  /* always the header row */
    else
        target = parent->children[parent->child_count - 1];

    /* Reuse an existing ctxmenu already bound to this node (reactive
       reconcile rebuilds invoke ca_context_menu every frame on the
       same reused node — without reuse the pool exhausts quickly). */
    Ca_CtxMenu *cm = NULL;
    Ca_Window  *win = g_ctx.window;
    if (win && win->ctxmenu_pool) {
        for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW; ++i) {
            Ca_CtxMenu *c = &win->ctxmenu_pool[i];
            if (c->in_use && c->node == target) { cm = c; break; }
        }
    }
    bool reused = (cm != NULL);
    if (!cm) cm = alloc_ctxmenu(win);
    if (!cm) return;

    cm->node = target;
    cm->in_use = true;
    if (!reused) {
        cm->open = false;
        cm->hover_index = -1;
    }
    cm->item_count = desc->item_count;
    if (cm->item_count > CA_MAX_CTXMENU_ITEMS)
        cm->item_count = CA_MAX_CTXMENU_ITEMS;
    for (int i = 0; i < cm->item_count; ++i) {
        if (desc->items[i])
            snprintf(cm->items[i], CA_OPTION_TEXT_MAX, "%s", desc->items[i]);
    }
    cm->on_select = desc->on_select;
    cm->select_data = desc->select_data;
    cm->on_open = desc->on_open;
    cm->open_data = desc->open_data;
}

static void update_context_menu_hover(Ca_Window *win, float mx, float my,
                                      float ui_s)
{
    if (!win || !win->ctxmenu_pool) return;

    const float item_h = 24.0f * ui_s;
    const float sep_h = 8.0f * ui_s;
    const float menu_w = 180.0f * ui_s;

    for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW; ++i) {
        Ca_CtxMenu *cm = &win->ctxmenu_pool[i];
        if (!cm->in_use || !cm->open) {
            continue;
        }
        if (cm->node && node_is_ancestor_hidden(cm->node)) {
            continue;
        }

        float menu_h = 6.0f * ui_s;
        for (int mi = 0; mi < cm->item_count; ++mi) {
            const bool is_sep =
                (cm->items[mi][0] == '-' && cm->items[mi][1] == '\0');
            menu_h += is_sep ? sep_h : item_h;
        }

        float x = cm->open_x;
        float y = cm->open_y;
        if (win->sc.extent.width > 0 &&
            x + menu_w > (float)win->sc.extent.width) {
            x = (float)win->sc.extent.width - menu_w;
        }
        if (win->sc.extent.height > 0 &&
            y + menu_h > (float)win->sc.extent.height) {
            y = (float)win->sc.extent.height - menu_h;
        }
        if (x < 0.0f) x = 0.0f;
        if (y < 0.0f) y = 0.0f;

        int hover_index = -1;
        if (mx >= x && mx <= x + menu_w && my >= y && my <= y + menu_h) {
            float item_y = y + 3.0f * ui_s;
            for (int mi = 0; mi < cm->item_count; ++mi) {
                const bool is_sep =
                    (cm->items[mi][0] == '-' && cm->items[mi][1] == '\0');
                const float row_h = is_sep ? sep_h : item_h;
                if (!is_sep && my >= item_y && my <= item_y + row_h) {
                    hover_index = mi;
                    break;
                }
                item_y += row_h;
            }
        }

        if (cm->hover_index != hover_index) {
            cm->hover_index = hover_index;
            if (cm->node) {
                cm->node->dirty |= CA_DIRTY_CONTENT;
            }
        }
    }
}

/* ============================================================
   PUBLIC — Modal / Dialog
   ============================================================ */

Ca_Modal *ca_modal_begin(const Ca_ModalDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    /* The modal takes full parent size, overlay renders in paint */
    nd.hidden = !desc->visible;

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_MODAL, CA_ELEM_MODAL, id, &reused);
    if (!node) return NULL;

    Ca_Modal *m = NULL;
    if (reused && node->widget_type == CA_WIDGET_MODAL && node->widget)
        m = (Ca_Modal *)node->widget;
    if (!m) {
        m = alloc_modal(g_ctx.window);
        if (!m) return NULL;
        memset(m, 0, sizeof(*m));
        m->in_use = true;
        node->widget_type = CA_WIDGET_MODAL;
        node->widget = m;
    }

    m->node = node;
    m->in_use = true;
    m->visible = desc->visible;
    m->overlay_color = desc->overlay_color
        ? desc->overlay_color
        : CA_THEME_MODAL_OVERLAY;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_MODAL, desc->style, id, &dummy);
    ctx_push_mode(node, ctx_top_reconcile());
    return m;
}


void ca_modal_set_visible(Ca_Modal *modal, bool visible)
{
    if (!modal || !modal->in_use) return;
    modal->visible = visible;
    if (modal->node)
        node_set_hidden(modal->node, !visible);
}

/* ============================================================
   PUBLIC — Splitter (resizable split container)
   ============================================================ */

Ca_Splitter *ca_split_begin(const Ca_SplitDesc *desc)
{
    assert(g_ctx.active && desc);
    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;

    Ca_NodeDesc nd = {0};
    nd.direction = dir_from_int(desc->direction);
    nd.flex_grow = 1.0f;
    /* The splitter itself fills available space by default. */

    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_SPLITTER, CA_ELEM_SPLITTER, id, &reused);
    if (!node) return NULL;

    Ca_Splitter *sp = NULL;
    if (reused && node->widget_type == CA_WIDGET_SPLITTER && node->widget)
        sp = (Ca_Splitter *)node->widget;
    if (!sp) {
        sp = alloc_splitter(g_ctx.window);
        if (!sp) return NULL;
        memset(sp, 0, sizeof(*sp));
        sp->in_use = true;
        node->widget_type = CA_WIDGET_SPLITTER;
        node->widget = sp;
    }

    sp->node      = node;
    sp->in_use    = true;
    sp->direction = desc->direction;
    /* The ratio is owned by the splitter widget once the user drags it.
       Only seed it from the desc on initial creation; subsequent rebuilds
       must preserve the dragged value. Callers that need to update the
       ratio programmatically should use ca_split_set_ratio(). */
    if (!reused) {
        sp->ratio = (desc->ratio > 0.0f && desc->ratio < 1.0f) ? desc->ratio : 0.5f;
    }
    sp->min_ratio = (desc->min_ratio > 0.0f) ? desc->min_ratio : 0.1f;
    sp->max_ratio = (desc->max_ratio > 0.0f) ? desc->max_ratio : 0.9f;
    sp->bar_size  = (desc->bar_size > 0.0f)  ? s(desc->bar_size)  : s(4.0f);
    const bool has_stylesheet = node->window && node->window->instance &&
                                node->window->instance->stylesheet;
    sp->bar_color       = desc->bar_color ? desc->bar_color :
                          (has_stylesheet ? 0u : CA_THEME_BG_VOID);
    sp->bar_hover_color = desc->bar_hover_color ? desc->bar_hover_color : CA_THEME_ACCENT;
    sp->on_resize = desc->on_resize;
    sp->user_data = desc->user_data;
    /* Do NOT reset sp->dragging here — ca_widget_input_pass owns the
       dragging lifecycle.  ca_split_begin runs every frame (builders
       re-run on blink ticks etc.), so unconditionally clearing dragging
       kills an in-progress drag one frame after it starts.  New splitters
       get dragging=false via memset; reused ones must preserve it. */

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_SPLITTER,
              desc->style, id, &dummy);

    ctx_push_mode(node, ctx_top_reconcile());
    return sp;
}


float ca_split_get_ratio(const Ca_Splitter *s)
{
    assert(s && s->in_use);
    return s->ratio;
}

void ca_split_set_ratio(Ca_Splitter *s, float ratio)
{
    assert(s && s->in_use);
    if (ratio < s->min_ratio) ratio = s->min_ratio;
    if (ratio > s->max_ratio) ratio = s->max_ratio;
    s->ratio = ratio;
    s->node->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
}

/* ============================================================
   IMAGE — textured quad widget
   ============================================================ */

#include "image.h"

Ca_Image *ca_image_create(Ca_Instance *instance,
                          const uint8_t *pixels, int width, int height)
{
    return ca_image_create_impl(instance, pixels, width, height);
}

void ca_image_destroy(Ca_Instance *instance, Ca_Image *image)
{
    ca_image_destroy_impl(instance, image);
}

void ca_image(const Ca_ImageDesc *desc)
{
    assert(g_ctx.active);
    if (!desc || !desc->image) return;

    Ca_Image *img = desc->image;
    float w = desc->width  > 0 ? s(desc->width)  : (float)img->width;
    float h = desc->height > 0 ? s(desc->height) : (float)img->height;

    Ca_NodeDesc nd = {0};
    nd.width  = w;
    nd.height = h;
    nd.corner_radius = s(desc->corner_radius);

    const char *next_key = consume_next_key();
    const char *id = next_key ? next_key : desc->id;
    bool reused = false;
    Ca_Node *node = claim_child(&nd, CA_WIDGET_IMAGE, CA_ELEM_IMAGE, id, &reused);
    if (!node) return;

    if (reused && node->widget != (void *)img)
        node->dirty |= CA_DIRTY_CONTENT;
    node->widget_type = CA_WIDGET_IMAGE;
    node->widget      = (void *)img;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_IMAGE, desc->style, id, &dummy);
}

/* ============================================================
   VIEWPORT — offscreen render target widget
   ============================================================ */

static Ca_Viewport *alloc_viewport(Ca_Window *win)
{
    for (uint32_t i = 0; i < CA_MAX_VIEWPORTS_PER_WINDOW; ++i) {
        if (!win->viewport_pool[i].in_use)
            return &win->viewport_pool[i];
    }
    fprintf(stderr, "[causality] viewport pool exhausted (max %d)\n",
            CA_MAX_VIEWPORTS_PER_WINDOW);
    return NULL;
}

Ca_Viewport *ca_viewport(const Ca_ViewportDesc *desc)
{
    assert(g_ctx.active);
    if (!desc) return NULL;

    Ca_Node *parent = ctx_top();
    if (!parent) return NULL;

    Ca_Window *win = g_ctx.window;
    Ca_Viewport *vp = alloc_viewport(win);
    if (!vp) return NULL;
    memset(vp, 0, sizeof(*vp));

    float w = desc->width  > 0 ? s(desc->width)  : 0;
    float h = desc->height > 0 ? s(desc->height) : 0;

    Ca_NodeDesc nd = {0};
    nd.width  = w;
    nd.height = h;
    /* Let viewport fill available space when no size is given */
    if (w == 0) nd.flex_grow = 1.0f;
    if (h == 0) nd.flex_grow = 1.0f;

    Ca_Node *node = ca_node_add(parent, &nd);
    if (!node) { vp->in_use = false; return NULL; }

    VkFormat fmt = desc->format ? (VkFormat)desc->format : VK_FORMAT_R8G8B8A8_UNORM;

    /* Calculate initial pixel dimensions from content scale */
    float content_scale = 1.0f;
    if (win->glfw)
        glfwGetWindowContentScale(win->glfw, &content_scale, NULL);
    uint32_t px_w = (uint32_t)(w * content_scale);
    uint32_t px_h = (uint32_t)(h * content_scale);
    if (px_w < 1) px_w = 64;
    if (px_h < 1) px_h = 64;

    vp->instance    = win->instance;
    vp->node        = node;
    vp->on_render   = desc->on_render;
    vp->render_data = desc->render_data;
    vp->on_resize   = desc->on_resize;
    vp->resize_data = desc->resize_data;
    memcpy(vp->clear_color.float32, desc->clear_color, sizeof desc->clear_color);
    vp->in_use      = true;
    vp->needs_redraw = true;

    if (!ca_viewport_gpu_create(win->instance, vp, px_w, px_h, fmt)) {
        vp->in_use = false;
        return NULL;
    }

    node->widget_type = CA_WIDGET_VIEWPORT;
    node->widget      = (void *)vp;

    if (desc->id)    snprintf(node->id, CA_NODE_ID_MAX, "%s", desc->id);
    if (desc->style) snprintf(node->classes, CA_NODE_CLASS_MAX, "%s", desc->style);

    return vp;
}

VkCommandBuffer ca_viewport_cmd(Ca_Viewport *viewport)
{
    /* Only valid called from inside the on_render callback (the only actual
       caller — see ca_render_trampoline in qs_gpu.c), where frame_index still
       names the slot currently being recorded — see the frame_index cycling
       comment in ca_viewport_render_all. */
    return viewport ? viewport->frame[viewport->frame_index].cmd : VK_NULL_HANDLE;
}

uint32_t ca_viewport_width(const Ca_Viewport *viewport)
{
    return viewport ? viewport->width : 0;
}

uint32_t ca_viewport_height(const Ca_Viewport *viewport)
{
    return viewport ? viewport->height : 0;
}

VkImage ca_viewport_image(const Ca_Viewport *viewport)
{
    return viewport ? viewport->frame[viewport->frame_index].color_image : VK_NULL_HANDLE;
}

VkImageView ca_viewport_image_view(const Ca_Viewport *viewport)
{
    return viewport ? viewport->frame[viewport->frame_index].color_view : VK_NULL_HANDLE;
}

VkFormat ca_viewport_format(const Ca_Viewport *viewport)
{
    return viewport ? viewport->format : VK_FORMAT_UNDEFINED;
}

uint32_t ca_viewport_frame_index(const Ca_Viewport *viewport)
{
    return viewport ? viewport->frame_index : 0;
}

Ca_Instance *ca_viewport_instance(Ca_Viewport *viewport)
{
    return viewport ? viewport->instance : NULL;
}

void ca_viewport_set_callbacks(Ca_Viewport *viewport,
                               Ca_ViewportRenderFn on_render, void *render_data,
                               Ca_ViewportResizeFn on_resize, void *resize_data)
{
    if (!viewport) return;
    viewport->on_render   = on_render;
    viewport->render_data = render_data;
    viewport->on_resize   = on_resize;
    viewport->resize_data = resize_data;
}

void ca_viewport_request_redraw(Ca_Viewport *viewport)
{
    if (viewport) viewport->needs_redraw = true;
}

void ca_viewport_screen_rect(const Ca_Viewport *viewport,
                              float *x, float *y, float *w, float *h)
{
    if (!viewport || !viewport->node) {
        if (x) *x = 0; if (y) *y = 0;
        if (w) *w = 0; if (h) *h = 0;
        return;
    }
    if (x) *x = viewport->node->x;
    if (y) *y = viewport->node->y;
    if (w) *w = viewport->node->w;
    if (h) *h = viewport->node->h;
}

/* ============================================================
   INPUT PASS — hit-test, focus, keyboard
   ============================================================ */

static bool point_in_node(Ca_Node *n, float px, float py)
{
    return px >= n->x && px <= n->x + n->w &&
           py >= n->y && py <= n->y + n->h;
}

/* Check that a point is not clipped out by any scroll/hidden ancestor.
   When scroll bakes the offset into child absolute coords, a row scrolled
   above its container can appear at any y on screen.  This walk rejects
   such hits by checking the point is inside every clipping ancestor. */
static bool point_within_clip_ancestors(Ca_Node *n, float px, float py)
{
    Ca_Node *p = n->parent;
    while (p) {
        bool clips = (p->desc.overflow_x != 0 || p->desc.overflow_y != 0);
        if (clips) {
            if (px < p->x ||
                px > p->x + ca_scrollbar_viewport_width(p) ||
                py < p->y ||
                py > p->y + ca_scrollbar_viewport_height(p))
                return false;
        }
        p = p->parent;
    }
    return true;
}

static int node_depth(Ca_Node *n)
{
    int depth = 0;
    while (n) {
        depth++;
        n = n->parent;
    }
    return depth;
}

/* Return the effective rendering z-index for a node: the first non-zero
   z_index found by walking up the ancestor chain (matching how paint.c
   propagates z_index to draw commands via apply_inherited_z). */
static int16_t node_effective_z(Ca_Node *n)
{
    for (Ca_Node *cur = n; cur; cur = cur->parent)
        if (cur->desc.z_index != 0)
            return cur->desc.z_index;
    return 0;
}

static bool node_paints_after(Ca_Node *candidate, Ca_Node *current)
{
    if (!current) return true;
    if (!candidate || candidate == current) return false;

    Ca_Node *a = candidate;
    Ca_Node *b = current;
    int da = node_depth(a);
    int db = node_depth(b);

    while (da > db && a) {
        if (a->parent == current) return true;
        a = a->parent;
        da--;
    }
    while (db > da && b) {
        if (b->parent == candidate) return false;
        b = b->parent;
        db--;
    }

    while (a && b && a->parent != b->parent) {
        a = a->parent;
        b = b->parent;
    }

    if (!a || !b || !a->parent) return false;

    Ca_Node *parent = a->parent;
    int candidate_index = -1;
    int current_index = -1;
    for (uint32_t i = 0; i < parent->child_count; ++i) {
        if (parent->children[i] == a) candidate_index = (int)i;
        if (parent->children[i] == b) current_index = (int)i;
    }

    return candidate_index > current_index;
}

/* Treat a node as input-inert if it (or any ancestor) is disabled or hidden.
   The hidden check matters because tabbed/collapsed panels whose builder
   doesn't re-run leave stale layout coords on descendants — without this,
   click hit-tests find buttons inside hidden panels and fire their
   callbacks (e.g. clicking the toolbar dispatches asset-row clicks when
   the Assets tab is hidden behind the Console tab). */
static bool is_effectively_disabled(Ca_Node *n)
{
    while (n) {
        if (n->desc.disabled || n->desc.hidden) return true;
        n = n->parent;
    }
    return false;
}

/* Check if a node is focusable (button or text input, and not disabled) */
static bool is_focusable_node(Ca_Window *win, Ca_Node *n)
{
    if (is_effectively_disabled(n)) return false;
    for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i)
        if (win->button_pool[i].in_use &&
            win->button_pool[i].node == n &&
            win->button_pool[i].keyboard_focusable)
            return true;
    for (uint32_t i = 0; i < CA_MAX_INPUTS_PER_WINDOW; ++i)
        if (win->input_pool[i].in_use && win->input_pool[i].node == n)
            return true;
    return false;
}

/* Find the Ca_TextInput for a given node (or NULL) */
static Ca_TextInput *input_for_node(Ca_Window *win, Ca_Node *n)
{
    if (!win->input_pool) return NULL;
    for (uint32_t i = 0; i < CA_MAX_INPUTS_PER_WINDOW; ++i)
        if (win->input_pool[i].in_use && win->input_pool[i].node == n)
            return &win->input_pool[i];
    return NULL;
}

/* Find the Ca_Button for a given node (or NULL) */
static Ca_Button *button_for_node(Ca_Window *win, Ca_Node *n)
{
    if (!win->button_pool) return NULL;
    for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i)
        if (win->button_pool[i].in_use && win->button_pool[i].node == n)
            return &win->button_pool[i];
    return NULL;
}

/* Collect all focusable nodes in document order */
static void collect_focusable(Ca_Node *node, Ca_Node **out, int *count, int max,
                              Ca_Window *win)
{
    if (!node || !node->in_use || node->desc.hidden || node->desc.disabled) return;
    if (is_focusable_node(win, node) && *count < max)
        out[(*count)++] = node;
    for (uint32_t i = 0; i < node->child_count; ++i)
        collect_focusable(node->children[i], out, count, max, win);
}

/* Encode a Unicode codepoint as UTF-8, return bytes written (1-4) */
static int utf8_encode(uint32_t cp, char *buf)
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/** Returns whether a typed codepoint is valid at the numeric input cursor. */
static bool numeric_input_accepts(const Ca_TextInput *input, uint32_t cp)
{
    if (!input || input->input_mode == CA_INPUT_TEXT) return true;
    if (cp >= '0' && cp <= '9') return true;
    if (input->input_mode == CA_INPUT_UINT) return false;

    if ((cp == '-' || cp == '+') && input->cursor == 0)
        return input->text[0] != '-' && input->text[0] != '+';

    return input->input_mode == CA_INPUT_FLOAT && cp == '.' &&
           strchr(input->text, '.') == NULL;
}

/** Writes a dragged numeric value using the input mode's canonical format. */
static void numeric_input_set_drag_value(Ca_TextInput *input, double value)
{
    if (!input) return;

    char text[CA_INPUT_TEXT_MAX];
    switch (input->input_mode) {
    case CA_INPUT_FLOAT:
        snprintf(text, sizeof(text), "%.3f", value);
        break;
    case CA_INPUT_INT:
        snprintf(text, sizeof(text), "%.0f", value);
        break;
    case CA_INPUT_UINT:
        if (value < 0.0) value = 0.0;
        snprintf(text, sizeof(text), "%.0f", value);
        break;
    default:
        return;
    }

    if (strcmp(input->text, text) == 0) return;
    snprintf(input->text, sizeof(input->text), "%s", text);
    input->cursor = (int)strlen(input->text);
    input->sel_start = -1;
    input->node->dirty |= CA_DIRTY_CONTENT;
    if (input->on_change)
        input->on_change(input, input->change_data);
}

/* Handle keyboard input for a focused text input */
static void input_handle_keys(Ca_Window *win, Ca_TextInput *inp)
{
    int len = (int)strlen(inp->text);
    bool changed = false;

    /* Process typed characters */
    for (uint32_t i = 0; i < win->char_count; ++i) {
        uint32_t cp = win->char_buf[i];
        if (cp < 32) continue; /* skip control chars */
        if (!numeric_input_accepts(inp, cp)) continue;

        char encoded[4];
        int enc_len = utf8_encode(cp, encoded);
        if (len + enc_len < CA_INPUT_TEXT_MAX - 1) {
            /* Insert at cursor */
            memmove(inp->text + inp->cursor + enc_len,
                    inp->text + inp->cursor,
                    (size_t)(len - inp->cursor + 1));
            memcpy(inp->text + inp->cursor, encoded, enc_len);
            inp->cursor += enc_len;
            len += enc_len;
            changed = true;
        }
    }

    /* Process key events */
    for (uint32_t i = 0; i < win->key_count; ++i) {
        int key  = win->key_buf[i];
        int mods = win->key_mods_buf[i];
        (void)mods;
        len = (int)strlen(inp->text);

        if (key == 259 /* GLFW_KEY_BACKSPACE */) {
            if (inp->cursor > 0) {
                /* Delete one byte before cursor (ASCII-safe; for UTF-8 multi-byte
                   we'd need to walk back, but for the common case this works) */
                int del_pos = inp->cursor - 1;
                /* Walk back over UTF-8 continuation bytes */
                while (del_pos > 0 && (inp->text[del_pos] & 0xC0) == 0x80)
                    del_pos--;
                int del_len = inp->cursor - del_pos;
                memmove(inp->text + del_pos,
                        inp->text + inp->cursor,
                        (size_t)(len - inp->cursor + 1));
                inp->cursor = del_pos;
                changed = true;
            }
        } else if (key == 261 /* GLFW_KEY_DELETE */) {
            if (inp->cursor < len) {
                /* Find length of char at cursor */
                int char_len = 1;
                unsigned char c = (unsigned char)inp->text[inp->cursor];
                if (c >= 0xC0 && c < 0xE0) char_len = 2;
                else if (c >= 0xE0 && c < 0xF0) char_len = 3;
                else if (c >= 0xF0) char_len = 4;
                if (inp->cursor + char_len > len) char_len = len - inp->cursor;
                memmove(inp->text + inp->cursor,
                        inp->text + inp->cursor + char_len,
                        (size_t)(len - inp->cursor - char_len + 1));
                changed = true;
            }
        } else if (key == 263 /* GLFW_KEY_LEFT */) {
            if (inp->cursor > 0) {
                inp->cursor--;
                while (inp->cursor > 0 && (inp->text[inp->cursor] & 0xC0) == 0x80)
                    inp->cursor--;
            }
        } else if (key == 262 /* GLFW_KEY_RIGHT */) {
            if (inp->cursor < len) {
                inp->cursor++;
                while (inp->cursor < len && (inp->text[inp->cursor] & 0xC0) == 0x80)
                    inp->cursor++;
            }
        } else if (key == 268 /* GLFW_KEY_HOME */) {
            inp->cursor = 0;
        } else if (key == 269 /* GLFW_KEY_END */) {
            inp->cursor = len;
        }
    }

    if (changed) {
        inp->node->dirty |= CA_DIRTY_CONTENT;
        if (inp->on_change)
            inp->on_change(inp, inp->change_data);
    }
}

void ca_widget_input_pass(Ca_Window *win)
{
    float mx = (float)win->mouse_x;
    float my = (float)win->mouse_y;
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    /* Query live OS button state where available.  GLFW's per-window cached
       state can miss releases when borderless-window drags leave the content
       area, so use the platform window helper and clear stale cached state on
       physical release. */
    bool left_down = ca_window_left_button_held(win);
    if (!left_down) win->mouse_buttons[0] = false;

    /* --- Scrollbar drag handling ---
       Scrollbars are paint-only overlays (no nodes).  We hit-test them here
       using the same geometry as paint_scrollbars, and drive scroll_x/scroll_y
       directly.  Scrollbar drag takes priority over wheel scroll and other drags.

       Geometry matches paint_scrollbars. CSS may replace the fallback width;
       both axes retain proportional thumbs and the existing minimum sizes.
    */
    if (win->node_pool) {
        const float SB_X_MARGIN = 2.0f * ui_s;
        const float SB_HIT_EXPAND = 2.0f * ui_s;

        /* --- Start drag on mouse-click in a scrollbar region --- */
        if (left_down && win->mouse_click_this_frame && !win->scrollbar_drag_node) {
            /* Find the innermost overflow-scroll node whose scrollbar was hit */
            float best_area = 1e18f;
            Ca_Node *best = NULL;
            bool best_y = true;

            for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
                Ca_Node *n = &win->node_pool[i];
                if (!n->in_use || node_is_ancestor_hidden(n)) continue;

                /* Y scrollbar */
                if (n->scrollbar_y_visible) {
                    float bar_w = ca_scrollbar_vertical_width(n);
                    float bar_x = n->x + n->w - bar_w;
                    if (mx >= bar_x - SB_HIT_EXPAND &&
                        mx <= bar_x + bar_w + SB_HIT_EXPAND &&
                        my >= n->y &&
                        my <= n->y + ca_scrollbar_viewport_height(n)) {
                        float area = n->w * n->h;
                        if (area < best_area) {
                            best_area = area;
                            best = n;
                            best_y = true;
                        }
                    }
                }

                /* X scrollbar */
                if (n->scrollbar_x_visible) {
                    float bar_h = ca_scrollbar_horizontal_height(n);
                    float bar_y = n->y + n->h - bar_h - SB_X_MARGIN;
                    if (my >= bar_y - SB_HIT_EXPAND &&
                        my <= bar_y + bar_h + SB_HIT_EXPAND &&
                        mx >= n->x + SB_X_MARGIN &&
                        mx <= n->x + ca_scrollbar_viewport_width(n) - SB_X_MARGIN) {
                        float area = n->w * n->h;
                        if (area < best_area) {
                            best_area = area;
                            best = n;
                            best_y = false;
                        }
                    }
                }
            }

            if (best) {
                win->scrollbar_drag_node  = best;
                win->scrollbar_drag_y     = best_y;
                win->mouse_click_this_frame = false; /* consume click — no focus/button activation */

                if (best_y) {
                    /* Compute grab offset from thumb top to mouse click */
                    float track_h  = ca_scrollbar_viewport_height(best);
                    float ratio    = track_h / best->content_h;
                    float thumb_h  = track_h * ratio;
                    if (thumb_h < 20.0f * ui_s) thumb_h = 20.0f * ui_s;
                    if (thumb_h > track_h) thumb_h = track_h;
                    float max_s    = ca_scrollbar_max_y(best);
                    float pct      = (max_s > 0.0f) ? best->scroll_y / max_s : 0.0f;
                    float thumb_y  = best->y + pct * (track_h - thumb_h);
                    /* If click is on the thumb, grab relative to its top.
                       If click is on the track, center the thumb on the mouse. */
                    if (my >= thumb_y && my <= thumb_y + thumb_h)
                        win->scrollbar_drag_grab = my - thumb_y;
                    else
                        win->scrollbar_drag_grab = thumb_h * 0.5f;
                } else {
                    float track_w  = ca_scrollbar_viewport_width(best) -
                                     SB_X_MARGIN * 2.0f;
                    if (track_w < 1.0f) track_w = 1.0f;
                    float ratio    = ca_scrollbar_viewport_width(best) /
                                     best->content_w;
                    float thumb_w  = track_w * ratio;
                    if (thumb_w < 16.0f * ui_s) thumb_w = 16.0f * ui_s;
                    if (thumb_w > track_w) thumb_w = track_w;
                    float max_s    = ca_scrollbar_max_x(best);
                    float pct      = (max_s > 0.0f) ? best->scroll_x / max_s : 0.0f;
                    float thumb_x  = best->x + SB_X_MARGIN + pct * (track_w - thumb_w);
                    if (mx >= thumb_x && mx <= thumb_x + thumb_w)
                        win->scrollbar_drag_grab = mx - thumb_x;
                    else
                        win->scrollbar_drag_grab = thumb_w * 0.5f;
                }
            }
        }

        /* --- Update scroll position while dragging --- */
        if (win->scrollbar_drag_node && left_down) {
            Ca_Node *n = win->scrollbar_drag_node;
            if (win->scrollbar_drag_y) {
                float track_h = ca_scrollbar_viewport_height(n);
                float ratio   = track_h / n->content_h;
                float thumb_h = track_h * ratio;
                if (thumb_h < 20.0f * ui_s) thumb_h = 20.0f * ui_s;
                if (thumb_h > track_h) thumb_h = track_h;
                float travel  = track_h - thumb_h;
                float thumb_y = my - win->scrollbar_drag_grab;
                float pct     = (travel > 0.0f) ? (thumb_y - n->y) / travel
                                                 : 0.0f;
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 1.0f) pct = 1.0f;
                float max_s = ca_scrollbar_max_y(n);
                float new_scroll = pct * max_s;
                if (new_scroll != n->scroll_y) {
                    n->scroll_y = new_scroll;
                    n->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
                    ca_node_sync_scroll_y_signal(n);
                }
            } else {
                float track_w = ca_scrollbar_viewport_width(n) -
                                SB_X_MARGIN * 2.0f;
                if (track_w < 1.0f) track_w = 1.0f;
                float ratio   = ca_scrollbar_viewport_width(n) / n->content_w;
                float thumb_w = track_w * ratio;
                if (thumb_w < 16.0f * ui_s) thumb_w = 16.0f * ui_s;
                if (thumb_w > track_w) thumb_w = track_w;
                float travel  = track_w - thumb_w;
                float thumb_x = mx - win->scrollbar_drag_grab;
                float pct     = (travel > 0.0f)
                    ? (thumb_x - (n->x + SB_X_MARGIN)) / travel
                                                 : 0.0f;
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 1.0f) pct = 1.0f;
                float max_s = ca_scrollbar_max_x(n);
                float new_scroll = pct * max_s;
                if (new_scroll != n->scroll_x) {
                    n->scroll_x = new_scroll;
                    n->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
                }
            }
        }

        /* --- End drag on mouse release --- */
        if (!left_down && win->scrollbar_drag_node) {
            if (win->scrollbar_drag_node) {
                /* Force repaint so thumb returns to normal color */
                win->scrollbar_drag_node->dirty |= CA_DIRTY_CONTENT;
            }
            win->scrollbar_drag_node = NULL;
        }
    }

    /* --- Draggable numeric inputs --- */
    if (win->input_pool) {
        if (win->numeric_drag_input &&
            (!win->numeric_drag_input->in_use ||
             !win->numeric_drag_input->node ||
             is_effectively_disabled(win->numeric_drag_input->node))) {
            ca_window_set_default_cursor(win);
            win->numeric_drag_input = NULL;
            win->numeric_drag_active = false;
        }

        if (left_down && win->mouse_click_this_frame &&
            !win->numeric_drag_input) {
            for (uint32_t i = 0; i < CA_MAX_INPUTS_PER_WINDOW; ++i) {
                Ca_TextInput *input = &win->input_pool[i];
                if (!input->in_use || !input->node ||
                    input->input_mode == CA_INPUT_TEXT ||
                    is_effectively_disabled(input->node))
                    continue;
                if (!point_within_clip_ancestors(input->node, mx, my) ||
                    !point_in_node(input->node, mx, my))
                    continue;

                char *end = NULL;
                double value = strtod(input->text, &end);
                if (end == input->text || (end && *end != '\0')) value = 0.0;
                win->numeric_drag_input = input;
                win->numeric_drag_active = false;
                win->numeric_drag_start_x = mx;
                win->numeric_drag_start_value = value;
                break;
            }
        }

        if (left_down && win->numeric_drag_input) {
            Ca_TextInput *input = win->numeric_drag_input;
            float delta = mx - win->numeric_drag_start_x;
            if (delta <= -2.0f || delta >= 2.0f) {
                win->numeric_drag_active = true;
                ca_window_set_horizontal_drag_cursor(win);
                double value = win->numeric_drag_start_value +
                               (double)delta * (double)input->drag_speed;
                numeric_input_set_drag_value(input, value);
            }
        }

        if (!left_down && win->numeric_drag_input) {
            if (win->numeric_drag_active) {
                if (win->focused_node == win->numeric_drag_input->node)
                    win->focused_node = NULL;
                ca_window_set_default_cursor(win);
            }
            win->numeric_drag_input = NULL;
            win->numeric_drag_active = false;
        }
    }

    /* --- Scroll wheel handling (skipped if dragging scrollbar) --- */
    if (win->scroll_this_frame && win->node_pool && !win->scrollbar_drag_node) {
        const float SCROLL_SPEED = 30.0f * ui_s;

        /* First: if any select dropdown is open and the cursor is over it, scroll its list */
        bool select_scroll_consumed = false;
        if (win->select_pool) {
            for (uint32_t i = 0; i < CA_MAX_SELECTS_PER_WINDOW; ++i) {
                Ca_Select *sel = &win->select_pool[i];
                if (!sel->in_use || !sel->node || !sel->open) continue;
                /* Force-close if the host panel is hidden */
                if (node_is_ancestor_hidden(sel->node)) {
                    sel->open = false;
                    continue;
                }
                /* Hit-test the visible dropdown panel */
                float drop_y = sel->node->y + sel->node->h;
                int visible = sel->option_count < CA_SELECT_MAX_VISIBLE
                              ? sel->option_count : CA_SELECT_MAX_VISIBLE;
                float drop_h = sel->node->h * (float)visible;
                if (mx >= sel->node->x && mx <= sel->node->x + sel->node->w &&
                    my >= drop_y && my <= drop_y + drop_h) {
                    int max_offset = sel->option_count - visible;
                    if (max_offset < 0) max_offset = 0;
                    /* Accumulate fractional scroll for smooth trackpad support */
                    sel->scroll_accum += (float)win->scroll_dy;
                    int steps = (int)sel->scroll_accum;
                    if (steps != 0) {
                        sel->scroll_offset -= steps;
                        sel->scroll_accum  -= (float)steps;
                    }
                    if (sel->scroll_offset < 0) sel->scroll_offset = 0;
                    if (sel->scroll_offset > max_offset) sel->scroll_offset = max_offset;
                    sel->node->dirty |= CA_DIRTY_CONTENT;
                    select_scroll_consumed = true;
                    break;
                }
            }
        }

        /* Then: normal scroll container handling (skipped if dropdown consumed scroll) */
        if (!select_scroll_consumed) {
            /* First try custom scroll callbacks (e.g. node graph canvas zoom) */
            Ca_Node *scroll_cb_node = NULL;
            float min_area = 1e30f;
            for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
                Ca_Node *n = &win->node_pool[i];
                if (!n->in_use || !n->scroll_fn) continue;
                if (!point_in_node(n, mx, my)) continue;
                float area = n->w * n->h;
                if (area < min_area) { min_area = area; scroll_cb_node = n; }
            }
            if (scroll_cb_node) {
                ((Ca_ScrollFn)scroll_cb_node->scroll_fn)(
                    win->scroll_dx, win->scroll_dy, scroll_cb_node->scroll_data);
            } else {
                /* Standard overflow scroll containers */
                Ca_Node *scroll_target = NULL;
                for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
                    Ca_Node *n = &win->node_pool[i];
                    if (!n->in_use) continue;
                    if (n->desc.overflow_y < 2) continue;
                    if (!point_in_node(n, mx, my)) continue;
                    if (!scroll_target || (n->w * n->h < scroll_target->w * scroll_target->h))
                        scroll_target = n;
                }
                if (scroll_target) {
                    scroll_target->scroll_y -= (float)win->scroll_dy * SCROLL_SPEED;
                    float max_scroll = ca_scrollbar_max_y(scroll_target);
                    if (scroll_target->scroll_y < 0) scroll_target->scroll_y = 0;
                    if (scroll_target->scroll_y > max_scroll) scroll_target->scroll_y = max_scroll;
                    scroll_target->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
                    ca_node_sync_scroll_y_signal(scroll_target);
                }
            }
        }
    }

    /* --- Tab focus navigation --- */
    for (uint32_t ki = 0; ki < win->key_count; ++ki) {
        int key = win->key_buf[ki];
        if (key != 258 /* GLFW_KEY_TAB */) continue;

        Ca_Node *focusable[256];
        int fcount = 0;
        if (win->root)
            collect_focusable(win->root, focusable, &fcount, 256, win);
        if (fcount == 0) break;

        bool shift = (win->key_mods_buf[ki] & 0x0001) != 0; /* GLFW_MOD_SHIFT */
        int cur_idx = -1;
        for (int i = 0; i < fcount; ++i) {
            if (focusable[i] == win->focused_node) { cur_idx = i; break; }
        }

        int next_idx;
        if (shift)
            next_idx = (cur_idx <= 0) ? fcount - 1 : cur_idx - 1;
        else
            next_idx = (cur_idx < 0 || cur_idx >= fcount - 1) ? 0 : cur_idx + 1;

        Ca_Node *old_focus = win->focused_node;
        win->focused_node = focusable[next_idx];
        if (old_focus != win->focused_node) {
            if (old_focus) old_focus->dirty |= CA_DIRTY_CONTENT;
            win->focused_node->dirty |= CA_DIRTY_CONTENT;
        }
        break; /* consume only the first Tab */
    }

    /* --- Enter/Space to activate focused button --- */
    if (win->focused_node && !is_effectively_disabled(win->focused_node)) {
        Ca_Button *fbtn = button_for_node(win, win->focused_node);
        if (fbtn && fbtn->on_click) {
            for (uint32_t ki = 0; ki < win->key_count; ++ki) {
                int key = win->key_buf[ki];
                if (key == 257 /* ENTER */ || key == 32 /* SPACE */) {
                    /* Keyboard activation has no spatial position; mark
                       the click pos invalid so handlers fall back to a
                       sensible default (typically end-of-line). */
                    fbtn->last_click_valid = false;
                    fbtn->last_click_x = 0.0f;
                    fbtn->last_click_y = 0.0f;
                    fbtn->on_click(fbtn, fbtn->click_data);
                    break;
                }
            }
        }

        /* --- Keyboard input for focused text input --- */
        Ca_TextInput *finp = input_for_node(win, win->focused_node);
        if (finp) {
            input_handle_keys(win, finp);
        }
    }

    /* --- Menu bar sub-menu hover tracking (runs every frame) --- */
    if (win->menubar_pool) {
        for (uint32_t i = 0; i < CA_MAX_MENUBARS_PER_WINDOW; ++i) {
            Ca_MenuBar *mb = &win->menubar_pool[i];
            if (!mb->in_use || !mb->node || mb->active_menu < 0) continue;
            Ca_MenuBarMenu *am = &mb->menus[mb->active_menu];
            Ca_Node *hdr = am->header_node;
            if (!hdr) continue;
            const float sep_h = 8.0f * ui_s;
            float item_h = 24.0f * ui_s;
            float menu_w = 180.0f * ui_s;
            float menu_h = 6.0f * ui_s;
            for (int ii = 0; ii < am->item_count; ++ii)
                menu_h += am->items[ii].separator ? sep_h : item_h;
            float drop_x = 0.0f;
            float drop_y = 0.0f;
            ca_menubar_dropdown_geometry(win, hdr, menu_w, menu_h,
                                         &drop_x, &drop_y);
            int new_sub  = -1;
            int hover_item = -1;
            int hover_sub_item = -1;
            float iy = drop_y + 3.0f * ui_s; /* 3px top inset — matches paint pass */
            for (int ii = 0; ii < am->item_count; ++ii) {
                float this_h = am->items[ii].separator ? sep_h : item_h;
                if (!am->items[ii].separator &&
                    mx >= drop_x && mx <= drop_x + menu_w &&
                    my >= iy && my <= iy + this_h) {
                    hover_item = ii;
                    if (am->items[ii].sub_item_count > 0) new_sub = ii;
                }
                iy += this_h;
            }
            /* Keep sub open when mouse is inside the sub-panel */
            if (new_sub < 0 && am->active_sub >= 0) {
                int   asi   = am->active_sub;
                float sub_w = 180.0f * ui_s;
                float parent_offset_y = 3.0f * ui_s;
                for (int jj = 0; jj < asi; ++jj)
                    parent_offset_y += am->items[jj].separator ? sep_h : item_h;
                float sub_h = item_h * (float)am->items[asi].sub_item_count + 6.0f * ui_s;
                float sub_x = 0.0f;
                float sub_y = 0.0f;
                ca_menubar_submenu_geometry(win, drop_x, drop_y,
                                            menu_w, sub_w, sub_h,
                                            parent_offset_y,
                                            &sub_x, &sub_y);
                if (mx >= sub_x && mx <= sub_x + sub_w &&
                    my >= sub_y && my <= sub_y + sub_h) {
                    new_sub = asi;
                    float sub_iy = sub_y + 3.0f * ui_s;
                    for (int si = 0; si < am->items[asi].sub_item_count; ++si) {
                        float siy = sub_iy + item_h * (float)si;
                        if (my >= siy && my <= siy + item_h) {
                            hover_sub_item = si;
                            break;
                        }
                    }
                }
            }
            if (mb->hover_item != hover_item ||
                mb->hover_sub_item != hover_sub_item ||
                am->active_sub != new_sub) {
                mb->hover_item = hover_item;
                mb->hover_sub_item = hover_sub_item;
                mb->node->dirty |= CA_DIRTY_CONTENT;
            }
            am->active_sub = new_sub;
        }
    }

    /* --- Click handling: focus + button activation --- */
    if (win->mouse_click_this_frame) {
        /* Overlay priority: open menu dropdown captures all clicks */
        bool click_consumed = false;
        if (win->menubar_pool) {
            for (uint32_t i = 0; i < CA_MAX_MENUBARS_PER_WINDOW && !click_consumed; ++i) {
                Ca_MenuBar *mb = &win->menubar_pool[i];
                if (!mb->in_use || !mb->node || mb->active_menu < 0) continue;

                click_consumed = true;
                Ca_MenuBarMenu *am = &mb->menus[mb->active_menu];
                Ca_Node *hdr = am->header_node;
                if (hdr) {
                    const float sep_h = 8.0f * ui_s;
                    float item_h = 24.0f * ui_s;
                    float menu_w = 180.0f * ui_s;
                    float menu_h = 6.0f * ui_s;
                    for (int ii = 0; ii < am->item_count; ++ii)
                        menu_h += am->items[ii].separator ? sep_h : item_h;
                    float drop_x = 0.0f;
                    float drop_y = 0.0f;
                    ca_menubar_dropdown_geometry(win, hdr, menu_w, menu_h,
                                                 &drop_x, &drop_y);
                    bool item_hit = false;

                    /* Check click in open sub-menu panel first */
                    bool sub_hit = false;
                    if (am->active_sub >= 0) {
                        int   asi        = am->active_sub;
                        float sub_item_h = 24.0f * ui_s;
                        float sub_w      = 180.0f * ui_s;
                        float parent_offset_y = 3.0f * ui_s;
                        for (int jj = 0; jj < asi; ++jj)
                            parent_offset_y += am->items[jj].separator ? sep_h : item_h;
                        float sub_h = sub_item_h *
                                      (float)am->items[asi].sub_item_count +
                                      6.0f * ui_s;
                        float sub_x = 0.0f;
                        float sub_y = 0.0f;
                        ca_menubar_submenu_geometry(win, drop_x, drop_y,
                                                    menu_w, sub_w, sub_h,
                                                    parent_offset_y,
                                                    &sub_x, &sub_y);
                        float sub_iy = sub_y + 3.0f * ui_s;
                        for (int si = 0; si < am->items[asi].sub_item_count; ++si) {
                            float siy = sub_iy + sub_item_h * (float)si;
                            if (mx >= sub_x && mx <= sub_x + sub_w &&
                                my >= siy  && my <= siy + sub_item_h) {
                                Ca_MenuBarSubItem *sitem = &am->items[asi].sub_items[si];
                                am->active_sub  = -1;
                                mb->active_menu = -1;
                                mb->hover_item = -1;
                                mb->hover_sub_item = -1;
                                mb->node->dirty |= CA_DIRTY_CONTENT;
                                if (sitem->action)
                                    sitem->action(sitem->action_data);
                                sub_hit  = true;
                                item_hit = true;
                                break;
                            }
                        }
                    }

                    if (!sub_hit) {
                        float iy = drop_y + 3.0f * ui_s; /* 3px top inset — matches paint pass */
                        for (int ii = 0; ii < am->item_count; ++ii) {
                            float this_h = am->items[ii].separator ? sep_h : item_h;
                            if (mx >= drop_x && mx <= drop_x + menu_w &&
                                my >= iy && my <= iy + this_h) {
                                /* Separator: consume click but don't close */
                                if (am->items[ii].separator) {
                                    item_hit = true;
                                    break;
                                }
                                /* Item with sub-menu: hover handles it, click is a no-op */
                                if (am->items[ii].sub_item_count > 0) {
                                    item_hit = true;
                                    break;
                                }
                                am->active_sub  = -1;
                                mb->active_menu = -1;
                                mb->hover_item = -1;
                                mb->hover_sub_item = -1;
                                mb->node->dirty |= CA_DIRTY_CONTENT;
                                if (am->items[ii].action)
                                    am->items[ii].action(am->items[ii].action_data);
                                item_hit = true;
                                break;
                            }
                            iy += this_h;
                        }
                    }

                    if (!item_hit) {
                        /* Check if clicked another menu header */
                        bool switched = false;
                        for (int mi = 0; mi < mb->menu_count; ++mi) {
                            if (mi == mb->active_menu) continue;
                            if (mb->menus[mi].header_node &&
                                point_in_node(mb->menus[mi].header_node, mx, my)) {
                                am->active_sub  = -1;
                                mb->active_menu = mi;
                                mb->hover_item = -1;
                                mb->hover_sub_item = -1;
                                mb->node->dirty |= CA_DIRTY_CONTENT;
                                switched = true;
                                break;
                            }
                        }
                        if (!switched) {
                            /* Click anywhere else closes dropdown */
                            am->active_sub  = -1;
                            mb->active_menu = -1;
                            mb->hover_item = -1;
                            mb->hover_sub_item = -1;
                            mb->node->dirty |= CA_DIRTY_CONTENT;
                        }
                    }
                }
            }
        }

        if (!click_consumed && win->ctxmenu_pool) {
            /* Context menu item clicks — checked before normal widget clicks.
               One menu can be open at a time.  If the click lands on a menu
               item we fire the callback and consume the event; if it lands
               outside we just close the menu without consuming, so the click
               can still reach the tree node / button underneath. */
            const float CTX_ITEM_H = 24.0f * ui_s;
            const float CTX_SEP_H  =  8.0f * ui_s;
            const float CTX_MENU_W = 180.0f * ui_s;
            for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW; ++i) {
                Ca_CtxMenu *cm = &win->ctxmenu_pool[i];
                if (!cm->in_use || !cm->open) continue;

                /* Compute menu height (mirrors paint.c logic) */
                float menu_h = 6.0f * ui_s; /* top + bottom inset */
                for (int mi = 0; mi < cm->item_count; ++mi) {
                    bool is_sep = (cm->items[mi][0] == '-' && cm->items[mi][1] == '\0');
                    menu_h += is_sep ? CTX_SEP_H : CTX_ITEM_H;
                }

                /* Apply same screen-edge clamping as paint to get display bounds */
                float mx_pos = cm->open_x;
                float my_pos = cm->open_y;
                if ((int)win->sc.extent.width  > 0 && mx_pos + CTX_MENU_W > (float)win->sc.extent.width)
                    mx_pos = (float)win->sc.extent.width  - CTX_MENU_W;
                if ((int)win->sc.extent.height > 0 && my_pos + menu_h > (float)win->sc.extent.height)
                    my_pos = (float)win->sc.extent.height - menu_h;
                if (mx_pos < 0.0f) mx_pos = 0.0f;
                if (my_pos < 0.0f) my_pos = 0.0f;

                bool inside_menu = (mx >= mx_pos && mx <= mx_pos + CTX_MENU_W &&
                                    my >= my_pos && my <= my_pos + menu_h);

                cm->open = false;
                cm->hover_index = -1;
                if (cm->node) cm->node->dirty |= CA_DIRTY_CONTENT;

                if (inside_menu) {
                    float iy = my_pos + 3.0f * ui_s; /* top inset (mirrors paint) */
                    for (int mi = 0; mi < cm->item_count; ++mi) {
                        bool is_sep = (cm->items[mi][0] == '-' && cm->items[mi][1] == '\0');
                        float this_h = is_sep ? CTX_SEP_H : CTX_ITEM_H;
                        if (!is_sep && my >= iy && my <= iy + this_h) {
                            if (cm->on_select)
                                cm->on_select(mi, cm->select_data);
                            break;
                        }
                        iy += this_h;
                    }
                    click_consumed = true;
                }
                break; /* only one menu open at a time */
            }
        }

        if (!click_consumed) {
        /* Click on an input or button focuses it */
        Ca_Node *clicked_focus = NULL;

        if (win->input_pool) {
            for (uint32_t i = 0; i < CA_MAX_INPUTS_PER_WINDOW; ++i) {
                Ca_TextInput *inp = &win->input_pool[i];
                if (!inp->in_use || !inp->node) continue;
                if (is_effectively_disabled(inp->node)) continue;
                if (point_in_node(inp->node, mx, my)) {
                    clicked_focus = inp->node;
                    break;
                }
            }
        }

        if (!clicked_focus && win->button_pool) {
            for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i) {
                Ca_Button *btn = &win->button_pool[i];
                if (!btn->in_use || !btn->node) continue;
                if (!btn->keyboard_focusable) continue;
                if (is_effectively_disabled(btn->node)) continue;
                if (point_in_node(btn->node, mx, my)) {
                    clicked_focus = btn->node;
                    break;
                }
            }
        }

        /* Update focus */
        Ca_Node *old_focus = win->focused_node;
        win->focused_node = clicked_focus; /* NULL if clicked empty space = defocus */
        if (old_focus != win->focused_node) {
            if (old_focus) old_focus->dirty |= CA_DIRTY_CONTENT;
            if (win->focused_node) win->focused_node->dirty |= CA_DIRTY_CONTENT;
        }

        /* Fire button callbacks — z-aware: when buttons overlap, only fire
           the one(s) at the highest z_index so overlay widgets don't leak
           clicks through to content rendered beneath them. */
        if (win->button_pool) {
            /* Pass 1: find the maximum z_index among matching buttons. */
            int16_t top_z = INT16_MIN;
            for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i) {
                Ca_Button *btn = &win->button_pool[i];
                if (!btn->in_use || !btn->on_click || !btn->node) continue;
                if (is_effectively_disabled(btn->node)) continue;
                if (!point_in_node(btn->node, mx, my)) continue;
                if (btn->node->desc.z_index > top_z)
                    top_z = btn->node->desc.z_index;
            }
            /* Pass 2: fire only buttons at the winning z_index. */
            for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i) {
                Ca_Button *btn = &win->button_pool[i];
                if (!btn->in_use || !btn->on_click || !btn->node) continue;
                if (is_effectively_disabled(btn->node)) continue;
                if (btn->node->desc.z_index != top_z) continue;
                if (point_in_node(btn->node, mx, my)) {
                    btn->last_click_x     = mx - btn->node->x;
                    btn->last_click_y     = my - btn->node->y;
                    btn->last_click_valid = true;
                    btn->on_click(btn, btn->click_data);
                }
            }
        }

        /* Checkbox toggle */
        if (win->checkbox_pool) {
            for (uint32_t i = 0; i < CA_MAX_CHECKBOXES_PER_WINDOW; ++i) {
                Ca_Checkbox *cb = &win->checkbox_pool[i];
                if (!cb->in_use || !cb->node) continue;
                if (is_effectively_disabled(cb->node)) continue;
                if (point_in_node(cb->node, mx, my)) {
                    cb->checked = !cb->checked;
                    cb->node->dirty |= CA_DIRTY_CONTENT;
                    if (cb->on_change) cb->on_change(cb, cb->change_data);
                }
            }
        }

        /* Radio select */
        if (win->radio_pool) {
            for (uint32_t i = 0; i < CA_MAX_RADIOS_PER_WINDOW; ++i) {
                Ca_Radio *r = &win->radio_pool[i];
                if (!r->in_use || !r->node) continue;
                if (is_effectively_disabled(r->node)) continue;
                if (point_in_node(r->node, mx, my)) {
                    /* Deselect all radios in the same group */
                    for (uint32_t j = 0; j < CA_MAX_RADIOS_PER_WINDOW; ++j) {
                        Ca_Radio *o = &win->radio_pool[j];
                        if (o->in_use && o->group == r->group && o->value) {
                            o->value = 0;
                            o->node->dirty |= CA_DIRTY_CONTENT;
                        }
                    }
                    r->value = 1;
                    r->node->dirty |= CA_DIRTY_CONTENT;
                    if (r->on_change) r->on_change((Ca_Checkbox*)r, r->change_data);
                }
            }
        }

        /* Toggle */
        if (win->toggle_pool) {
            for (uint32_t i = 0; i < CA_MAX_TOGGLES_PER_WINDOW; ++i) {
                Ca_Toggle *t = &win->toggle_pool[i];
                if (!t->in_use || !t->node) continue;
                if (is_effectively_disabled(t->node)) continue;
                if (point_in_node(t->node, mx, my)) {
                    t->on = !t->on;
                    t->node->dirty |= CA_DIRTY_CONTENT;
                    if (t->on_change) t->on_change(t, t->change_data);
                }
            }
        }

        /* Select dropdown — toggle open/close, or pick option */
        if (win->select_pool) {
            bool select_handled = false;
            for (uint32_t i = 0; i < CA_MAX_SELECTS_PER_WINDOW; ++i) {
                Ca_Select *sel = &win->select_pool[i];
                if (!sel->in_use || !sel->node) continue;
                if (is_effectively_disabled(sel->node)) continue;
                /* Force-close and skip selects inside hidden panels */
                if (node_is_ancestor_hidden(sel->node)) {
                    sel->open = false;
                    continue;
                }
                if (sel->open) {
                    /* Check if clicked on a visible dropdown option */
                    float opt_y = sel->node->y + sel->node->h;
                    float opt_h = sel->node->h;
                    int   visible = sel->option_count < CA_SELECT_MAX_VISIBLE
                                    ? sel->option_count : CA_SELECT_MAX_VISIBLE;
                    int   scroll  = sel->scroll_offset;
                    if (scroll > sel->option_count - visible) scroll = sel->option_count - visible;
                    if (scroll < 0) scroll = 0;
                    bool clicked_option = false;
                    for (int vi = 0; vi < visible; ++vi) {
                        float oy = opt_y + opt_h * (float)vi;
                        if (mx >= sel->node->x && mx <= sel->node->x + sel->node->w &&
                            my >= oy && my <= oy + opt_h) {
                            sel->selected = scroll + vi;
                            sel->open = false;
                            sel->hover_item = -1;
                            sel->scroll_accum = 0.0f;
                            sel->node->dirty |= CA_DIRTY_CONTENT;
                            if (sel->on_change) sel->on_change(sel, sel->change_data);
                            select_handled = true;
                            clicked_option = true;
                            break;
                        }
                    }
                    if (!clicked_option) {
                        /* Click anywhere else closes the dropdown */
                        sel->open = false;
                        sel->hover_item = -1;
                        sel->scroll_accum = 0.0f;
                        sel->node->dirty |= CA_DIRTY_CONTENT;
                        select_handled = true;
                    }
                } else if (point_in_node(sel->node, mx, my)) {
                    sel->open = true;
                    /* Scroll so selected item is visible */
                    {
                        int visible = sel->option_count < CA_SELECT_MAX_VISIBLE
                                      ? sel->option_count : CA_SELECT_MAX_VISIBLE;
                        if (sel->selected >= sel->scroll_offset + visible)
                            sel->scroll_offset = sel->selected - visible + 1;
                        else if (sel->selected < sel->scroll_offset)
                            sel->scroll_offset = sel->selected;
                    }
                    sel->node->dirty |= CA_DIRTY_CONTENT;
                    select_handled = true;
                }
            }
        }

        /* Menu bar header clicks (no dropdown is open) */
        if (win->menubar_pool) {
            for (uint32_t i = 0; i < CA_MAX_MENUBARS_PER_WINDOW; ++i) {
                Ca_MenuBar *mb = &win->menubar_pool[i];
                if (!mb->in_use || !mb->node || mb->active_menu >= 0) continue;
                for (int mi = 0; mi < mb->menu_count; ++mi) {
                    if (mb->menus[mi].header_node &&
                        point_in_node(mb->menus[mi].header_node, mx, my)) {
                        mb->active_menu = mi;
                        mb->hover_item = -1;
                        mb->hover_sub_item = -1;
                        mb->node->dirty |= CA_DIRTY_CONTENT;
                        break;
                    }
                }
            }
        }

        /* Tab bar clicks */
        if (win->tabbar_pool) {
            for (uint32_t i = 0; i < CA_MAX_TABBARS_PER_WINDOW; ++i) {
                Ca_TabBar *tb = &win->tabbar_pool[i];
                if (!tb->in_use || !tb->node) continue;
                if (is_effectively_disabled(tb->node)) continue;
                for (int ti = 0; ti < tb->count; ++ti) {
                    if (!tb->tab_nodes[ti]) continue;
                    if (point_in_node(tb->tab_nodes[ti], mx, my)) {
                        if (tb->active != ti) {
                            /* Update backgrounds */
                            if (tb->active >= 0 && tb->active < tb->count && tb->tab_nodes[tb->active]) {
                                tb->tab_nodes[tb->active]->desc.background = tb->inactive_bg;
                                tb->tab_nodes[tb->active]->dirty |= CA_DIRTY_CONTENT;
                            }
                            tb->active = ti;
                            tb->tab_nodes[ti]->desc.background = tb->active_bg;
                            tb->tab_nodes[ti]->dirty |= CA_DIRTY_CONTENT;
                            if (tb->on_change) tb->on_change(tb, tb->change_data);
                        }
                        break;
                    }
                }
            }
        }

        /* Tree node expand/collapse */
        if (win->treenode_pool) {
            for (uint32_t i = 0; i < CA_MAX_TREENODES_PER_WINDOW; ++i) {
                Ca_TreeNode *tn = &win->treenode_pool[i];
                if (!tn->in_use || !tn->node) continue;
                if (is_effectively_disabled(tn->node)) continue;
                /* Click on the first child (header row) */
                if (tn->node->child_count > 0) {
                    Ca_Node *hdr = tn->node->children[0];
                    if (point_in_node(hdr, mx, my)) {
                        if (!tn->is_leaf) {
                            tn->expanded = !tn->expanded;
                            /* Hide/show children after the header */
                            for (uint32_t ci = 1; ci < tn->node->child_count; ++ci) {
                                tn->node->children[ci]->desc.hidden = !tn->expanded;
                                tn->node->children[ci]->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
                            }
                            tn->node->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
                        }
                        if (tn->on_toggle) tn->on_toggle(tn, tn->toggle_data);
                    }
                }
            }
        }

        } /* end !click_consumed */

        /* Context menu — right-click detection is below */
    }

    /* --- Right-click for context menus --- */
    if (win->ctxmenu_pool) {
        static bool prev_right = false;
        bool right_now = win->mouse_buttons[1];
        if (right_now && !prev_right) {
            /* Close any currently-open context menu first */
            for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW; ++i) {
                Ca_CtxMenu *cm = &win->ctxmenu_pool[i];
                if (cm->in_use && cm->open) {
                    cm->open = false;
                    cm->hover_index = -1;
                    if (cm->node) cm->node->dirty |= CA_DIRTY_CONTENT;
                }
            }
            /* Find the most specific (smallest-area) context menu under cursor.
               This ensures a tree-node menu wins over its parent container. */
            Ca_CtxMenu *best      = NULL;
            float       best_area = 1e30f;
            for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW; ++i) {
                Ca_CtxMenu *cm = &win->ctxmenu_pool[i];
                if (!cm->in_use || !cm->node) continue;
                if (node_is_ancestor_hidden(cm->node)) continue;
                if (is_effectively_disabled(cm->node)) continue;
                if (!point_within_clip_ancestors(cm->node, mx, my)) continue;
                if (point_in_node(cm->node, mx, my)) {
                    float area = cm->node->w * cm->node->h;
                    if (area < best_area) { best_area = area; best = cm; }
                }
            }
            if (best) {
                best->open   = true;
                best->hover_index = -1;
                best->open_x = mx;
                best->open_y = my;
                if (best->on_open) {
                    best->on_open(mx - best->node->x, my - best->node->y,
                                  mx, my, best->open_data);
                }
                if (best->node) best->node->dirty |= CA_DIRTY_CONTENT;
            }
        }
        prev_right = right_now;
    }

    update_context_menu_hover(win, mx, my, ui_s);

    /* --- Slider drag handling --- */
    if (win->slider_pool) {
        bool left_down = win->mouse_buttons[0];
        if (left_down && !win->drag_node) {
            /* Check if we're starting a drag on a slider */
            for (uint32_t i = 0; i < CA_MAX_SLIDERS_PER_WINDOW; ++i) {
                Ca_Slider *sl = &win->slider_pool[i];
                if (!sl->in_use || !sl->node) continue;
                if (is_effectively_disabled(sl->node)) continue;
                if (point_in_node(sl->node, mx, my)) {
                    win->drag_node = sl->node;
                    win->drag_start_x = mx;
                    win->drag_start_value = sl->value;
                    break;
                }
            }
        }
        if (win->drag_node && left_down) {
            /* Update slider value from drag */
            for (uint32_t i = 0; i < CA_MAX_SLIDERS_PER_WINDOW; ++i) {
                Ca_Slider *sl = &win->slider_pool[i];
                if (!sl->in_use || sl->node != win->drag_node) continue;
                float range = sl->max_val - sl->min_val;
                float pct = (mx - sl->node->x) / sl->node->w;
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 1.0f) pct = 1.0f;
                float new_val = sl->min_val + pct * range;
                if (new_val != sl->value) {
                    sl->value = new_val;
                    sl->node->dirty |= CA_DIRTY_CONTENT;
                    if (sl->on_change) sl->on_change(sl, sl->change_data);
                }
                break;
            }
        }
        if (!left_down && win->drag_node) {
            win->drag_node = NULL;
        }
    }

    /* --- Splitter drag handling --- */
    if (win->splitter_pool) {
        bool left_down = win->mouse_buttons[0];

        /* Start splitter drag */
        if (left_down && win->mouse_click_this_frame) {
            for (uint32_t i = 0; i < CA_MAX_SPLITTERS_PER_WINDOW; ++i) {
                Ca_Splitter *sp = &win->splitter_pool[i];
                if (!sp->in_use || !sp->node) continue;
                Ca_Node *n = sp->node;
                /* Compute the divider bar rect */
                bool is_h = (sp->direction == CA_HORIZONTAL);
                float bar_x, bar_y, bar_w, bar_h;
                if (is_h) {
                    bar_x = n->x + (n->w - sp->bar_size) * sp->ratio;
                    bar_y = n->y;
                    bar_w = sp->bar_size;
                    bar_h = n->h;
                } else {
                    bar_x = n->x;
                    bar_y = n->y + (n->h - sp->bar_size) * sp->ratio;
                    bar_w = n->w;
                    bar_h = sp->bar_size;
                }
                /* Expand hit zone slightly for easier grabbing */
                float expand = 4.0f;
                if (mx >= bar_x - expand && mx <= bar_x + bar_w + expand &&
                    my >= bar_y - expand && my <= bar_y + bar_h + expand) {
                    sp->dragging = true;
                }
            }
        }

        /* Update splitter ratio during drag */
        for (uint32_t i = 0; i < CA_MAX_SPLITTERS_PER_WINDOW; ++i) {
            Ca_Splitter *sp = &win->splitter_pool[i];
            if (!sp->in_use || !sp->dragging) continue;
            if (!left_down) {
                sp->dragging = false;
                continue;
            }
            Ca_Node *n = sp->node;
            bool is_h = (sp->direction == CA_HORIZONTAL);
            float total = is_h ? n->w : n->h;
            if (total <= sp->bar_size) continue;
            float local = is_h ? (mx - n->x) : (my - n->y);
            float new_ratio = local / (total - sp->bar_size);
            if (new_ratio < sp->min_ratio) new_ratio = sp->min_ratio;
            if (new_ratio > sp->max_ratio) new_ratio = sp->max_ratio;
            if (new_ratio != sp->ratio) {
                sp->ratio = new_ratio;
                n->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
                if (sp->on_resize)
                    sp->on_resize(new_ratio, sp->user_data);
            }
        }
    }

    /* --- Generic drag interaction (user-defined drag callbacks on divs) --- */
    {
        /* Start a new user drag */
        if (left_down && win->mouse_click_this_frame && !win->user_drag_node) {
            /* Find the topmost draggable node under the cursor. */
            Ca_Node *best = NULL;
            int best_z = -32768;
            if (win->node_pool) {
                for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
                    Ca_Node *n = &win->node_pool[i];
                    if (!n->in_use || node_is_ancestor_hidden(n)) continue;
                    if (is_effectively_disabled(n)) continue;
                    if (!n->drag_fn_start && !n->drag_fn_move && !n->drag_fn_end) continue;
                    if (!point_within_clip_ancestors(n, mx, my)) continue;
                    if (!point_in_node(n, mx, my)) continue;

                    int z = n->desc.z_index;
                    if (!best || z > best_z ||
                        (z == best_z && node_paints_after(n, best))) {
                        best_z = z;
                        best = n;
                    }
                }
            }
            if (best) {
                win->user_drag_node    = best;
                win->user_drag_start_x = mx;
                win->user_drag_start_y = my;
                win->user_drag_active  = true;
                if (best->drag_fn_start) {
                    Ca_DragEvent ev = {
                        .window  = win,
                        .x = mx, .y = my,
                        .start_x = mx, .start_y = my,
                        .dx = 0, .dy = 0,
                        .local_x = mx - best->x,
                        .local_y = my - best->y,
                        .node_w  = best->w,
                        .node_h  = best->h,
                    };
                    ((Ca_DragFn)best->drag_fn_start)(&ev, best->drag_data);
                }
            }
        }

        /* Continue drag */
        if (win->user_drag_active && left_down && win->user_drag_node) {
            Ca_Node *dn = win->user_drag_node;
            if (dn->drag_fn_move) {
                Ca_DragEvent ev = {
                    .window  = win,
                    .x = mx, .y = my,
                    .start_x = win->user_drag_start_x,
                    .start_y = win->user_drag_start_y,
                    .dx = mx - win->user_drag_start_x,
                    .dy = my - win->user_drag_start_y,
                    .local_x = mx - dn->x,
                    .local_y = my - dn->y,
                    .node_w  = dn->w,
                    .node_h  = dn->h,
                };
                ((Ca_DragFn)dn->drag_fn_move)(&ev, dn->drag_data);
            }
        }

        /* End drag */
        if (win->user_drag_active && !left_down) {
            Ca_Node *dn = win->user_drag_node;
            if (dn && dn->drag_fn_end) {
                Ca_DragEvent ev = {
                    .window  = win,
                    .x = mx, .y = my,
                    .start_x = win->user_drag_start_x,
                    .start_y = win->user_drag_start_y,
                    .dx = mx - win->user_drag_start_x,
                    .dy = my - win->user_drag_start_y,
                    .local_x = dn ? mx - dn->x : 0.0f,
                    .local_y = dn ? my - dn->y : 0.0f,
                    .node_w  = dn ? dn->w : 0.0f,
                    .node_h  = dn ? dn->h : 0.0f,
                };
                ((Ca_DragFn)dn->drag_fn_end)(&ev, dn->drag_data);
            }
            win->user_drag_node   = NULL;
            win->user_drag_active = false;
        }
    }

    /* --- Hover tracking --- */
    win->hovered_node = NULL;

    /* If the cursor is over an active overlay (select dropdown, context menu,
       menu-bar dropdown, or modal) do NOT let hover pierce through to background
       nodes — that would cause tooltips and hover styles to fire on invisible
       elements sitting behind the overlay. */
    bool over_overlay = false;

    /* Ca_Select open dropdown */
    if (!over_overlay && win->select_pool) {
        for (uint32_t i = 0; i < CA_MAX_SELECTS_PER_WINDOW && !over_overlay; ++i) {
            Ca_Select *sel = &win->select_pool[i];
            if (!sel->in_use || !sel->open || !sel->node) continue;
            Ca_Node *sn = sel->node;
            int visible = sel->option_count < CA_SELECT_MAX_VISIBLE
                          ? sel->option_count : CA_SELECT_MAX_VISIBLE;
            float drop_y = sn->y + sn->h;
            float drop_h = sn->h * (float)visible;
            if (mx >= sn->x && mx <= sn->x + sn->w &&
                my >= drop_y  && my <= drop_y + drop_h)
                over_overlay = true;
        }
    }

    /* Ca_CtxMenu open */
    if (!over_overlay && win->ctxmenu_pool) {
        const float cm_item_h = 24.0f * ui_s;
        const float cm_sep_h  =  8.0f * ui_s;
        const float cm_menu_w = 180.0f * ui_s;
        for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW && !over_overlay; ++i) {
            Ca_CtxMenu *cm = &win->ctxmenu_pool[i];
            if (!cm->in_use || !cm->open) continue;
            if (cm->node && node_is_ancestor_hidden(cm->node)) continue;
            float menu_h = 6.0f * ui_s;
            for (int mi = 0; mi < cm->item_count; ++mi) {
                bool is_sep = (cm->items[mi][0] == '-' && cm->items[mi][1] == '\0');
                menu_h += is_sep ? cm_sep_h : cm_item_h;
            }
            float cx = cm->open_x, cy = cm->open_y;
            if (win->sc.extent.width  > 0 && cx + cm_menu_w > (float)win->sc.extent.width)
                cx = (float)win->sc.extent.width  - cm_menu_w;
            if (win->sc.extent.height > 0 && cy + menu_h > (float)win->sc.extent.height)
                cy = (float)win->sc.extent.height - menu_h;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (mx >= cx && mx <= cx + cm_menu_w && my >= cy && my <= cy + menu_h)
                over_overlay = true;
        }
    }

    /* Ca_MenuBar active dropdown (and sub-menu) */
    if (!over_overlay && win->menubar_pool) {
        const float mb_item_h = 24.0f * ui_s;
        const float mb_sep_h  =  8.0f * ui_s;
        const float mb_menu_w = 180.0f * ui_s;
        for (uint32_t i = 0; i < CA_MAX_MENUBARS_PER_WINDOW && !over_overlay; ++i) {
            Ca_MenuBar *mb = &win->menubar_pool[i];
            if (!mb->in_use || mb->active_menu < 0) continue;
            Ca_MenuBarMenu *am = &mb->menus[mb->active_menu];
            Ca_Node *hdr = am->header_node;
            if (!hdr) continue;
            float menu_h = 6.0f * ui_s;
            for (int ii = 0; ii < am->item_count; ++ii)
                menu_h += am->items[ii].separator ? mb_sep_h : mb_item_h;
            float drop_x = 0.0f;
            float drop_y = 0.0f;
            ca_menubar_dropdown_geometry(win, hdr, mb_menu_w, menu_h,
                                         &drop_x, &drop_y);
            if (mx >= drop_x && mx <= drop_x + mb_menu_w &&
                my >= drop_y  && my <= drop_y + menu_h)
                over_overlay = true;
            /* Sub-menu panel */
            if (!over_overlay && am->active_sub >= 0 && am->active_sub < am->item_count) {
                float parent_offset_y = 3.0f * ui_s;
                for (int jj = 0; jj < am->active_sub; ++jj)
                    parent_offset_y += am->items[jj].separator ? mb_sep_h : mb_item_h;
                float sub_h = mb_item_h * (float)am->items[am->active_sub].sub_item_count + 6.0f * ui_s;
                float sub_x = 0.0f;
                float sub_y = 0.0f;
                ca_menubar_submenu_geometry(win, drop_x, drop_y,
                                            mb_menu_w, mb_menu_w, sub_h,
                                            parent_offset_y,
                                            &sub_x, &sub_y);
                if (mx >= sub_x && mx <= sub_x + mb_menu_w &&
                    my >= sub_y  && my <= sub_y + sub_h)
                    over_overlay = true;
            }
        }
    }

    /* Ca_Modal visible — overlay covers the full window */
    if (!over_overlay && win->modal_pool) {
        for (uint32_t i = 0; i < CA_MAX_MODALS_PER_WINDOW && !over_overlay; ++i) {
            Ca_Modal *m = &win->modal_pool[i];
            if (m->in_use && m->visible) over_overlay = true;
        }
    }

    if (!over_overlay && win->node_pool) {
        /* Find the deepest / smallest node under the cursor (most specific hit).
           When two nodes have identical area (common for an auto-sized
           parent that wraps a single child — e.g. a tree-node container
           around its header row), prefer the descendant so CSS :hover
           ancestor-walk semantics still light up classes on the inner
           node (.tree-row, etc.). Pool insertion order isn't reliable
           after frees/reuses so we explicitly test ancestry. */
        Ca_Node *best = NULL;
        float    best_area = 1e18f;

        /* Shared helper: returns true when node n should be considered as a
           hover candidate at (mx, my).  Nodes flagged no_hover are treated
           as transparent to hit-testing (their descendants still qualify). */
#define HOVER_CANDIDATE(n) \
            ((n)->in_use && !(n)->desc.hidden && !(n)->desc.no_hover && \
             point_in_node((n), mx, my) && !node_is_ancestor_hidden(n))

        /* Pass 1 — find the highest effective z-index among all hit nodes
           that are hover-eligible.  This implements CSS stacking-context
           semantics: every node in a z>0 subtree (e.g. the sticky overlay,
           z=5) renders visually above all z=0 content, so it must also win
           hover priority over any z=0 node regardless of area.
           Nodes marked no_hover (e.g. the full-window popup_host overlay)
           are excluded so they cannot inflate max_ez and block everything
           else when no popup is active. */
        int16_t max_ez = 0;
        for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
            Ca_Node *n = &win->node_pool[i];
            if (!HOVER_CANDIDATE(n)) continue;
            int16_t ez = node_effective_z(n);
            if (ez > max_ez) max_ez = ez;
        }

        /* Pass 2 — among nodes whose effective z matches max_ez, pick the
           most-specific one using the original area + descendant logic. */
        for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
            Ca_Node *n = &win->node_pool[i];
            if (!HOVER_CANDIDATE(n)) continue;
            if (node_effective_z(n) != max_ez) continue;
            float area = n->w * n->h;
            if (area < best_area) {
                best_area = area; best = n;
            } else if (best && area == best_area) {
                /* Tie — prefer n if it's a descendant of best (deeper). */
                for (Ca_Node *p = n->parent; p; p = p->parent) {
                    if (p == best) { best = n; break; }
                }
            }
        }

#undef HOVER_CANDIDATE
        /* Tree-node containers wrap their clickable header row as
           children[0].  The container often auto-sizes to the same
           bounds as the header, producing an area-tie that the loop
           above resolves via descendant-preference — but with hidden
           children participating in layout, scroll baking offsets, or
           floating-point edge cases the container can still come out
           on top.  Descend to the header row whenever the deepest hit
           landed on a tree-node container so .tree-row :hover lights
           up reliably (and the geometric hover paint in paint.c picks
           up the resolved CSS background on every frame). */
        while (best && best->widget_type == CA_WIDGET_TREENODE &&
               best->child_count > 0 && best->children[0] &&
               !best->children[0]->desc.hidden &&
               point_in_node(best->children[0], mx, my)) {
            best = best->children[0];
        }
        win->hovered_node = best;
    }

    /* Mark any open select dropdown's node dirty every frame so paint_overlays
       runs and can update hover_item and fire on_hover during mouse motion.
       Dropdown items are overlay draw commands (not Ca_Nodes), so hovered_node
       never changes while the cursor is over them — without this, paint_overlays
       only runs when something else marks a node dirty, causing on_hover to stall. */
    if (win->select_pool) {
        for (uint32_t i = 0; i < CA_MAX_SELECTS_PER_WINDOW; ++i) {
            Ca_Select *sel = &win->select_pool[i];
            if (sel->in_use && sel->open && sel->node)
                sel->node->dirty |= CA_DIRTY_CONTENT;
        }
    }
}
