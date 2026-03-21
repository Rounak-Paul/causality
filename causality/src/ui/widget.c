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
#include "font.h"
#include "viewport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <GLFW/glfw3.h>

/* Measure the pixel width of a text string using the instance font.
   Returns 0 if no font is available. */
static float measure_text_px(Ca_Window *win, const char *text)
{
    if (!text || text[0] == '\0') return 0.0f;
    Ca_Font *font = win->instance->font;
    if (!font) return 0.0f;
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float cs   = font->content_scale / ui_s;
    float w = 0.0f;
    for (const char *p = text; *p; p++) {
        int c = (unsigned char)(*p);
        if (c >= CA_FONT_GLYPH_FIRST &&
            c <  CA_FONT_GLYPH_FIRST + CA_FONT_GLYPH_COUNT)
            w += font->glyphs[c - CA_FONT_GLYPH_FIRST].xadvance / cs;
    }
    return w;
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

static struct {
    Ca_Window *window;
    Ca_Node   *stack[CA_STACK_MAX];
    int        depth;    /* index of top; -1 = empty */
    bool       active;
} g_ctx;

static Ca_Node *ctx_top(void)
{
    assert(g_ctx.active && g_ctx.depth >= 0);
    return g_ctx.stack[g_ctx.depth];
}

static void ctx_push(Ca_Node *node)
{
    assert(g_ctx.depth + 1 < CA_STACK_MAX);
    g_ctx.stack[++g_ctx.depth] = node;
}

static void ctx_pop(void)
{
    assert(g_ctx.depth >= 0);
    --g_ctx.depth;
}

/* Scale a value by the window's UI scale factor */
static float s(float v) { return v * g_ctx.window->ui_scale; }

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

    Ca_Stylesheet *ss = g_ctx.window->instance->stylesheet;
    if (!ss) return;

    Ca_ResolvedStyle rs;
    ca_style_resolve(ss, node, elem_type, node->classes, &rs);

    /* Scale CSS-resolved pixel values before applying (skip percentages) */
    if (!rs.width_pct)  rs.width  = s(rs.width);
    if (!rs.height_pct) rs.height = s(rs.height);
    rs.min_width    = s(rs.min_width);
    rs.max_width    = s(rs.max_width);
    rs.min_height   = s(rs.min_height);
    rs.max_height   = s(rs.max_height);
    rs.padding[0]   = s(rs.padding[0]);
    rs.padding[1]   = s(rs.padding[1]);
    rs.padding[2]   = s(rs.padding[2]);
    rs.padding[3]   = s(rs.padding[3]);
    rs.gap          = s(rs.gap);
    rs.border_radius = s(rs.border_radius);
    rs.margin[0]    = s(rs.margin[0]);
    rs.margin[1]    = s(rs.margin[1]);
    rs.margin[2]    = s(rs.margin[2]);
    rs.margin[3]    = s(rs.margin[3]);

    ca_style_apply_to_node(&rs, nd, out_color);

    /* Store transition config on the node */
    node->transition_duration = rs.transition_duration;
    node->transition_props    = rs.transition_props;

    /* display: none */
    if ((rs.set_mask & (1ULL << CA_CSS_PROP_DISPLAY)) &&
        rs.display == CA_CSS_DISPLAY_NONE)
        nd->hidden = true;
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

    Ca_NodeDesc nd = div_to_nd(root_desc);
    Ca_Node *root  = ca_node_root(window);
    assert(root && "failed to create root node");
    ca_node_set_desc(root, &nd);

    /* Apply CSS to root */
    uint32_t dummy_color = 0;
    apply_css(root, &root->desc, CA_ELEM_DIV,
              root_desc ? root_desc->style : NULL,
              root_desc ? root_desc->id : NULL, &dummy_color);

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
   PUBLIC — <div>
   ============================================================ */

void ca_div_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    Ca_Node *node  = add_container(ctx_top(), &nd);
    assert(node);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_DIV,
              desc ? desc->style : NULL,
              desc ? desc->id : NULL, &dummy);

    /* Store drag callbacks on the node */
    if (desc) {
        node->drag_fn_start = (void *)desc->on_drag_start;
        node->drag_fn_move  = (void *)desc->on_drag;
        node->drag_fn_end   = (void *)desc->on_drag_end;
        node->drag_data     = desc->drag_data;
    }

    ctx_push(node);
}

void ca_div_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

/* ============================================================
   PUBLIC — <p> / <text>  (self-closing leaf)
   ============================================================ */

Ca_Label *ca_text(const Ca_TextDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Label *lbl = add_label(g_ctx.window, ctx_top(), desc);
    if (lbl && lbl->node) {
        apply_css(lbl->node, &lbl->node->desc, CA_ELEM_TEXT,
                  desc->style, desc->id, &lbl->color);
        /* Default height if neither user nor CSS set it.
           Skip for wrapped labels — their height is computed at layout time
           from the actual wrapped line count. */
        if (lbl->node->desc.height <= 0.0f && !lbl->node->desc.text_wrap) {
            float pad_v = lbl->node->desc.padding_top
                        + lbl->node->desc.padding_bottom;
            lbl->node->desc.height = s(16.0f) + pad_v;
        }
        /* Auto-width for padded labels inside row containers (tag-like).
           Labels in column layouts keep width=0 to stretch. */
        if (lbl->node->desc.width <= 0.0f && desc->text && lbl->node->parent) {
            bool parent_is_row = (lbl->node->parent->desc.direction == CA_DIR_ROW);
            float pad_h = lbl->node->desc.padding_left
                        + lbl->node->desc.padding_right;
            if (parent_is_row && pad_h > 0.0f) {
                float tw = measure_text_px(g_ctx.window, desc->text);
                if (tw > 0.0f)
                    lbl->node->desc.width = tw + pad_h;
            }
        }
    }
    return lbl;
}

