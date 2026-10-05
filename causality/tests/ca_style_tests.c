// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "causality.h"
#include "css.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "style.h"
#include "ca_internal.h"
#include "ca_node_graph.h"
#include "node.h"
#include "widget.h"
#include "../src/reactive/reactive.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static const Ca_CssVar *find_var(const Ca_Stylesheet *ss, const char *name)
{
    for (int i = 0; i < ss->var_count; ++i)
        if (strcmp(ss->vars[i].name, name) == 0) return &ss->vars[i];
    return NULL;
}

/** An existing :root variable is overwritten in place; a new one is appended. */
static bool test_set_color_var(void)
{
    Ca_Stylesheet *ss = ca_css_parse(":root { --a: #112233; } .x { background: var(--a); }");
    CHECK(ss);
    CHECK(ss->var_count == 1);

    CHECK(ca_css_set_color_var(ss, "--a", 0x445566ffu));
    CHECK(ss->var_count == 1);
    CHECK(find_var(ss, "--a")->value.type == CA_CSS_VAL_COLOR);
    CHECK(find_var(ss, "--a")->value.color == 0x445566ffu);

    CHECK(ca_css_set_color_var(ss, "--b", 0x778899ffu));
    CHECK(ss->var_count == 2);
    CHECK(find_var(ss, "--b")->value.color == 0x778899ffu);
    CHECK(find_var(ss, "--a")->value.color == 0x445566ffu);

    char too_long[CA_CSS_VAR_NAME_MAX + 8];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[0] = too_long[1] = '-';
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(!ca_css_set_color_var(ss, too_long, 0u));
    CHECK(!ca_css_set_color_var(NULL, "--a", 0u));
    CHECK(ss->var_count == 2);

    ca_css_destroy(ss);
    return true;
}

/** The built-in `--ca-*` palette is fully populated with distinct depth levels. */
static bool test_default_palette(void)
{
    Ca_Instance instance = {0};
    instance.system_stylesheet = ca_style_create_system_stylesheet();
    CHECK(instance.system_stylesheet);
    ca_instance_resolve_palette(&instance);
    const Ca_Palette *p = &instance.palette;
    CHECK(p->bg_void && p->bg_base && p->bg_elevated && p->bg_surface && p->bg_overlay && p->separator);
    CHECK(p->text_bright && p->text_medium && p->text_muted && p->text_dim);
    CHECK(p->accent && p->on_accent && p->success && p->warning && p->danger && p->on_danger);
    CHECK(p->bg_base != p->bg_surface && p->bg_surface != p->bg_overlay);
    CHECK(p->text_bright != p->text_medium && p->text_medium != p->text_muted && p->text_muted != p->text_dim);
    ca_css_destroy(instance.system_stylesheet);
    return true;
}

/** App `:root` variables override library defaults everywhere, including through var() chains. */
static bool test_author_vars_override_defaults(void)
{
    Ca_Instance instance = {0};
    instance.system_stylesheet = ca_style_create_system_stylesheet();
    instance.stylesheet = ca_css_parse(":root { --brand: #123456; --ca-accent: var(--brand); }");
    CHECK(instance.system_stylesheet && instance.stylesheet);
    ca_instance_resolve_palette(&instance);
    CHECK(instance.palette.accent == 0x123456ffu);
    CHECK(instance.palette.bg_base == 0x0d0d0dffu);

    const Ca_VarScope scope = { { instance.stylesheet, instance.system_stylesheet }, 2 };
    CHECK(ca_style_lookup_var(&scope, "--ca-accent").color == 0x123456ffu);
    CHECK(ca_style_lookup_var(&scope, "--missing").type == CA_CSS_VAL_NONE);

    Ca_Stylesheet *loop = ca_css_parse(":root { --a: var(--b); --b: var(--a); }");
    const Ca_VarScope loop_scope = { { loop }, 1 };
    CHECK(ca_style_lookup_var(&loop_scope, "--a").type == CA_CSS_VAL_NONE);

    ca_css_destroy(loop);
    ca_css_destroy(instance.stylesheet);
    ca_css_destroy(instance.system_stylesheet);
    return true;
}

