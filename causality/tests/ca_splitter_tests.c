// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

typedef struct GLFWwindow GLFWwindow;
#include "../src/ui/paint.c"
#include "widget.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

/** Verify cached handles repaint on hover and release, for direction and UI scale. */
static bool test_splitter(int direction, float scale)
{
    Ca_Font font = {0};
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    instance.font = &font;
    window.instance = &instance;
    window.ui_scale = scale;
    CHECK(ca_dyn_array_init(&window.draw_cmd_storage, sizeof(Ca_DrawCmd)));
    CHECK(ca_dyn_array_init(&window.paint_cache_storage, sizeof(Ca_DrawCmd)));
    CHECK(ca_pool_init(&window.node_pool, sizeof(Ca_Node), 1));
    CHECK(ca_pool_init(&window.splitter_pool, sizeof(Ca_Splitter), 1));
    Ca_Node *node = ca_pool_acquire(&window.node_pool);
    Ca_Splitter *splitter = ca_pool_acquire(&window.splitter_pool);
    CHECK(node && splitter);
    node->in_use = true;
    node->window = &window;
    node->widget_type = CA_WIDGET_SPLITTER;
    node->widget = splitter;
    node->w = 200 * scale; node->h = 160 * scale;
    node->dirty = CA_DIRTY_CONTENT;
    splitter->in_use = true;
    splitter->node = node;
    splitter->direction = direction;
    splitter->ratio = 0.5f;
    splitter->min_ratio = 0.1f; splitter->max_ratio = 0.9f;
    splitter->bar_size = 4 * scale;
    window.root = node;

    window.draw_cmd_count = 0;
    paint_tree_cached(&instance, &window, node, (ClipRect){0}, 0, ca_transform_identity());
    CHECK(window.draw_cmd_count == 1 && window.draw_cmds[0].a == 0);
    window.mouse_x = node->w * 0.5f;
    window.mouse_y = node->h * 0.5f;
    ca_widget_input_pass(&window);
    CHECK(window.hovered_node == node && (node->dirty & CA_DIRTY_CONTENT));
    window.draw_cmd_count = 0;
    paint_tree_cached(&instance, &window, node, (ClipRect){0}, 0, ca_transform_identity());
    CHECK(window.draw_cmd_count == 19 && node->cache_post_count == 18);
    for (unsigned i = 10; i < 19; ++i) {
        const Ca_DrawCmd *cmd = &window.draw_cmds[i];
        CHECK(cmd->w > 0 && cmd->h > 0);
        CHECK(cmd->x >= 0 && cmd->y >= 0);
        CHECK(cmd->x + cmd->w <= node->w && cmd->y + cmd->h <= node->h);
        CHECK((cmd->corner_radius > 0) == (i >= 12));
        if (i >= 12) CHECK(cmd->w == cmd->h);
    }
    Ca_Node *pane = ca_pool_acquire(&window.node_pool);
    CHECK(pane);
    pane->in_use = true;
    pane->window = &window;
    pane->parent = node;
    pane->widget_type = CA_WIDGET_NONE;
    pane->w = node->w;
    pane->h = node->h;
    if (direction == CA_HORIZONTAL) {
        pane->w = (node->w - splitter->bar_size) * 0.5f;
        window.mouse_x = pane->w - 2.0f * scale;
    } else {
        pane->h = (node->h - splitter->bar_size) * 0.5f;
        window.mouse_y = pane->h - 2.0f * scale;
    }
    ca_widget_input_pass(&window);
    CHECK(window.hovered_node == node);
    pane->desc.z_index = 5;
    ca_widget_input_pass(&window);
    CHECK(window.hovered_node == pane);
    pane->desc.z_index = 0;
    window.mouse_x = 5.0f * scale;
    window.mouse_y = 5.0f * scale;
    ca_widget_input_pass(&window);
    CHECK(window.hovered_node == pane);
    window.mouse_x = -100;
    window.mouse_y = -100;
    ca_widget_input_pass(&window);
    CHECK(window.hovered_node == NULL && (node->dirty & CA_DIRTY_CONTENT));
    window.draw_cmd_count = 0;
    paint_tree_cached(&instance, &window, node, (ClipRect){0}, 0, ca_transform_identity());
    CHECK(window.draw_cmd_count == 1 && node->cache_count == 1 && node->cache_post_count == 0 && window.draw_cmds[0].a == 0);

    splitter->dragging = true;
    node->dirty = CA_DIRTY_CONTENT;
    window.draw_cmd_count = 0;
    paint_tree_cached(&instance, &window, node, (ClipRect){0}, 0, ca_transform_identity());
    CHECK(node->cache_post_count == 18);
    float ratio = splitter->ratio;
    ca_widget_input_pass(&window);
    CHECK(!splitter->dragging && splitter->ratio == ratio);
    CHECK(node->dirty & CA_DIRTY_CONTENT);
    window.draw_cmd_count = 0;
    paint_tree_cached(&instance, &window, node, (ClipRect){0}, 0, ca_transform_identity());
    CHECK(window.draw_cmd_count == 1 && node->cache_count == 1 && node->cache_post_count == 0 && window.draw_cmds[0].a == 0);
    ca_widget_input_pass(&window);
    CHECK(!(node->dirty & CA_DIRTY_CONTENT));

    splitter->dragging = true;
    node->w = node->h = 3 * scale;
    node->dirty = CA_DIRTY_CONTENT;
    window.draw_cmd_count = 0;
    paint_tree_cached(&instance, &window, node, (ClipRect){0}, 0, ca_transform_identity());
    for (unsigned i = 0; i < window.draw_cmd_count; ++i) {
        CHECK(isfinite(window.draw_cmds[i].x) && isfinite(window.draw_cmds[i].y));
        CHECK(window.draw_cmds[i].w > 0 && window.draw_cmds[i].h > 0);
    }
    ca_dyn_array_destroy(&window.draw_cmd_storage);
    ca_dyn_array_destroy(&window.paint_cache_storage);
    ca_pool_destroy(&window.splitter_pool, NULL, NULL);
    ca_pool_destroy(&window.node_pool, NULL, NULL);
    return true;
}