/* ============================================================
   PUBLIC — <button>  (nestable OR self-closing)
   ============================================================ */

/* Self-closing: ca_btn creates a leaf button with built-in text */
Ca_Button *ca_btn(const Ca_BtnDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Button *btn = add_button(g_ctx.window, ctx_top(), desc);
    if (btn && btn->node) {
        apply_css(btn->node, &btn->node->desc, CA_ELEM_BUTTON,
                  desc->style, desc->id, &btn->text_color);
        /* Auto-width from text if neither user nor CSS set it */
        if (btn->node->desc.width <= 0.0f) {
            float tw = measure_text_px(g_ctx.window, desc->text);
            btn->node->desc.width = tw > 0.0f ? tw + s(16.0f) : s(72.0f);
        }
        if (btn->node->desc.height <= 0.0f) btn->node->desc.height = s(24.0f);
    }
    return btn;
}

/* Nestable: btn_begin pushes the button onto the stack so children
   (text, icons, other elements) are laid out inside the button rect. */
Ca_Button *ca_btn_begin(const Ca_BtnDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Button *btn = add_button(g_ctx.window, ctx_top(), desc);
    assert(btn);
    apply_css(btn->node, &btn->node->desc, CA_ELEM_BUTTON,
              desc->style, desc->id, &btn->text_color);
    /* Nestable buttons auto-size from children; only apply fallback
       if no CSS sets the dimension either. */
    ctx_push(btn->node);
    return btn;
}

void ca_btn_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
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
    Ca_Node *node = add_container(ctx_top(), &nd);
    assert(node);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_LIST,
              desc ? desc->style : NULL,
              desc ? desc->id : NULL, &dummy);

    ctx_push(node);
}

void ca_list_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
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
    Ca_Node *node = add_container(ctx_top(), &nd);
    assert(node);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_LI,
              desc ? desc->style : NULL,
              desc ? desc->id : NULL, &dummy);

    ctx_push(node);
}

void ca_li_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

/* ============================================================
   PUBLIC — <hr>  (horizontal rule — self-closing)
   ============================================================ */

void ca_hr(const Ca_HrDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = {0};
    /* Only set user-specified values; leave zeros for CSS to fill */
    if (desc && desc->thickness > 0.0f) nd.height = s(desc->thickness);
    if (desc && desc->color) nd.background = desc->color;
    Ca_Node *node = add_container(ctx_top(), &nd);
    if (node) {
        uint32_t dummy = 0;
        apply_css(node, &node->desc, CA_ELEM_HR,
                  desc ? desc->style : NULL,
                  desc ? desc->id : NULL, &dummy);
        /* Defaults after CSS */
        if (node->desc.height <= 0.0f) node->desc.height = s(1.0f);
        if (node->desc.background == 0)
            node->desc.background = ca_color(0.3f, 0.3f, 0.3f, 1.0f);
    }
}

/* ============================================================
   PUBLIC — spacer  (invisible spacing element)
   ============================================================ */

void ca_spacer(const Ca_SpacerDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = {0};
    if (desc) {
        nd.width  = s(desc->width);
        nd.height = s(desc->height);
    }
    Ca_Node *node = add_container(ctx_top(), &nd);
    if (node) {
        uint32_t dummy = 0;
        apply_css(node, &node->desc, CA_ELEM_SPACER,
                  desc ? desc->style : NULL,
                  desc ? desc->id : NULL, &dummy);
    }
}

/* ============================================================
   PUBLIC — <input>  (text input field — self-closing leaf)
   ============================================================ */

Ca_TextInput *ca_input(const Ca_InputDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_TextInput *inp = alloc_input(g_ctx.window);
    if (!inp) return NULL;

    Ca_NodeDesc nd = {0};
    nd.width          = s(desc->width);
    nd.height         = s(desc->height);
    nd.background     = desc->background;
    nd.corner_radius  = s(desc->corner_radius);
    nd.padding_top    = s(desc->padding[0]);
    nd.padding_right  = s(desc->padding[1]);
    nd.padding_bottom = s(desc->padding[2]);
    nd.padding_left   = s(desc->padding[3]);

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    inp->node       = node;
    inp->in_use     = true;
    inp->text_color = desc->text_color;
    node->widget_type = CA_WIDGET_TEXT_INPUT;
    node->widget      = inp;
    inp->cursor     = 0;
    inp->sel_start  = -1;
    inp->placeholder_color = ca_color(0.5f, 0.5f, 0.5f, 1.0f);

    if (desc->text)
        snprintf(inp->text, CA_INPUT_TEXT_MAX, "%s", desc->text);
    else
        inp->text[0] = '\0';

    if (desc->placeholder)
        snprintf(inp->placeholder, CA_INPUT_TEXT_MAX, "%s", desc->placeholder);
    else
        inp->placeholder[0] = '\0';

    inp->cursor = (int)strlen(inp->text);

    if (desc->on_change) {
        inp->on_change   = desc->on_change;
        inp->change_data = desc->change_data;
    }

    apply_css(node, &node->desc, CA_ELEM_INPUT,
              desc->style, desc->id, &inp->text_color);

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
}

