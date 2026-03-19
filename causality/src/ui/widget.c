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
    nd.width  = s(desc->width);
    nd.height = s(desc->height);  /* 0 if not user-set; CSS or default fills later */

    Ca_Node *node = ca_node_add(parent, &nd);
    if (!node) return NULL;

    slot->node   = node;
    slot->in_use = true;
    slot->color  = desc->color;
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
    ca_style_resolve(ss, node, elem_type, classes, &rs);

    /* Scale CSS-resolved pixel values before applying */
    rs.width        = s(rs.width);
    rs.height       = s(rs.height);
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
        /* Default height if neither user nor CSS set it */
        if (lbl->node->desc.height <= 0.0f) {
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
                         Ca_ElementType elem_type)
{
    assert(g_ctx.active && desc);
    Ca_Label *lbl = add_label(g_ctx.window, ctx_top(), desc);
    if (lbl && lbl->node) {
        apply_css(lbl->node, &lbl->node->desc, elem_type,
                  desc->style, desc->id, &lbl->color);
        /* Default heading height if neither user nor CSS set it */
        if (lbl->node->desc.height <= 0.0f)
            lbl->node->desc.height = s(default_height);
    }
    return lbl;
}

Ca_Label *ca_h1(const Ca_TextDesc *desc) { return heading(desc, 24.0f, CA_ELEM_H1); }
Ca_Label *ca_h2(const Ca_TextDesc *desc) { return heading(desc, 20.0f, CA_ELEM_H2); }
Ca_Label *ca_h3(const Ca_TextDesc *desc) { return heading(desc, 18.0f, CA_ELEM_H3); }
Ca_Label *ca_h4(const Ca_TextDesc *desc) { return heading(desc, 16.0f, CA_ELEM_H4); }
Ca_Label *ca_h5(const Ca_TextDesc *desc) { return heading(desc, 14.0f, CA_ELEM_H5); }
Ca_Label *ca_h6(const Ca_TextDesc *desc) { return heading(desc, 12.0f, CA_ELEM_H6); }

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
    }
}