/** Uniform border shorthands override earlier per-side borders, as in CSS. */
static bool test_border_shorthand_resets_sides(void)
{
    Ca_Stylesheet *ss = ca_css_parse(
        ".x { border-bottom-width: 1px; border-bottom-color: #ff0000; border-width: 0px; }"
        ".y { border-left-width: 2px; border-width: 3px; border-color: #00ff00; }");
    CHECK(ss && ss->rule_count == 2);

    Ca_ResolvedStyle x = {0};
    for (int i = 0; i < ss->rules[0].decl_count; ++i)
        ca_style_apply_one_declaration(&x, ss->rules[0].decls[i].prop, &ss->rules[0].decls[i].value);
    CHECK(x.border_width == 0.0f && x.border_bottom_w == 0.0f);
    CHECK(x.border_top_w == 0.0f && x.border_left_w == 0.0f && x.border_right_w == 0.0f);

    Ca_ResolvedStyle y = {0};
    for (int i = 0; i < ss->rules[1].decl_count; ++i)
        ca_style_apply_one_declaration(&y, ss->rules[1].decls[i].prop, &ss->rules[1].decls[i].value);
    CHECK(y.border_left_w == 3.0f && y.border_top_w == 3.0f && y.border_bottom_w == 3.0f);
    CHECK(y.border_left_c == y.border_color && y.border_top_c == y.border_color && y.border_color != 0u);

    ca_css_destroy(ss);
    return true;
}

/** Headless window with node and label pools, styled by `css`. */
static bool widget_env_init(Ca_Instance *instance, Ca_Window *window, Ca_Node *root,
                            const char *css)
{
    window->instance = instance;
    window->ui_scale = 1.0f;
    window->root = root;
    root->window = window;
    root->in_use = true;
    instance->stylesheet = ca_css_parse(css);
    CHECK(instance->stylesheet);
    CHECK(ca_pool_init(&window->node_pool, sizeof(Ca_Node), 16));
    CHECK(ca_pool_init(&window->label_pool, sizeof(Ca_Label), 16));
    return true;
}

static void widget_env_destroy(Ca_Instance *instance, Ca_Window *window, Ca_Node *root)
{
    ca_node_clear(root);
    ca_pool_destroy(&window->label_pool, NULL, NULL);
    ca_pool_destroy(&window->node_pool, NULL, NULL);
    ca_widget_ctx_release_instance(instance);
    ca_reactive_shutdown(instance);
    ca_css_destroy(instance->stylesheet);
}

/** A label's descriptor colour beats CSS colour on build, pseudo-state reapply and refresh. */
static bool test_label_inline_color_survives_restyle(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    Ca_Node root = {0};
    CHECK(widget_env_init(&instance, &window, &root, ".t { color: #ff0000; }"));

    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)&root);
    Ca_Label *inline_lbl = ca_text(&(Ca_TextDesc){ .text = "a", .style = "t", .color = 0x00FF00FFu });
    Ca_Label *css_lbl = ca_text(&(Ca_TextDesc){ .text = "b", .style = "t" });
    ca_div_end();
    ca_widget_ctx_leave();
    CHECK(inline_lbl && css_lbl);
    CHECK(inline_lbl->color == 0x00FF00FFu && css_lbl->color == 0xFF0000FFu);

    ca_widget_reapply_css(inline_lbl->node);
    ca_widget_reapply_css(css_lbl->node);
    CHECK(inline_lbl->color == 0x00FF00FFu && css_lbl->color == 0xFF0000FFu);
    ca_widget_refresh_css(inline_lbl->node);
    ca_widget_refresh_css(css_lbl->node);
    CHECK(inline_lbl->color == 0x00FF00FFu && css_lbl->color == 0xFF0000FFu);

    widget_env_destroy(&instance, &window, &root);
    return true;
}

/** Counts nodes under `node` whose class list starts with `cls` and labels in the subtree. */
static void count_graph_parts(const Ca_Node *node, const char *cls, int *matches, int *labels)
{
    if (strncmp(node->classes, cls, strlen(cls)) == 0) ++*matches;
    if (node->widget_type == CA_WIDGET_LABEL) ++*labels;
    for (uint32_t i = 0; i < node->child_count; ++i)
        count_graph_parts(node->children[i], cls, matches, labels);
}