/** Verify reusable CSS glow, geometry, clipping, and disabled/invalid values. */
static bool test_glow(void)
{
    Ca_Node node = {0};
    Ca_ResolvedStyle style = {0};
    Ca_Stylesheet *css = ca_css_parse(".light { glow-radius: 9px; glow-color: #ffd34e80; }");
    CHECK(css);
    ca_style_resolve_layers(NULL, css, &node, CA_ELEM_DIV, "light", &style);
    ca_style_apply_to_node(&style, &node.desc, NULL);
    CHECK(node.desc.glow_radius == 9.0f && node.desc.glow_color == 0xFFD34E80u);
    Ca_NodeDesc off = node.desc;
    off.glow_radius = 0;
    CHECK(content_desc_changed(&node.desc, &off));
    CHECK(!layout_desc_changed(&node.desc, &off));
    ca_css_destroy(css);

    Ca_Window window = {0};
    CHECK(ca_dyn_array_init(&window.draw_cmd_storage, sizeof(Ca_DrawCmd)));
    Ca_DrawCmd source = {0};
    source.x = 20; source.y = 30; source.w = 10; source.h = 12;
    source.a = 0.5f; source.z_index = 4;
    source.corner_tl = 2; source.corner_tr = 3;
    source.has_clip = true; source.clip_x = 0; source.clip_y = 0;
    source.clip_w = 100; source.clip_h = 100; source.clip_radius = 8;
    paint_glow(&window, &source, node.desc.glow_radius, node.desc.glow_color);
    CHECK(window.draw_cmd_count == 1);
    const Ca_DrawCmd *glow = &window.draw_cmds[0];
    CHECK(glow->draw_mode == CA_DRAW_MODE_GLOW && glow->blur_radius == 9);
    CHECK(glow->x == 11 && glow->y == 21 && glow->w == 28 && glow->h == 30);
    CHECK(glow->corner_tl == 2 && glow->corner_tr == 3 && glow->z_index == 4);
    CHECK(glow->has_clip && glow->clip_radius == 8 && glow->clip_w == 100);
    CHECK(fabsf(glow->a - 0.5f * 128.0f / 255.0f) < 0.00001f);
    paint_glow(&window, &source, 0, 0xFFFFFFFFu);
    paint_glow(&window, &source, -1, 0xFFFFFFFFu);
    paint_glow(&window, &source, NAN, 0xFFFFFFFFu);
    paint_glow(&window, &source, INFINITY, 0xFFFFFFFFu);
    paint_glow(&window, &source, 9, 0xFFFFFF00u);
    CHECK(window.draw_cmd_count == 1);
    ca_dyn_array_destroy(&window.draw_cmd_storage);
    return true;
}