void ca_label_set_text(Ca_Label *label, const char *text)
{
    assert(label && label->in_use);
    snprintf(label->text, CA_LABEL_TEXT_MAX, "%s", text ? text : "");
    label->node->dirty |= CA_DIRTY_CONTENT;
}

void ca_button_set_text(Ca_Button *button, const char *text)
{
    assert(button && button->in_use);
    snprintf(button->text, CA_BUTTON_TEXT_MAX, "%s", text ? text : "");
    button->node->dirty |= CA_DIRTY_CONTENT;
}

void ca_button_set_background(Ca_Button *button, uint32_t color)
{
    assert(button && button->in_use);
    uint32_t old_color = button->node->desc.background;
    if (old_color != color) {
        maybe_transition(button->node, CA_CSS_PROP_BACKGROUND_COLOR,
                         0, 0, old_color, color);
    }
    button->node->desc.background = color;
    button->node->dirty |= CA_DIRTY_CONTENT;
}

void ca_input_set_text(Ca_TextInput *input, const char *text)
{
    assert(input && input->in_use);
    snprintf(input->text, CA_INPUT_TEXT_MAX, "%s", text ? text : "");
    input->cursor = (int)strlen(input->text);
    input->sel_start = -1;
    input->node->dirty |= CA_DIRTY_CONTENT;
}

const char *ca_input_get_text(const Ca_TextInput *input)
{
    assert(input && input->in_use);
    return input->text;
}

/* ============================================================
   PUBLIC — Checkbox
   ============================================================ */

Ca_Checkbox *ca_checkbox(const Ca_CheckboxDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Checkbox *cb = alloc_checkbox(g_ctx.window);
    if (!cb) return NULL;

    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_ROW;
    nd.height = s(20.0f);

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    cb->node = node;
    cb->in_use = true;
    cb->checked = desc->checked;
    node->widget_type = CA_WIDGET_CHECKBOX;
    node->widget      = cb;
    cb->text_color = 0; /* default white */
    if (desc->text) snprintf(cb->text, CA_LABEL_TEXT_MAX, "%s", desc->text);
    else cb->text[0] = '\0';
    cb->on_change = desc->on_change;
    cb->change_data = desc->change_data;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_CHECKBOX, desc->style, desc->id, &cb->text_color);

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
    Ca_Slider *sl = alloc_slider(g_ctx.window);
    if (!sl) return NULL;

    Ca_NodeDesc nd = {0};
    nd.width  = desc->width > 0 ? s(desc->width) : s(160.0f);
    nd.height = s(20.0f);

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    sl->node = node;
    sl->in_use = true;
    sl->min_val = desc->min;
    node->widget_type = CA_WIDGET_SLIDER;
    node->widget      = sl;
    sl->max_val = desc->max;
    sl->value = desc->value;
    sl->on_change = desc->on_change;
    sl->change_data = desc->change_data;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_SLIDER, desc->style, desc->id, &dummy);
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
    Ca_Toggle *t = alloc_toggle(g_ctx.window);
    if (!t) return NULL;

    Ca_NodeDesc nd = {0};
    nd.width  = s(40.0f);
    nd.height = s(22.0f);

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    t->node = node;
    t->in_use = true;
    t->on = desc->on;
    node->widget_type = CA_WIDGET_TOGGLE;
    node->widget      = t;
    t->on_change = desc->on_change;
    t->change_data = desc->change_data;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TOGGLE, desc->style, desc->id, &dummy);
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
    Ca_Progress *p = alloc_progress(g_ctx.window);
    if (!p) return NULL;

    Ca_NodeDesc nd = {0};
    nd.width  = desc->width > 0 ? s(desc->width) : s(200.0f);
    nd.height = desc->height > 0 ? s(desc->height) : s(8.0f);

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    p->node = node;
    p->in_use = true;
    p->value = desc->value;
    node->widget_type = CA_WIDGET_PROGRESS;
    node->widget      = p;
    p->bar_color = desc->bar_color ? desc->bar_color : ca_color(0.2f, 0.6f, 1.0f, 1.0f);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_PROGRESS, desc->style, desc->id, &dummy);
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

/* ============================================================
   PUBLIC — Select / Dropdown
   ============================================================ */