/** Counts ca-ng-* divs left visible with an empty authored size (layout fills the parent). */
static int count_visible_empty_parts(const Ca_Node *node)
{
    int n = 0;
    if (strncmp(node->classes, "ca-ng-", 6) == 0 && !node->desc.hidden &&
        (node->desc.width <= 0.0f || node->desc.height <= 0.0f) &&
        strncmp(node->classes, "ca-ng-canvas", 12) != 0 &&
        strcmp(node->classes, "ca-ng-node") != 0 &&
        strcmp(node->classes, "ca-ng-body") != 0)
        ++n;
    for (uint32_t i = 0; i < node->child_count; ++i)
        n += count_visible_empty_parts(node->children[i]);
    return n;
}

/** Without measurable text, every title and pin label is drawn as a themed stub line. */
static bool test_node_graph_stubs_unfit_text(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    Ca_Node root = {0};
    CHECK(widget_env_init(&instance, &window, &root, ".ca-ng-pin-label { font-size: 13px; }"));
    Ca_Stylesheet *system = ca_style_create_system_stylesheet();
    CHECK(system);
    instance.system_stylesheet = system;

    Ca_NodeGraph graph;
    CHECK(ca_node_graph_init(&graph));
    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)&root);
    ca_node_graph_begin(&graph, NULL, &(Ca_NodeGraphDesc){0});
    ca_ng_node_begin(&graph, &(Ca_NgNodeDesc){ .key = "n", .title = "Node" });
    ca_ng_input_pin(&graph, &(Ca_NgPinDesc){ .label = "in" });
    ca_ng_output_pin(&graph, &(Ca_NgPinDesc){ .label = "out", .color = 0x336699FFu });
    ca_ng_node_end(&graph);
    ca_ng_node_begin(&graph, &(Ca_NgNodeDesc){ .key = "m", .title = "", .x = 400.0f });
    ca_ng_input_pin(&graph, &(Ca_NgPinDesc){ .label = "" });
    ca_ng_node_end(&graph);
    ca_ng_wire(&graph, &(Ca_NgWireDesc){ .src_node = "n", .dst_node = "m" });
    ca_node_graph_end(&graph);
    ca_div_end();
    ca_widget_ctx_leave();

    int stubs = 0, labels = 0;
    count_graph_parts(&root, "ca-ng-pin-stub", &stubs, &labels);
    CHECK(stubs == 3 && labels == 0);
    CHECK(count_visible_empty_parts(&root) == 0);
    int title_stubs = 0;
    labels = 0;
    count_graph_parts(&root, "ca-ng-title-stub", &title_stubs, &labels);
    CHECK(title_stubs == 2);

    const Ca_Node *canvas = root.children[0];
    CHECK(strcmp(canvas->classes, "ca-ng-canvas") == 0 && canvas->desc.background != 0u);
    CHECK(canvas->desc.overflow_x == 1 && canvas->desc.overflow_y == 1);

    ca_node_graph_destroy(&graph);
    instance.system_stylesheet = NULL;
    widget_env_destroy(&instance, &window, &root);
    ca_css_destroy(system);
    return true;
}

/** Records the latest selection callback index. */
static void record_selection(Ca_NodeGraph *ng, int node_idx, void *user_data)
{
    (void)ng;
    *(int *)user_data = node_idx;
}

/** Builds two connected nodes into root. */
static void build_two_node_graph(Ca_NodeGraph *graph, Ca_Window *window, Ca_Node *root,
                                 int *selection)
{
    ca_widget_ctx_enter(window);
    ca_reconcile_begin((Ca_Div *)root);
    ca_node_graph_begin(graph, NULL, &(Ca_NodeGraphDesc){
        .on_node_select = record_selection, .select_data = selection });
    ca_ng_wire(graph, &(Ca_NgWireDesc){ .src_node = "a", .dst_node = "b" });
    ca_ng_node_begin(graph, &(Ca_NgNodeDesc){ .key = "a", .title = "A", .x = 0.0f, .y = 0.0f });
    ca_ng_output_pin(graph, &(Ca_NgPinDesc){ .label = "out" });
    ca_ng_node_end(graph);
    ca_ng_node_begin(graph, &(Ca_NgNodeDesc){ .key = "b", .title = "B", .x = 900.0f, .y = 500.0f });
    ca_ng_input_pin(graph, &(Ca_NgPinDesc){ .label = "in" });
    ca_ng_node_end(graph);
    ca_node_graph_end(graph);
    ca_div_end();
    ca_widget_ctx_leave();
}