/** Verify app CSS can resolve and paint directional scrollbar chrome. */
static bool test_scrollbar_chrome(void)
{
    Ca_Node node = {0};
    Ca_ResolvedStyle style = {0};
    Ca_Stylesheet *css = ca_css_parse(
        ".scroll { scrollbar-track-border-width: 1px;"
        " scrollbar-track-border-top-color: #112233;"
        " scrollbar-track-border-right-color: #223344;"
        " scrollbar-track-border-bottom-color: #334455;"
        " scrollbar-track-border-left-color: #445566;"
        " scrollbar-thumb-border-width: 2px;"
        " scrollbar-thumb-border-top-color: #556677;"
        " scrollbar-thumb-border-right-color: #667788;"
        " scrollbar-thumb-border-bottom-color: #778899;"
        " scrollbar-thumb-border-left-color: #8899aa; }");
    CHECK(css);
    ca_style_resolve_layers(NULL, css, &node, CA_ELEM_DIV, "scroll", &style);
    ca_style_apply_to_node(&style, &node.desc, NULL);
    CHECK(node.desc.scrollbar_track_border_width == 1.0f);
    CHECK(node.desc.scrollbar_thumb_border_width == 2.0f);
    CHECK(node.desc.scrollbar_track_border_top_color == 0x112233FFu);
    CHECK(node.desc.scrollbar_thumb_border_bottom_color == 0x778899FFu);

    Ca_Window window = {0};
    CHECK(ca_dyn_array_init(&window.draw_cmd_storage, sizeof(Ca_DrawCmd)));
    paint_scrollbar_border(&window, 5.0f, 7.0f, 12.0f, 20.0f,
                           node.desc.scrollbar_track_border_width,
                           &node.desc, false, (ClipRect){0});
    CHECK(window.draw_cmd_count == 4);
    CHECK(window.draw_cmds[0].w == 12.0f && window.draw_cmds[0].h == 1.0f);
    CHECK(window.draw_cmds[1].x == 16.0f && window.draw_cmds[1].h == 18.0f);
    CHECK(window.draw_cmds[2].y == 26.0f && window.draw_cmds[2].w == 12.0f);
    CHECK(window.draw_cmds[3].x == 5.0f && window.draw_cmds[3].h == 18.0f);
    ca_dyn_array_destroy(&window.draw_cmd_storage);
    ca_css_destroy(css);
    return true;
}

/** Run both splitter orientations at standard and fractional UI scales. */
int main(void)
{
    if (!test_glow()) return 1;
    if (!test_scrollbar_chrome()) return 1;
    const float scales[] = {1.0f, 1.12f, 2.0f};
    for (unsigned i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        if (!test_splitter(CA_HORIZONTAL, scales[i])) return 1;
        if (!test_splitter(CA_VERTICAL, scales[i])) return 1;
    }
    puts("causality splitter tests passed");
    return 0;
}