Ca_Select *ca_select(const Ca_SelectDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Select *sel = alloc_select(g_ctx.window);
    if (!sel) return NULL;

    Ca_NodeDesc nd = {0};
    nd.width  = desc->width > 0 ? s(desc->width) : s(140.0f);
    nd.height = s(26.0f);
    nd.corner_radius = s(4.0f);
    nd.background = ca_color(0.2f, 0.2f, 0.25f, 1.0f);

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    sel->node = node;
    sel->in_use = true;
    sel->selected = desc->selected;
    node->widget_type = CA_WIDGET_SELECT;
    node->widget      = sel;
    sel->open = false;
    sel->option_count = desc->option_count;
    if (sel->option_count > CA_MAX_SELECT_OPTIONS)
        sel->option_count = CA_MAX_SELECT_OPTIONS;
    for (int i = 0; i < sel->option_count; ++i) {
        if (desc->options[i])
            snprintf(sel->options[i], CA_OPTION_TEXT_MAX, "%s", desc->options[i]);
    }
    sel->on_change = desc->on_change;
    sel->change_data = desc->change_data;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_SELECT, desc->style, desc->id, &dummy);
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

/* ============================================================
   PUBLIC — Tab bar
   ============================================================ */

Ca_TabBar *ca_tabs(const Ca_TabBarDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_TabBar *tb = alloc_tabbar(g_ctx.window);
    if (!tb) return NULL;

    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_ROW;

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    tb->node = node;
    tb->in_use = true;
    tb->active = desc->active;
    tb->count = desc->count;
    if (tb->count > CA_MAX_TAB_LABELS) tb->count = CA_MAX_TAB_LABELS;
    for (int i = 0; i < tb->count; ++i) {
        if (desc->labels[i])
            snprintf(tb->labels[i], CA_OPTION_TEXT_MAX, "%s", desc->labels[i]);
        tb->tab_nodes[i] = NULL;
    }
    tb->on_change = desc->on_change;
    tb->change_data = desc->change_data;
    tb->active_bg     = desc->active_bg     ? desc->active_bg     : ca_color(0.3f, 0.3f, 0.4f, 1.0f);
    tb->inactive_bg   = desc->inactive_bg   ? desc->inactive_bg   : ca_color(0.15f, 0.15f, 0.2f, 1.0f);
    tb->active_text   = desc->active_text   ? desc->active_text   : ca_color(1.0f, 1.0f, 1.0f, 1.0f);
    tb->inactive_text = desc->inactive_text ? desc->inactive_text : ca_color(0.6f, 0.6f, 0.6f, 1.0f);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TABBAR, desc->style, desc->id, &dummy);

    /* Create child nodes for each tab header */
    for (int i = 0; i < tb->count; ++i) {
        Ca_NodeDesc tnd = {0};
        float tw = measure_text_px(g_ctx.window, tb->labels[i]);
        tnd.width = (tw > 0 ? tw : s(40.0f)) + s(16.0f);
        tnd.background = (i == tb->active) ? tb->active_bg : tb->inactive_bg;
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
    Ca_Node *node = add_container(ctx_top(), &nd);
    assert(node);
    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TREE,
              desc ? desc->style : NULL, desc ? desc->id : NULL, &dummy);
    ctx_push(node);
}

void ca_tree_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

Ca_TreeNode *ca_tree_node_begin(const Ca_TreeNodeDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_TreeNode *tn = alloc_treenode(g_ctx.window);
    if (!tn) return NULL;

    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_COLUMN;

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    tn->node = node;
    tn->in_use = true;
    tn->expanded = desc->expanded;
    node->widget_type = CA_WIDGET_TREENODE;
    node->widget      = tn;
    tn->text_color = 0;
    if (desc->text) snprintf(tn->text, CA_LABEL_TEXT_MAX, "%s", desc->text);
    else tn->text[0] = '\0';
    tn->on_toggle = desc->on_toggle;
    tn->toggle_data = desc->toggle_data;

    /* Compute depth from parent chain (count tree node ancestors) */
    tn->depth = 0;
    Ca_Node *p = node->parent;
    while (p) {
        if (p->elem_type == CA_ELEM_TREENODE) tn->depth++;
        p = p->parent;
    }

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TREENODE, desc->style, desc->id, &tn->text_color);

    /* Create a header row node for the clickable label */
    Ca_NodeDesc hdr = {0};
    hdr.direction = CA_DIR_ROW;
    hdr.height = s(22.0f);
    hdr.padding_left = s(16.0f) * (float)tn->depth;
    Ca_Node *hdr_node = ca_node_add(node, &hdr);
    (void)hdr_node;

    ctx_push(node);
    return tn;
}

void ca_tree_node_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    /* If tree node starts collapsed, hide all children except the header */
    Ca_Node *node = ctx_top();
    if (node->widget_type == CA_WIDGET_TREENODE) {
        Ca_TreeNode *tn = (Ca_TreeNode *)node->widget;
        if (tn && !tn->expanded) {
            for (uint32_t i = 1; i < node->child_count; ++i)
                node->children[i]->desc.hidden = true;
        }
    }
    ctx_pop();
}

bool ca_tree_node_expanded(const Ca_TreeNode *n)
{
    assert(n && n->in_use);
    return n->expanded;
}

/* ============================================================
   PUBLIC — Table
   ============================================================ */

void ca_table_begin(const Ca_TableDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Table *tbl = alloc_table(g_ctx.window);
    if (!tbl) return;

    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_COLUMN;

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return;

    tbl->node = node;
    tbl->in_use = true;
    tbl->column_count = desc->column_count;
    if (tbl->column_count > 16) tbl->column_count = 16;
    for (int i = 0; i < tbl->column_count; ++i)
        tbl->column_widths[i] = desc->column_widths ? s(desc->column_widths[i]) : s(80.0f);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TABLE, desc->style, desc->id, &dummy);
    ctx_push(node);
}