/** Counts ca-ng-* nodes whose class list equals `cls`. */
static int count_class(const Ca_Node *node, const char *cls)
{
    int n = strcmp(node->classes, cls) == 0;
    for (uint32_t i = 0; i < node->child_count; ++i)
        n += count_class(node->children[i], cls);
    return n;
}

/** Fit frames all nodes, scroll zooms about the cursor, selection drives wire emphasis. */
static bool test_node_graph_view(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    Ca_Node root = {0};
    CHECK(widget_env_init(&instance, &window, &root, ""));
    Ca_NodeGraph graph;
    CHECK(ca_node_graph_init(&graph));
    int selection = 99;

    build_two_node_graph(&graph, &window, &root, &selection);
    Ca_Node *canvas = root.children[0];
    canvas->x = 10.0f; canvas->y = 20.0f; canvas->w = 800.0f; canvas->h = 600.0f;
    ca_window_sync_size_signals(&window);
    ca_node_graph_request_fit(&graph);
    build_two_node_graph(&graph, &window, &root, &selection);
    CHECK(graph.zoom > 0.15f && graph.zoom <= 1.0f);
    for (int i = 0; i < 2; ++i) {
        const Ca_NgNodeState *st = ca_node_graph_state(&graph, i);
        float x0 = st->canvas_x * graph.zoom + graph.pan_x;
        float y0 = st->canvas_y * graph.zoom + graph.pan_y;
        CHECK(x0 >= 0.0f && y0 >= 0.0f);
        CHECK(x0 + 180.0f * graph.zoom <= 800.0f && y0 + 60.0f * graph.zoom <= 600.0f);
    }

    window.mouse_x = canvas->x + 300.0f;
    window.mouse_y = canvas->y + 200.0f;
    float before_x = (300.0f - graph.pan_x) / graph.zoom;
    float before_y = (200.0f - graph.pan_y) / graph.zoom;
    float zoom_before = graph.zoom;
    ((Ca_ScrollFn)canvas->scroll_fn)(0.0, 3.0, canvas->scroll_data);
    CHECK(graph.zoom > zoom_before);
    CHECK(fabsf((300.0f - graph.pan_x) / graph.zoom - before_x) < 1e-3f);
    CHECK(fabsf((200.0f - graph.pan_y) / graph.zoom - before_y) < 1e-3f);

    CHECK(count_class(&root, "ca-ng-wire") == 21);
    graph.selected_node = 0;
    build_two_node_graph(&graph, &window, &root, &selection);
    CHECK(count_class(&root, "ca-ng-wire active") == 21);

    ((Ca_DragFn)canvas->drag_fn_end)(&(Ca_DragEvent){ .window = &window, .dx = 1.0f }, canvas->drag_data);
    CHECK(graph.selected_node == -1 && selection == -1);
    graph.selected_node = 1;
    selection = 99;
    ((Ca_DragFn)canvas->drag_fn_end)(&(Ca_DragEvent){ .window = &window, .dx = 40.0f }, canvas->drag_data);
    CHECK(graph.selected_node == 1 && selection == 99);

    ca_node_graph_destroy(&graph);
    widget_env_destroy(&instance, &window, &root);
    return true;
}

/** Collects visible wire pieces under node. */
static int collect_wire_pieces(const Ca_Node *node, const Ca_Node **out, int max)
{
    int n = 0;
    if (strncmp(node->classes, "ca-ng-wire", 10) == 0 && !node->desc.hidden && n < max)
        out[n++] = node;
    for (uint32_t i = 0; i < node->child_count; ++i)
        n += collect_wire_pieces(node->children[i], out + n, max - n);
    return n;
}

/** Axis-aligned bounds of a straight (0/90/180 degree) wire piece. */
static bool straight_piece_rect(const Ca_Node *p, float r[4])
{
    float rot = fmodf(fabsf(p->desc.rotation), 180.0f);
    bool vertical = fabsf(rot - 90.0f) < 0.01f;
    if (!vertical && rot > 0.01f && rot < 179.99f) return false;
    float cx = p->desc.pos_x + p->desc.width * 0.5f;
    float cy = p->desc.pos_y + p->desc.height * 0.5f;
    float hw = (vertical ? p->desc.height : p->desc.width) * 0.5f;
    float hh = (vertical ? p->desc.width : p->desc.height) * 0.5f;
    r[0] = cx - hw; r[1] = cy - hh; r[2] = cx + hw; r[3] = cy + hh;
    return true;
}

