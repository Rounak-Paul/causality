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

static int s_changes;
static int s_commits;

static void on_change(Ca_ColorPicker *p, void *u) { (void)p; (void)u; s_changes++; }
static void on_commit(Ca_ColorPicker *p, void *u) { (void)p; (void)u; s_commits++; }

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static float s_scale;

static void press(Ca_Window *w, float x, float y, bool click)
{
    ca_color_picker_input(w, x, y, true, click, 0, s_scale);
}

static void release(Ca_Window *w)
{
    ca_color_picker_input(w, 0, 0, false, false, 0, s_scale);
}

/** Expand, SV/hue/alpha drags, gesture callbacks, hue retention and clipping. */
static bool test_color_picker(float scale)
{
    s_scale = scale;
    Ca_Font font = {0};
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    instance.font = &font;
    window.instance = &instance;
    window.ui_scale = scale;
    CHECK(ca_dyn_array_init(&window.draw_cmd_storage, sizeof(Ca_DrawCmd)));
    CHECK(ca_dyn_array_init(&window.paint_cache_storage, sizeof(Ca_DrawCmd)));
    CHECK(ca_pool_init(&window.node_pool, sizeof(Ca_Node), 1));
    CHECK(ca_pool_init(&window.color_picker_pool, sizeof(Ca_ColorPicker), 1));
    Ca_Node *node = ca_pool_acquire(&window.node_pool);
    Ca_ColorPicker *p = ca_pool_acquire(&window.color_picker_pool);
    CHECK(node && p);
    node->in_use = true;
    node->window = &window;
    node->widget_type = CA_WIDGET_COLOR_PICKER;
    node->widget = p;
    node->w = 200 * scale;
    p->in_use = true;
    p->node = node;
    p->alpha = true;
    p->on_change = on_change;
    p->on_commit = on_commit;
    window.root = node;

    const float red[4] = { 1, 0, 0, 1 };
    ca_color_picker_set(p, red);
    CHECK(near(p->h, 0) && near(p->s, 1) && near(p->v, 1));

    Ca_ColorPickerRects r;
    ca_color_picker_rects(p, scale, &r);
    node->h = r.total_h;
    node->dirty = CA_DIRTY_CONTENT;
    window.draw_cmd_count = 0;
    paint_tree_cached(&instance, &window, node, (ClipRect){0}, 0, ca_transform_identity());
    CHECK(window.draw_cmd_count == 3);

    press(&window, 10 * scale, 5 * scale, true);
    release(&window);
    CHECK(p->expanded && node->desc.height > r.total_h);
    CHECK(s_changes == 0 && s_commits == 0);
    ca_color_picker_rects(p, scale, &r);
    node->h = r.total_h;

    press(&window, node->w * 0.5f, r.sv_y + r.sv_h * 0.5f, true);
    CHECK(s_changes == 1 && p->drag_part == CA_COLOR_PICKER_PART_SV);
    CHECK(near(p->rgba[0], 0.5f) && near(p->rgba[1], 0.25f) && near(p->rgba[2], 0.25f));
    press(&window, node->w * 0.5f, r.sv_y + r.sv_h * 0.5f, false);
    CHECK(s_changes == 1);
    press(&window, node->w * 2.0f, r.sv_y - 50 * scale, false);
    CHECK(s_changes == 2 && near(p->s, 1) && near(p->v, 1));
    release(&window);
    CHECK(s_commits == 1 && p->drag_part == CA_COLOR_PICKER_PART_NONE && !window.drag_node);

    press(&window, node->w / 3.0f, r.hue_y + r.hue_h * 0.5f, true);
    release(&window);
    CHECK(near(p->h, 1.0f / 3.0f) && near(p->rgba[0], 0) && near(p->rgba[1], 1));
    press(&window, node->w * 0.25f, r.alpha_y + r.alpha_h * 0.5f, true);
    release(&window);
    CHECK(near(p->rgba[3], 0.25f) && s_commits == 3);

    const float gray[4] = { 0.5f, 0.5f, 0.5f, 1 };
    ca_color_picker_set(p, gray);
    CHECK(near(p->s, 0) && near(p->h, 1.0f / 3.0f));

    node->dirty = CA_DIRTY_CONTENT;
    window.draw_cmd_count = 0;
    ClipRect clip = { .active = true, .x = 0, .y = 0, .w = node->w, .h = 20 * scale };
    paint_tree_cached(&instance, &window, node, clip, 0, ca_transform_identity());
    CHECK(window.draw_cmd_count > 10);
    for (unsigned i = 0; i < window.draw_cmd_count; ++i) {
        const Ca_DrawCmd *c = &window.draw_cmds[i];
        CHECK(c->has_clip && c->clip_h == clip.h);
        CHECK(isfinite(c->x) && isfinite(c->y) && c->w > 0 && c->h > 0);
    }

    ca_dyn_array_destroy(&window.draw_cmd_storage);
    ca_dyn_array_destroy(&window.paint_cache_storage);
    ca_pool_destroy(&window.color_picker_pool, NULL, NULL);
    ca_pool_destroy(&window.node_pool, NULL, NULL);
    s_changes = s_commits = 0;
    return true;
}

int main(void)
{
    if (!test_color_picker(1.0f) || !test_color_picker(2.0f)) return 1;
    puts("causality_color_picker_tests: all tests passed");
    return 0;
}