void ca_table_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

void ca_table_row_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    nd.direction = CA_DIR_ROW;
    Ca_Node *node = add_container(ctx_top(), &nd);
    assert(node);
    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_TABLE_ROW,
              desc ? desc->style : NULL, desc ? desc->id : NULL, &dummy);

    /* Apply column widths from the table ancestor */
    Ca_Node *p = node->parent;
    while (p) {
        if (p->elem_type == CA_ELEM_TABLE) break;
        p = p->parent;
    }

    ctx_push(node);
}

void ca_table_row_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

void ca_table_cell(const Ca_TextDesc *desc)
{
    assert(g_ctx.active && desc);

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

    Ca_Label *lbl = alloc_label(g_ctx.window);
    if (!lbl) return;

    Ca_NodeDesc nd = {0};
    nd.width = cell_w;
    nd.height = s(24.0f);
    nd.padding_left = s(4.0f);
    nd.padding_right = s(4.0f);

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return;

    lbl->node = node;
    lbl->in_use = true;
    lbl->color = desc->color;
    node->widget_type = CA_WIDGET_LABEL;
    node->widget      = lbl;
    if (desc->text) snprintf(lbl->text, CA_LABEL_TEXT_MAX, "%s", desc->text);

    apply_css(node, &node->desc, CA_ELEM_TABLE_CELL, desc->style, desc->id, &lbl->color);
}

/* ============================================================
   PUBLIC — Tooltip (attach to previously created element)
   ============================================================ */

void ca_tooltip(const Ca_TooltipDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Tooltip *tt = alloc_tooltip(g_ctx.window);
    if (!tt) return;

    /* Attach to the last child of the current parent (the element just created) */
    Ca_Node *parent = ctx_top();
    if (parent->child_count == 0) return;
    Ca_Node *target = parent->children[parent->child_count - 1];

    tt->node = target;
    tt->in_use = true;
    if (desc->text) snprintf(tt->text, CA_LABEL_TEXT_MAX, "%s", desc->text);
    else tt->text[0] = '\0';
}

/* ============================================================
   PUBLIC — Menu bar
   ============================================================ */