/** Builds nodes a at (0,0) and b at (bx,by) joined by one wire, at 100% zoom. */
static void build_route(Ca_NodeGraph *graph, Ca_Window *window, Ca_Node *root, float bx, float by)
{
    ca_widget_ctx_enter(window);
    ca_reconcile_begin((Ca_Div *)root);
    ca_node_graph_begin(graph, NULL, &(Ca_NodeGraphDesc){0});
    ca_ng_wire(graph, &(Ca_NgWireDesc){ .src_node = "a", .dst_node = "b" });
    ca_ng_node_begin(graph, &(Ca_NgNodeDesc){ .key = "a", .title = "A" });
    ca_ng_output_pin(graph, &(Ca_NgPinDesc){ .label = "out" });
    ca_ng_node_end(graph);
    ca_ng_node_begin(graph, &(Ca_NgNodeDesc){ .key = "b", .title = "B", .x = bx, .y = by });
    ca_ng_input_pin(graph, &(Ca_NgPinDesc){ .label = "in" });
    ca_ng_node_end(graph);
    ca_node_graph_end(graph);
    ca_div_end();
    ca_widget_ctx_leave();
}

/** Orthogonal routes: straight when aligned, clear of both nodes when backward, never overlapping. */
static bool test_node_graph_orthogonal_routes(void)
{
    const struct { float bx, by; int min_pieces, max_pieces; } cases[] = {
        { 400.0f,    0.0f,  1,  1 },
        { 400.0f,  200.0f,  3, 11 },
        { 400.0f,    0.5f,  3,  3 },
        { -300.0f, 150.0f,  5, 21 },
        {  60.0f,  -40.0f,  5, 21 },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        Ca_Instance instance = {0};
        Ca_Window window = {0};
        Ca_Node root = {0};
        CHECK(widget_env_init(&instance, &window, &root, ""));
        Ca_NodeGraph graph;
        CHECK(ca_node_graph_init(&graph));
        build_route(&graph, &window, &root, cases[c].bx, cases[c].by);
        build_route(&graph, &window, &root, cases[c].bx, cases[c].by);

        CHECK(count_class(&root, "ca-ng-wire") == 21);
        const Ca_Node *pieces[32];
        int n = collect_wire_pieces(&root, pieces, 32);
        CHECK(n >= cases[c].min_pieces && n <= cases[c].max_pieces);

        float rects[32][4];
        int straight = 0;
        for (int i = 0; i < n; ++i)
            if (straight_piece_rect(pieces[i], rects[straight])) ++straight;
        for (int i = 0; i < straight; ++i)
            for (int j = i + 1; j < straight; ++j) {
                float ox = fminf(rects[i][2], rects[j][2]) - fmaxf(rects[i][0], rects[j][0]);
                float oy = fminf(rects[i][3], rects[j][3]) - fmaxf(rects[i][1], rects[j][1]);
                CHECK(ox <= 0.01f || oy <= 0.01f);
            }

        if (cases[c].bx < 0.0f || cases[c].bx < 100.0f) {
            const Ca_NgNodeState *a = ca_node_graph_state(&graph, 0);
            const Ca_NgNodeState *b = ca_node_graph_state(&graph, 1);
            for (int i = 0; i < straight; ++i) {
                bool horizontal = rects[i][2] - rects[i][0] > rects[i][3] - rects[i][1];
                if (!horizontal || rects[i][2] - rects[i][0] < 30.0f) continue;
                float y = 0.5f * (rects[i][1] + rects[i][3]);
                bool in_a = y > a->canvas_y && y < a->canvas_y + 58.0f;
                bool in_b = y > b->canvas_y && y < b->canvas_y + 58.0f;
                CHECK(!in_a && !in_b);
            }
        }

        ca_node_graph_destroy(&graph);
        widget_env_destroy(&instance, &window, &root);
    }
    return true;
}

int main(void)
{
    if (!test_set_color_var()) return 1;
    if (!test_default_palette()) return 1;
    if (!test_author_vars_override_defaults()) return 1;
    if (!test_border_shorthand_resets_sides()) return 1;
    if (!test_label_inline_color_survives_restyle()) return 1;
    if (!test_node_graph_stubs_unfit_text()) return 1;
    if (!test_node_graph_view()) return 1;
    if (!test_node_graph_orthogonal_routes()) return 1;
    printf("causality_style_tests passed\n");
    return 0;
}