Ca_MenuBar *ca_menu_bar(const Ca_MenuBarDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_MenuBar *mb = alloc_menubar(g_ctx.window);
    if (!mb) return NULL;

    /* Bar container — dimensions and styling driven by CSS */
    Ca_NodeDesc nd = {0};
    nd.direction = CA_DIR_ROW;

    Ca_Node *bar = ca_node_add(ctx_top(), &nd);
    if (!bar) return NULL;

    mb->node = bar;
    mb->in_use = true;
    mb->active_menu = -1;
    mb->menu_count = desc->menu_count;
    if (mb->menu_count > CA_MAX_MENUS_PER_BAR)
        mb->menu_count = CA_MAX_MENUS_PER_BAR;

    /* Theme colors — use caller-provided or sensible defaults */
    mb->header_highlight = desc->header_highlight ? desc->header_highlight
                         : ca_color(0.25f, 0.25f, 0.28f, 1.0f);
    mb->dropdown_bg      = desc->dropdown_bg ? desc->dropdown_bg
                         : ca_color(0.15f, 0.15f, 0.17f, 0.98f);
    mb->dropdown_border  = desc->dropdown_border ? desc->dropdown_border
                         : ca_color(0.25f, 0.25f, 0.28f, 1.0f);
    mb->dropdown_hover   = desc->dropdown_hover ? desc->dropdown_hover
                         : ca_color(0.25f, 0.25f, 0.30f, 1.0f);
    mb->dropdown_text    = desc->dropdown_text ? desc->dropdown_text
                         : ca_color(0.85f, 0.85f, 0.85f, 1.0f);
    mb->text_color       = desc->text_color ? desc->text_color
                         : ca_color(0.80f, 0.80f, 0.82f, 1.0f);

    uint32_t dummy = 0;
    apply_css(bar, &bar->desc, CA_ELEM_DIV, desc->style, desc->id, &dummy);

    for (int mi = 0; mi < mb->menu_count; ++mi) {
        const Ca_MenuDesc *mdesc = &desc->menus[mi];
        Ca_MenuBarMenu *menu = &mb->menus[mi];

        if (mdesc->label)
            snprintf(menu->label, CA_MENU_LABEL_MAX, "%s", mdesc->label);
        menu->item_count = mdesc->item_count;
        if (menu->item_count > CA_MAX_ITEMS_PER_MENU)
            menu->item_count = CA_MAX_ITEMS_PER_MENU;
        for (int ii = 0; ii < menu->item_count; ++ii) {
            if (mdesc->items[ii].label)
                snprintf(menu->items[ii].label, CA_MENU_LABEL_MAX, "%s",
                         mdesc->items[ii].label);
            menu->items[ii].action = mdesc->items[ii].action;
            menu->items[ii].action_data = mdesc->items[ii].action_data;
        }

        float tw = measure_text_px(g_ctx.window, menu->label);

        /* Header node — padding & alignment via CSS (.menu-bar-item) */
        Ca_NodeDesc hnd = {0};
        hnd.align_items = CA_ALIGN_CENTER;

        Ca_Node *hdr = ca_node_add(bar, &hnd);
        if (!hdr) continue;

        apply_css(hdr, &hdr->desc, CA_ELEM_DIV, "menu-bar-item", NULL, &dummy);
        hdr->desc.width = tw + hdr->desc.padding_left + hdr->desc.padding_right;

        menu->header_node = hdr;

        /* Label inside header */
        Ca_Label *lbl = alloc_label(g_ctx.window);
        if (lbl) {
            Ca_NodeDesc lnd = {0};
            lnd.width = tw;
            Ca_Node *ln = ca_node_add(hdr, &lnd);
            if (ln) {
                ln->widget_type = CA_WIDGET_LABEL;
                ln->widget = lbl;
                lbl->node = ln;
                lbl->in_use = true;
                lbl->color = mb->text_color;
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
    Ca_CtxMenu *cm = alloc_ctxmenu(g_ctx.window);
    if (!cm) return;

    Ca_Node *parent = ctx_top();
    if (parent->child_count == 0) return;
    Ca_Node *target = parent->children[parent->child_count - 1];

    cm->node = target;
    cm->in_use = true;
    cm->open = false;
    cm->item_count = desc->item_count;
    if (cm->item_count > CA_MAX_CTXMENU_ITEMS)
        cm->item_count = CA_MAX_CTXMENU_ITEMS;
    for (int i = 0; i < cm->item_count; ++i) {
        if (desc->items[i])
            snprintf(cm->items[i], CA_OPTION_TEXT_MAX, "%s", desc->items[i]);
    }
    cm->on_select = desc->on_select;
    cm->select_data = desc->select_data;
}

/* ============================================================
   PUBLIC — Modal / Dialog
   ============================================================ */

void ca_modal_begin(const Ca_ModalDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Modal *m = alloc_modal(g_ctx.window);
    if (!m) return;

    Ca_NodeDesc nd = {0};
    /* The modal takes full parent size, overlay renders in paint */
    nd.hidden = !desc->visible;

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return;

    m->node = node;
    m->in_use = true;
    m->visible = desc->visible;
    m->overlay_color = desc->overlay_color
        ? desc->overlay_color
        : ca_color(0.0f, 0.0f, 0.0f, 0.5f);

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_MODAL, desc->style, desc->id, &dummy);
    ctx_push(node);
}

void ca_modal_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

/* ============================================================
   PUBLIC — Splitter (resizable split container)
   ============================================================ */

Ca_Splitter *ca_split_begin(const Ca_SplitDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Splitter *sp = alloc_splitter(g_ctx.window);
    if (!sp) return NULL;

    Ca_NodeDesc nd = {0};
    nd.direction = dir_from_int(desc->direction);
    /* The splitter itself fills available space by default (width/height = 0) */

    Ca_Node *node = ca_node_add(ctx_top(), &nd);
    if (!node) return NULL;

    sp->node      = node;
    sp->in_use    = true;
    sp->direction = desc->direction;
    sp->ratio     = (desc->ratio > 0.0f && desc->ratio < 1.0f) ? desc->ratio : 0.5f;
    sp->min_ratio = (desc->min_ratio > 0.0f) ? desc->min_ratio : 0.1f;
    sp->max_ratio = (desc->max_ratio > 0.0f) ? desc->max_ratio : 0.9f;
    sp->bar_size  = (desc->bar_size > 0.0f)  ? s(desc->bar_size)  : s(4.0f);
    sp->bar_color = desc->bar_color ? desc->bar_color : ca_color(0.25f, 0.25f, 0.3f, 1.0f);
    sp->bar_hover_color = desc->bar_hover_color
        ? desc->bar_hover_color : ca_color(0.35f, 0.55f, 0.9f, 1.0f);
    sp->dragging  = false;

    node->widget_type = CA_WIDGET_SPLITTER;
    node->widget      = sp;

    uint32_t dummy = 0;
    apply_css(node, &node->desc, CA_ELEM_SPLITTER,
              desc->style, desc->id, &dummy);

    ctx_push(node);
    return sp;
}

void ca_split_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
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

    Ca_Node *parent = ctx_top();
    if (!parent) return;

    Ca_Image *img = desc->image;
    float w = desc->width  > 0 ? s(desc->width)  : (float)img->width;
    float h = desc->height > 0 ? s(desc->height) : (float)img->height;

    Ca_NodeDesc nd = {0};
    nd.width  = w;
    nd.height = h;
    nd.corner_radius = s(desc->corner_radius);

    Ca_Node *node = ca_node_add(parent, &nd);
    if (!node) return;

    node->widget_type = CA_WIDGET_IMAGE;
    node->widget      = (void *)img;

    if (desc->id)    snprintf(node->id, CA_NODE_ID_MAX, "%s", desc->id);
    if (desc->style) snprintf(node->classes, CA_NODE_CLASS_MAX, "%s", desc->style);
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

    VkFormat fmt = desc->format ? desc->format : VK_FORMAT_R8G8B8A8_UNORM;

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
    vp->clear_color = desc->clear_color;
    vp->in_use      = true;

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
    return viewport ? viewport->cmd : VK_NULL_HANDLE;
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
    return viewport ? viewport->color_image : VK_NULL_HANDLE;
}

VkImageView ca_viewport_image_view(const Ca_Viewport *viewport)
{
    return viewport ? viewport->color_view : VK_NULL_HANDLE;
}

VkFormat ca_viewport_format(const Ca_Viewport *viewport)
{
    return viewport ? viewport->format : VK_FORMAT_UNDEFINED;
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

/* ============================================================
   INPUT PASS — hit-test, focus, keyboard
   ============================================================ */

static bool point_in_node(Ca_Node *n, float px, float py)
{
    return px >= n->x && px <= n->x + n->w &&
           py >= n->y && py <= n->y + n->h;
}

/* Check if a node is focusable (button or text input) */
static bool is_focusable_node(Ca_Window *win, Ca_Node *n)
{
    for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i)
        if (win->button_pool[i].in_use && win->button_pool[i].node == n)
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
    if (!node || !node->in_use || node->desc.hidden) return;
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

/* Handle keyboard input for a focused text input */
static void input_handle_keys(Ca_Window *win, Ca_TextInput *inp)
{
    int len = (int)strlen(inp->text);
    bool changed = false;

    /* Process typed characters */
    for (uint32_t i = 0; i < win->char_count; ++i) {
        uint32_t cp = win->char_buf[i];
        if (cp < 32) continue; /* skip control chars */

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

    /* --- Scroll handling --- */
    if (win->scroll_this_frame && win->node_pool) {
        static const float SCROLL_SPEED = 30.0f;
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
            float max_scroll = scroll_target->content_h - scroll_target->h;
            if (max_scroll < 0) max_scroll = 0;
            if (scroll_target->scroll_y < 0) scroll_target->scroll_y = 0;
            if (scroll_target->scroll_y > max_scroll) scroll_target->scroll_y = max_scroll;
            scroll_target->dirty |= CA_DIRTY_LAYOUT;
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
    if (win->focused_node) {
        Ca_Button *fbtn = button_for_node(win, win->focused_node);
        if (fbtn && fbtn->on_click) {
            for (uint32_t ki = 0; ki < win->key_count; ++ki) {
                int key = win->key_buf[ki];
                if (key == 257 /* ENTER */ || key == 32 /* SPACE */) {
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

    /* --- Click handling: focus + button activation --- */
    if (win->mouse_click_this_frame) {
        /* Click on an input or button focuses it */
        Ca_Node *clicked_focus = NULL;

        if (win->input_pool) {
            for (uint32_t i = 0; i < CA_MAX_INPUTS_PER_WINDOW; ++i) {
                Ca_TextInput *inp = &win->input_pool[i];
                if (!inp->in_use || !inp->node) continue;
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

        /* Fire button callbacks */
        if (win->button_pool) {
            for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i) {
                Ca_Button *btn = &win->button_pool[i];
                if (!btn->in_use || !btn->on_click || !btn->node) continue;
                if (point_in_node(btn->node, mx, my))
                    btn->on_click(btn, btn->click_data);
            }
        }

        /* Checkbox toggle */
        if (win->checkbox_pool) {
            for (uint32_t i = 0; i < CA_MAX_CHECKBOXES_PER_WINDOW; ++i) {
                Ca_Checkbox *cb = &win->checkbox_pool[i];
                if (!cb->in_use || !cb->node) continue;
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
                if (sel->open) {
                    /* Check if clicked on a dropdown option */
                    float opt_y = sel->node->y + sel->node->h;
                    float opt_h = sel->node->h;
                    for (int oi = 0; oi < sel->option_count; ++oi) {
                        float oy = opt_y + opt_h * (float)oi;
                        if (mx >= sel->node->x && mx <= sel->node->x + sel->node->w &&
                            my >= oy && my <= oy + opt_h) {
                            sel->selected = oi;
                            sel->open = false;
                            sel->node->dirty |= CA_DIRTY_CONTENT;
                            if (sel->on_change) sel->on_change(sel, sel->change_data);
                            select_handled = true;
                            break;
                        }
                    }
                    if (!select_handled) {
                        /* Click anywhere else closes the dropdown */
                        sel->open = false;
                        sel->node->dirty |= CA_DIRTY_CONTENT;
                        select_handled = true;
                    }
                } else if (point_in_node(sel->node, mx, my)) {
                    sel->open = true;
                    sel->node->dirty |= CA_DIRTY_CONTENT;
                    select_handled = true;
                }
            }
        }

        /* Menu bar — toggle dropdown or pick item */
        if (win->menubar_pool) {
            for (uint32_t i = 0; i < CA_MAX_MENUBARS_PER_WINDOW; ++i) {
                Ca_MenuBar *mb = &win->menubar_pool[i];
                if (!mb->in_use || !mb->node) continue;

                if (mb->active_menu >= 0) {
                    /* A dropdown is open — check if click hit a dropdown item */
                    Ca_MenuBarMenu *am = &mb->menus[mb->active_menu];
                    Ca_Node *hdr = am->header_node;
                    if (hdr) {
                        float item_h = 24.0f;
                        float menu_w = 140.0f;
                        float drop_x = hdr->x;
                        float drop_y = hdr->y + hdr->h;
                        bool item_hit = false;

                        for (int ii = 0; ii < am->item_count; ++ii) {
                            float iy = drop_y + item_h * (float)ii;
                            if (mx >= drop_x && mx <= drop_x + menu_w &&
                                my >= iy && my <= iy + item_h) {
                                mb->active_menu = -1;
                                mb->node->dirty |= CA_DIRTY_CONTENT;
                                if (am->items[ii].action)
                                    am->items[ii].action(am->items[ii].action_data);
                                item_hit = true;
                                break;
                            }
                        }
                        if (!item_hit) {
                            /* Check if clicked another menu header */
                            bool switched = false;
                            for (int mi = 0; mi < mb->menu_count; ++mi) {
                                if (mi == mb->active_menu) continue;
                                if (mb->menus[mi].header_node &&
                                    point_in_node(mb->menus[mi].header_node, mx, my)) {
                                    mb->active_menu = mi;
                                    mb->node->dirty |= CA_DIRTY_CONTENT;
                                    switched = true;
                                    break;
                                }
                            }
                            if (!switched) {
                                /* Click anywhere else closes dropdown */
                                mb->active_menu = -1;
                                mb->node->dirty |= CA_DIRTY_CONTENT;
                            }
                        }
                    }
                } else {
                    /* No dropdown open — check if a menu header was clicked */
                    for (int mi = 0; mi < mb->menu_count; ++mi) {
                        if (mb->menus[mi].header_node &&
                            point_in_node(mb->menus[mi].header_node, mx, my)) {
                            mb->active_menu = mi;
                            mb->node->dirty |= CA_DIRTY_CONTENT;
                            break;
                        }
                    }
                }
            }
        }

        /* Tab bar clicks */
        if (win->tabbar_pool) {
            for (uint32_t i = 0; i < CA_MAX_TABBARS_PER_WINDOW; ++i) {
                Ca_TabBar *tb = &win->tabbar_pool[i];
                if (!tb->in_use || !tb->node) continue;
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
                /* Click on the first child (header row) toggles */
                if (tn->node->child_count > 0) {
                    Ca_Node *hdr = tn->node->children[0];
                    if (point_in_node(hdr, mx, my)) {
                        tn->expanded = !tn->expanded;
                        /* Hide/show children after the header */
                        for (uint32_t ci = 1; ci < tn->node->child_count; ++ci) {
                            tn->node->children[ci]->desc.hidden = !tn->expanded;
                            tn->node->children[ci]->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
                        }
                        tn->node->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
                        if (tn->on_toggle) tn->on_toggle(tn, tn->toggle_data);
                    }
                }
            }
        }

        /* Context menu — right-click detection is below */
    }

    /* --- Right-click for context menus --- */
    if (win->mouse_buttons[1] && win->ctxmenu_pool) {
        /* Check mouse_buttons[1] (right button) just became pressed */
        static bool prev_right = false;
        bool right_now = win->mouse_buttons[1];
        if (right_now && !prev_right) {
            for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW; ++i) {
                Ca_CtxMenu *cm = &win->ctxmenu_pool[i];
                if (!cm->in_use || !cm->node) continue;
                if (point_in_node(cm->node, mx, my)) {
                    cm->open = true;
                    cm->open_x = mx;
                    cm->open_y = my;
                    cm->node->dirty |= CA_DIRTY_CONTENT;
                    break;
                }
            }
        }
        prev_right = right_now;
    }

    /* --- Slider drag handling --- */
    if (win->slider_pool) {
        bool left_down = win->mouse_buttons[0];
        if (left_down && !win->drag_node) {
            /* Check if we're starting a drag on a slider */
            for (uint32_t i = 0; i < CA_MAX_SLIDERS_PER_WINDOW; ++i) {
                Ca_Slider *sl = &win->slider_pool[i];
                if (!sl->in_use || !sl->node) continue;
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
            }
        }
    }

    /* --- Generic drag interaction (user-defined drag callbacks on divs) --- */
    {
        bool left_down = win->mouse_buttons[0];

        /* Start a new user drag */
        if (left_down && win->mouse_click_this_frame && !win->user_drag_node) {
            /* Find the smallest draggable node under cursor */
            float best_area = 1e18f;
            Ca_Node *best = NULL;
            if (win->node_pool) {
                for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
                    Ca_Node *n = &win->node_pool[i];
                    if (!n->in_use || n->desc.hidden) continue;
                    if (!n->drag_fn_start && !n->drag_fn_move && !n->drag_fn_end) continue;
                    if (point_in_node(n, mx, my)) {
                        float area = n->w * n->h;
                        if (area < best_area) {
                            best_area = area;
                            best = n;
                        }
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
                };
                ((Ca_DragFn)dn->drag_fn_end)(&ev, dn->drag_data);
            }
            win->user_drag_node   = NULL;
            win->user_drag_active = false;
        }
    }

    /* --- Hover tracking --- */
    win->hovered_node = NULL;
    if (win->node_pool) {
        /* Find the smallest node under the cursor (most specific hit) */
        float best_area = 1e18f;
        for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
            Ca_Node *n = &win->node_pool[i];
            if (!n->in_use || n->desc.hidden) continue;
            if (point_in_node(n, mx, my)) {
                float area = n->w * n->h;
                if (area < best_area) {
                    best_area = area;
                    win->hovered_node = n;
                }
            }
        }
    }
}
