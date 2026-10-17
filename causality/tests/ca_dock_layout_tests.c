// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* Headless tests for the dock layout model: operations, invariants,
   failure atomicity under injected allocation failure, persistence. */
#include "ca_dock.h"
#include "ca_dock_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        g_failures++; \
    } \
} while (0)

/* ---- Allocation failure injection ---- */

static size_t g_allocation_attempts;
static size_t g_fail_at_attempt;

static bool should_fail(void)
{
    g_allocation_attempts++;
    return g_fail_at_attempt && g_fail_at_attempt == g_allocation_attempts;
}

static void *test_malloc(size_t size) { return should_fail() ? NULL : malloc(size); }
static void *test_calloc(size_t n, size_t size) { return should_fail() ? NULL : calloc(n, size); }
static void *test_realloc(void *p, size_t size) { return should_fail() ? NULL : realloc(p, size); }
static void test_free(void *p) { free(p); }

static void arm_failure(size_t attempt)
{
    g_allocation_attempts = 0;
    g_fail_at_attempt = attempt;
    ca_set_allocator(test_malloc, test_calloc, test_realloc, test_free);
}

static void disarm_failure(void)
{
    ca_set_allocator(NULL, NULL, NULL, NULL);
    g_fail_at_attempt = 0;
}

/* ---- Helpers ---- */

static char *save(const Ca_DockLayout *layout)
{
    size_t needed = ca_dock_layout_save(layout, NULL, 0);
    char *text = malloc(needed + 1);
    if (!text) return NULL;
    size_t written = ca_dock_layout_save(layout, text, needed + 1);
    if (written != needed) text[0] = '\0';
    return text;
}

static Ca_DockLayout *make_layout(int panels)
{
    Ca_DockLayout *layout = ca_dock_layout_create();
    char id[32];
    for (int i = 0; i < panels; ++i) {
        snprintf(id, sizeof(id), "p%d", i);
        ca_dock_layout_register(layout, id);
    }
    return layout;
}

/* Main window: [p0 p1] | [p2] split horizontally. */
static void seed_two_groups(Ca_DockLayout *layout)
{
    int left = ca_dock_layout_new_group(layout);
    int right = ca_dock_layout_new_group(layout);
    ca_dock_layout_add_tab(layout, left, 0, -1);
    ca_dock_layout_add_tab(layout, left, 1, -1);
    ca_dock_layout_add_tab(layout, right, 2, -1);
    int split = ca_dock_layout_new_split(layout, true, 0.3f, left, right);
    ca_dock_layout_set_root(layout, CA_DOCK_MAIN_WINDOW, split);
}

/* ---- Tests ---- */

static void test_registration(void)
{
    Ca_DockLayout *layout = ca_dock_layout_create();
    CHECK(ca_dock_layout_register(layout, "outline") == 0);
    CHECK(ca_dock_layout_register(layout, "inspector") == 1);
    CHECK(ca_dock_layout_register(layout, "outline") == 0);
    CHECK(ca_dock_layout_register(layout, "") == CA_DOCK_NONE);
    CHECK(ca_dock_layout_register(layout, NULL) == CA_DOCK_NONE);
    char long_id[CA_DOCK_ID_MAX + 4];
    memset(long_id, 'x', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';
    CHECK(ca_dock_layout_register(layout, long_id) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_find(layout, "inspector") == 1);
    CHECK(ca_dock_layout_find(layout, "missing") == CA_DOCK_NONE);
    CHECK(strcmp(ca_dock_layout_panel_id(layout, 1), "inspector") == 0);
    CHECK(ca_dock_layout_panel_id(layout, 7) == NULL);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_destroy(layout);
}

static void test_tabs_and_reorder(void)
{
    Ca_DockLayout *layout = make_layout(4);
    int group = ca_dock_layout_new_group(layout);
    CHECK(ca_dock_layout_set_root(layout, CA_DOCK_MAIN_WINDOW, group));
    for (int p = 0; p < 4; ++p) CHECK(ca_dock_layout_add_tab(layout, group, p, -1));
    CHECK(ca_dock_group_tab_count(layout, group) == 4);
    CHECK(ca_dock_layout_active_panel(layout, group) == 3);

    CHECK(ca_dock_layout_add_tab(layout, group, 3, 0));
    CHECK(ca_dock_group_tab(layout, group, 0) == 3);
    CHECK(ca_dock_group_tab(layout, group, 1) == 0);
    CHECK(ca_dock_layout_add_tab(layout, group, 3, 4));
    CHECK(ca_dock_group_tab(layout, group, 3) == 3);
    CHECK(ca_dock_layout_tab_index(layout, 0) == 0);
    CHECK(ca_dock_layout_set_active(layout, group, 1));
    CHECK(!ca_dock_layout_set_active(layout, group, 9));
    CHECK(ca_dock_layout_valid(layout));

    ca_dock_layout_remove(layout, 1);
    CHECK(ca_dock_group_tab_count(layout, group) == 3);
    CHECK(ca_dock_layout_group_of(layout, 1) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_remove(layout, 0);
    ca_dock_layout_remove(layout, 2);
    ca_dock_layout_remove(layout, 3);
    CHECK(ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_destroy(layout);
}

static void test_split_and_collapse(void)
{
    Ca_DockLayout *layout = make_layout(4);
    seed_two_groups(layout);
    CHECK(ca_dock_layout_valid(layout));
    const int right = ca_dock_layout_group_of(layout, 2);

    CHECK(ca_dock_layout_split(layout, right, 0, CA_DOCK_ZONE_BOTTOM, 0.0f));
    CHECK(ca_dock_layout_valid(layout));
    const int new_group = ca_dock_layout_group_of(layout, 0);
    CHECK(new_group != CA_DOCK_NONE && new_group != right);
    const Ca_DockNodeEntry *parent = ca_dock_node_const(layout, ca_dock_node_const(layout, new_group)->parent);
    CHECK(parent && parent->kind == CA_DOCK_NODE_SPLIT && !parent->horizontal && parent->second == new_group);

    CHECK(!ca_dock_layout_split(layout, new_group, 0, CA_DOCK_ZONE_LEFT, 0.0f));
    CHECK(ca_dock_layout_split(layout, new_group, 3, CA_DOCK_ZONE_TAB, 0.0f));
    CHECK(ca_dock_layout_group_of(layout, 3) == new_group);

    ca_dock_layout_remove(layout, 0);
    ca_dock_layout_remove(layout, 3);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_remove(layout, 2);
    CHECK(ca_dock_layout_valid(layout));
    CHECK(ca_dock_layout_node_kind(layout, ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW)) == CA_DOCK_NODE_GROUP);
    ca_dock_layout_destroy(layout);
}

static void test_dock_root(void)
{
    Ca_DockLayout *layout = make_layout(4);
    CHECK(ca_dock_layout_dock_root(layout, CA_DOCK_MAIN_WINDOW, 0, CA_DOCK_ZONE_TAB, 0.0f));
    CHECK(!ca_dock_layout_dock_root(layout, CA_DOCK_MAIN_WINDOW, 0, CA_DOCK_ZONE_LEFT, 0.0f));
    CHECK(ca_dock_layout_dock_root(layout, CA_DOCK_MAIN_WINDOW, 1, CA_DOCK_ZONE_LEFT, 0.25f));
    const int root = ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW);
    const Ca_DockNodeEntry *split = ca_dock_node_const(layout, root);
    CHECK(split->kind == CA_DOCK_NODE_SPLIT && split->horizontal);
    CHECK(split->ratio > 0.24f && split->ratio < 0.26f);
    CHECK(ca_dock_layout_dock_root(layout, CA_DOCK_MAIN_WINDOW, 3, CA_DOCK_ZONE_BOTTOM, 0.2f));
    const Ca_DockNodeEntry *bottom = ca_dock_node_const(layout, ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW));
    CHECK(!bottom->horizontal && bottom->ratio > 0.79f && bottom->ratio < 0.81f);
    ca_dock_layout_remove(layout, 3);
    CHECK(ca_dock_layout_dock_root(layout, CA_DOCK_MAIN_WINDOW, 2, CA_DOCK_ZONE_TAB, 0.0f));
    CHECK(ca_dock_layout_group_of(layout, 2) == ca_dock_layout_group_of(layout, 1));
    CHECK(!ca_dock_layout_dock_root(layout, 5, 3, CA_DOCK_ZONE_TAB, 0.0f));
    CHECK(!ca_dock_layout_dock_root(layout, CA_DOCK_MAIN_WINDOW, 3, (Ca_DockZone)99, 0.0f));
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_destroy(layout);
}

static void test_float_and_dock_window(void)
{
    Ca_DockLayout *layout = make_layout(5);
    seed_two_groups(layout);
    const int w1 = ca_dock_layout_float(layout, 1, 10, 20, 300, 200);
    CHECK(w1 == 1);
    CHECK(ca_dock_layout_valid(layout));
    CHECK(ca_dock_layout_window_of(layout, ca_dock_layout_group_of(layout, 1)) == w1);
    CHECK(ca_dock_layout_float(layout, 1, 0, 0, 100, 100) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_float(layout, 2, 0, 0, 0, 100) == CA_DOCK_NONE);

    const int floating_group = ca_dock_layout_group_of(layout, 1);
    CHECK(ca_dock_layout_add_tab(layout, floating_group, 3, -1));
    CHECK(ca_dock_layout_split(layout, floating_group, 4, CA_DOCK_ZONE_RIGHT, 0.0f));
    CHECK(ca_dock_layout_valid(layout));

    const int w2 = ca_dock_layout_float(layout, 0, 0, 0, 300, 200);
    CHECK(w2 == 2);
    const int main_left = ca_dock_layout_group_of(layout, 2);
    CHECK(ca_dock_layout_dock_window(layout, w2, CA_DOCK_MAIN_WINDOW, main_left, CA_DOCK_ZONE_TAB, 0, 0.0f));
    CHECK(ca_dock_layout_group_of(layout, 0) == main_left);
    CHECK(ca_dock_group_tab(layout, main_left, 0) == 0);
    CHECK(ca_dock_layout_window_root(layout, w2) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_valid(layout));

    const int main_root = ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW);
    CHECK(ca_dock_layout_dock_window(layout, w1, CA_DOCK_MAIN_WINDOW, main_root, CA_DOCK_ZONE_BOTTOM, -1, 0.0f));
    CHECK(ca_dock_layout_window_root(layout, w1) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_window_of(layout, ca_dock_layout_group_of(layout, 4)) == CA_DOCK_MAIN_WINDOW);
    CHECK(ca_dock_layout_valid(layout));
    CHECK(!ca_dock_layout_dock_window(layout, CA_DOCK_MAIN_WINDOW, 1, CA_DOCK_NONE, CA_DOCK_ZONE_TAB, -1, 0.0f));

    const int w3 = ca_dock_layout_float(layout, 3, 0, 0, 200, 200);
    CHECK(w3 == 1);
    ca_dock_layout_remove(layout, 3);
    CHECK(ca_dock_layout_window_root(layout, w3) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_destroy(layout);
}

static void test_new_split_validation(void)
{
    Ca_DockLayout *layout = make_layout(2);
    const int a = ca_dock_layout_new_group(layout);
    const int b = ca_dock_layout_new_group(layout);
    CHECK(ca_dock_layout_new_split(layout, true, 0.5f, a, a) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_new_split(layout, true, 0.5f, a, 99) == CA_DOCK_NONE);
    const int split = ca_dock_layout_new_split(layout, false, 2.0f, a, b);
    CHECK(split != CA_DOCK_NONE);
    CHECK(ca_dock_node_const(layout, split)->ratio <= 0.95f);
    CHECK(ca_dock_layout_new_split(layout, true, 0.5f, a, split) == CA_DOCK_NONE);
    CHECK(!ca_dock_layout_set_root(layout, CA_DOCK_MAIN_WINDOW, a));
    CHECK(ca_dock_layout_set_root(layout, CA_DOCK_MAIN_WINDOW, split));
    CHECK(!ca_dock_layout_set_root(layout, 1, split));
    ca_dock_layout_add_tab(layout, a, 0, -1);
    ca_dock_layout_add_tab(layout, b, 1, -1);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_destroy(layout);
}

static void test_persistence_roundtrip(void)
{
    Ca_DockLayout *layout = make_layout(5);
    seed_two_groups(layout);
    ca_dock_layout_float(layout, 3, -40, 25, 640, 480);
    ca_dock_layout_split(layout, ca_dock_layout_group_of(layout, 3), 4, CA_DOCK_ZONE_TOP, 0.0f);
    ca_dock_layout_set_active(layout, ca_dock_layout_group_of(layout, 0), 0);
    char *text = save(layout);
    CHECK(text && strstr(text, "\"windows\""));

    Ca_DockLayout *loaded = make_layout(5);
    CHECK(ca_dock_layout_load(loaded, text));
    CHECK(ca_dock_layout_valid(loaded));
    char *again = save(loaded);
    CHECK(again && strcmp(text, again) == 0);
    const Ca_DockWindowEntry *floating = ca_dock_window_entry_const(loaded, 1);
    CHECK(floating && floating->used && floating->x == -40 && floating->width == 640);

    size_t needed = ca_dock_layout_save(layout, NULL, 0);
    char tiny[8];
    CHECK(ca_dock_layout_save(layout, tiny, sizeof(tiny)) == needed);
    CHECK(tiny[sizeof(tiny) - 1] == '\0' && strlen(tiny) == sizeof(tiny) - 1);

    free(text);
    free(again);
    ca_dock_layout_destroy(layout);
    ca_dock_layout_destroy(loaded);
}

static void test_load_tolerance_and_rejection(void)
{
    Ca_DockLayout *layout = make_layout(3);
    seed_two_groups(layout);
    char *before = save(layout);

    const char *unknown =
        "{\"windows\":[{\"x\":0,\"y\":0,\"w\":0,\"h\":0,\"root\":"
        "{\"split\":\"v\",\"ratio\":0.4,\"first\":{\"tabs\":[\"gone\",\"p2\"],\"active\":1},"
        "\"second\":{\"tabs\":[\"also-gone\"],\"active\":0}}},"
        "{\"x\":5,\"y\":5,\"w\":10,\"h\":10,\"root\":{\"tabs\":[\"nobody\"],\"active\":0}}]}";
    CHECK(ca_dock_layout_load(layout, unknown));
    CHECK(ca_dock_layout_valid(layout));
    const int root = ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW);
    CHECK(ca_dock_layout_node_kind(layout, root) == CA_DOCK_NODE_GROUP);
    CHECK(ca_dock_group_tab_count(layout, root) == 1 && ca_dock_group_tab(layout, root, 0) == 2);
    CHECK(ca_dock_layout_window_root(layout, 1) == CA_DOCK_NONE);

    const char *escaped = "{\"windows\":[{\"root\":{\"tabs\":[\"p\\u0030\",\"p\\u0031\"],\"active\":1}}]}";
    CHECK(ca_dock_layout_load(layout, escaped));
    CHECK(ca_dock_layout_active_panel(layout, ca_dock_layout_window_root(layout, 0)) == 1);

    ca_dock_layout_clear(layout);
    seed_two_groups(layout);
    free(before);
    before = save(layout);
    const char *bad[] = {
        "", "null", "[]", "{}", "{\"windows\":[]}", "{\"windows\":[1]}",
        "{\"windows\":[{\"x\":0}]}", "{\"windows\":[{\"root\":{\"split\":\"q\",\"ratio\":0.5}}]}",
        "{\"windows\":[{\"root\":{\"tabs\":\"p0\"}}]}", "{\"windows\":[{\"root\":null}]} trailing",
        "{\"windows\":[{\"root\":{\"tabs\":[\"p0\\uD800\"]}}]}", "{\"windows\":[{\"root\":{\"tabs\":[\"p0",
        "{\"windows\":[{\"root\":{\"split\":\"h\",\"ratio\":0.5,\"first\":{\"tabs\":[\"p0\"]}}}]}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(!ca_dock_layout_load(layout, bad[i]));
        char *after = save(layout);
        CHECK(after && strcmp(after, before) == 0);
        free(after);
    }
    CHECK(!ca_dock_layout_load(layout, NULL));

    enum { DEPTH = 2000 };
    char *deep = malloc(DEPTH * 64 + 64);
    size_t n = (size_t)sprintf(deep, "{\"windows\":[{\"root\":");
    for (int i = 0; i < DEPTH; ++i) n += (size_t)sprintf(deep + n, "{\"split\":\"h\",\"ratio\":0.5,\"first\":");
    sprintf(deep + n, "null}]}");
    CHECK(!ca_dock_layout_load(layout, deep));
    free(deep);

    free(before);
    ca_dock_layout_destroy(layout);
}

static void test_copy(void)
{
    Ca_DockLayout *layout = make_layout(3);
    seed_two_groups(layout);
    Ca_DockLayout *copy = ca_dock_layout_create();
    CHECK(ca_dock_layout_copy(copy, layout));
    ca_dock_layout_remove(layout, 2);
    CHECK(ca_dock_layout_group_of(copy, 2) != CA_DOCK_NONE);
    CHECK(ca_dock_layout_valid(copy) && ca_dock_layout_valid(layout));
    CHECK(ca_dock_layout_copy(copy, copy));
    ca_dock_layout_destroy(layout);
    ca_dock_layout_destroy(copy);
}


/* ImGui: the central group keeps its space when emptied; other groups collapse. */
static void test_central_keeps_space(void)
{
    Ca_DockLayout *layout = make_layout(4);
    seed_two_groups(layout);
    const int right = ca_dock_layout_group_of(layout, 2);
    CHECK(ca_dock_layout_set_central(layout, right));
    CHECK(ca_dock_layout_central(layout) == right);
    CHECK(!ca_dock_layout_set_central(layout, 99));
    CHECK(ca_dock_layout_valid(layout));

    CHECK(ca_dock_layout_float(layout, 2, 0, 0, 200, 200) != CA_DOCK_NONE);
    CHECK(ca_dock_layout_valid(layout));
    CHECK(ca_dock_layout_node_kind(layout, right) == CA_DOCK_NODE_GROUP);
    CHECK(ca_dock_group_tab_count(layout, right) == 0);
    CHECK(ca_dock_layout_node_visible(layout, right));
    const int root = ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW);
    CHECK(ca_dock_layout_node_kind(layout, root) == CA_DOCK_NODE_SPLIT);

    ca_dock_layout_remove(layout, 0);
    ca_dock_layout_remove(layout, 1);
    CHECK(ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW) == right);
    CHECK(ca_dock_layout_valid(layout));

    CHECK(ca_dock_layout_split(layout, right, 3, CA_DOCK_ZONE_LEFT, 0.3f));
    CHECK(ca_dock_layout_central(layout) == right);
    CHECK(ca_dock_contains_central(layout, ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW)));
    CHECK(!ca_dock_contains_central(layout, ca_dock_layout_group_of(layout, 3)));
    CHECK(ca_dock_layout_set_central(layout, CA_DOCK_NONE));
    CHECK(ca_dock_layout_node_kind(layout, right) == CA_DOCK_NODE_NONE);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_destroy(layout);
}

/* ImGui: closing remembers the place; reopening restores it; undocking forgets. */
static void test_close_remembers_home(void)
{
    Ca_DockLayout *layout = make_layout(4);
    seed_two_groups(layout);
    const int right = ca_dock_layout_group_of(layout, 2);
    const int split = ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW);
    ca_dock_layout_set_split_size(layout, split, 240.0f);

    ca_dock_layout_close(layout, 2);
    CHECK(ca_dock_layout_valid(layout));
    CHECK(ca_dock_layout_group_of(layout, 2) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_home_of(layout, 2) == right);
    CHECK(!ca_dock_layout_node_visible(layout, right));
    CHECK(ca_dock_layout_node_kind(layout, split) == CA_DOCK_NODE_SPLIT);
    CHECK(ca_dock_visible_group_count(layout, split) == 1);

    CHECK(ca_dock_layout_add_tab(layout, right, 2, -1));
    CHECK(ca_dock_layout_home_of(layout, 2) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW) == split);
    CHECK(ca_dock_node_const(layout, split)->size == 240.0f);
    CHECK(ca_dock_layout_valid(layout));

    ca_dock_layout_close(layout, 2);
    const int left = ca_dock_layout_group_of(layout, 0);
    CHECK(ca_dock_layout_add_tab(layout, left, 2, -1));
    CHECK(ca_dock_layout_node_kind(layout, right) == CA_DOCK_NODE_NONE);
    CHECK(ca_dock_layout_valid(layout));

    ca_dock_layout_close(layout, 0);
    ca_dock_layout_close(layout, 1);
    ca_dock_layout_close(layout, 2);
    CHECK(ca_dock_layout_valid(layout));
    CHECK(!ca_dock_layout_node_visible(layout, ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW)));
    ca_dock_layout_remove(layout, 0);
    ca_dock_layout_remove(layout, 1);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_remove(layout, 2);
    CHECK(ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW) == CA_DOCK_NONE);
    CHECK(ca_dock_layout_valid(layout));

    const int floating = ca_dock_layout_float(layout, 3, 0, 0, 100, 100);
    ca_dock_layout_close(layout, 3);
    CHECK(ca_dock_window_entry_const(layout, floating)->used);
    CHECK(!ca_dock_layout_node_visible(layout, ca_dock_layout_window_root(layout, floating)));
    CHECK(ca_dock_layout_add_tab(layout, ca_dock_layout_home_of(layout, 3), 3, -1));
    CHECK(ca_dock_layout_window_of(layout, ca_dock_layout_group_of(layout, 3)) == floating);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_destroy(layout);
}

/* ImGui: an empty target is replaced by the whole payload tree; central role moves with it. */
static void test_dock_window_into_central(void)
{
    Ca_DockLayout *layout = make_layout(5);
    seed_two_groups(layout);
    const int right = ca_dock_layout_group_of(layout, 2);
    ca_dock_layout_set_central(layout, right);
    const int w = ca_dock_layout_float(layout, 2, 0, 0, 300, 300);
    CHECK(ca_dock_layout_split(layout, ca_dock_layout_group_of(layout, 2), 3, CA_DOCK_ZONE_RIGHT, 0.0f));
    ca_dock_layout_close(layout, 4);
    CHECK(ca_dock_layout_valid(layout));

    const int other = ca_dock_layout_group_of(layout, 0);
    CHECK(!ca_dock_layout_dock_window(layout, w, CA_DOCK_MAIN_WINDOW, other, CA_DOCK_ZONE_TAB, -1, 0.0f));
    CHECK(ca_dock_layout_dock_window(layout, w, CA_DOCK_MAIN_WINDOW, right, CA_DOCK_ZONE_TAB, -1, 0.0f));
    CHECK(ca_dock_layout_valid(layout));
    CHECK(ca_dock_layout_node_kind(layout, right) == CA_DOCK_NODE_NONE || ca_dock_layout_group_of(layout, 2) == right);
    const int central = ca_dock_layout_central(layout);
    CHECK(central != CA_DOCK_NONE && ca_dock_group_tab_count(layout, central) > 0);
    CHECK(ca_dock_layout_window_of(layout, ca_dock_layout_group_of(layout, 3)) == CA_DOCK_MAIN_WINDOW);
    CHECK(!ca_dock_window_entry_const(layout, w)->used);

    const int w2 = ca_dock_layout_float(layout, 3, 0, 0, 300, 300);
    ca_dock_layout_add_tab(layout, ca_dock_layout_group_of(layout, 3), 4, -1);
    ca_dock_layout_close(layout, 4);
    CHECK(ca_dock_layout_dock_window(layout, w2, CA_DOCK_MAIN_WINDOW, other, CA_DOCK_ZONE_TAB, 0, 0.0f));
    CHECK(ca_dock_layout_group_of(layout, 3) == other);
    CHECK(ca_dock_layout_home_of(layout, 4) == other);
    CHECK(ca_dock_layout_valid(layout));
    ca_dock_layout_destroy(layout);
}

static void test_imgui_state_persists(void)
{
    Ca_DockLayout *layout = make_layout(4);
    seed_two_groups(layout);
    const int right = ca_dock_layout_group_of(layout, 2);
    ca_dock_layout_set_central(layout, right);
    ca_dock_layout_set_split_size(layout, ca_dock_layout_window_root(layout, CA_DOCK_MAIN_WINDOW), 312.0f);
    ca_dock_layout_close(layout, 1);
    ca_dock_layout_float(layout, 2, 0, 0, 100, 100);
    char *text = save(layout);
    CHECK(text && strstr(text, "\"central\":true") && strstr(text, "\"closed\":[\"p1\"]") && strstr(text, "\"size\":312"));

    Ca_DockLayout *loaded = make_layout(4);
    CHECK(ca_dock_layout_load(loaded, text));
    CHECK(ca_dock_layout_valid(loaded));
    const int central = ca_dock_layout_central(loaded);
    CHECK(central != CA_DOCK_NONE && ca_dock_group_tab_count(loaded, central) == 0);
    CHECK(ca_dock_layout_home_of(loaded, 1) == ca_dock_layout_group_of(loaded, 0));
    char *again = save(loaded);
    CHECK(again && strcmp(text, again) == 0);

    const char *floating_central =
        "{\"windows\":[{\"root\":{\"tabs\":[\"p0\"],\"active\":0}},"
        "{\"x\":1,\"y\":1,\"w\":9,\"h\":9,\"root\":{\"tabs\":[\"p1\"],\"active\":0,\"central\":true}}]}";
    CHECK(ca_dock_layout_load(loaded, floating_central));
    CHECK(ca_dock_layout_central(loaded) == CA_DOCK_NONE);
    free(text);
    free(again);
    ca_dock_layout_destroy(layout);
    ca_dock_layout_destroy(loaded);
}

/* Every mutation either succeeds or leaves the saved form byte-identical. */
static void test_failure_atomicity(void)
{
    enum { OPS = 6 };
    for (int op = 0; op < OPS; ++op) {
        int injected_failures = 0;
        bool allocated = false;
        for (size_t attempt = 1; attempt < 200; ++attempt) {
            Ca_DockLayout *layout = make_layout(6);
            seed_two_groups(layout);
            ca_dock_layout_float(layout, 4, 0, 0, 100, 100);
            char *before = save(layout);
            const int left = ca_dock_layout_group_of(layout, 0);
            const int floating = ca_dock_layout_group_of(layout, 4);

            arm_failure(attempt);
            bool ok = false;
            switch (op) {
            case 0: ok = ca_dock_layout_add_tab(layout, ca_dock_layout_group_of(layout, 2), 5, -1); break;
            case 1: ok = ca_dock_layout_split(layout, left, 5, CA_DOCK_ZONE_TOP, 0.0f); break;
            case 2: ok = ca_dock_layout_dock_root(layout, CA_DOCK_MAIN_WINDOW, 5, CA_DOCK_ZONE_RIGHT, 0.0f); break;
            case 3: ok = ca_dock_layout_float(layout, 1, 0, 0, 50, 50) != CA_DOCK_NONE; break;
            case 4: ok = ca_dock_layout_dock_window(layout, 1, CA_DOCK_MAIN_WINDOW, left, CA_DOCK_ZONE_TAB, -1, 0.0f); break;
            default: ok = ca_dock_layout_dock_window(layout, 1, CA_DOCK_MAIN_WINDOW, left, CA_DOCK_ZONE_LEFT, -1, 0.0f); break;
            }
            const bool injected = g_allocation_attempts >= attempt;
            allocated = allocated || g_allocation_attempts > 0;
            disarm_failure();
            (void)floating;

            CHECK(ca_dock_layout_valid(layout));
            if (!ok) {
                injected_failures++;
                char *after = save(layout);
                CHECK(after && strcmp(before, after) == 0);
                free(after);
            }
            free(before);
            ca_dock_layout_destroy(layout);
            if (!injected) break;
        }
        /* An operation that allocates must have been failed at least once. */
        CHECK(injected_failures > 0 || !allocated);
    }
}

/* Long random operation sequences never break invariants, at sizes well past any fixed cap. */
static void test_random_operations(void)
{
    enum { PANELS = 300, STEPS = 20000 };
    Ca_DockLayout *layout = make_layout(PANELS);
    srand(1234);
    for (int step = 0; step < STEPS; ++step) {
        const int panel = rand() % PANELS;
        const int other = ca_dock_layout_group_of(layout, rand() % PANELS);
        const Ca_DockZone zone = (Ca_DockZone)(rand() % 5);
        const int windows = ca_dock_layout_window_count(layout);
        switch (rand() % 9) {
        case 0: if (other != CA_DOCK_NONE) ca_dock_layout_add_tab(layout, other, panel, rand() % 4 - 1); break;
        case 1: if (other != CA_DOCK_NONE) ca_dock_layout_split(layout, other, panel, zone, 0.0f); break;
        case 2: ca_dock_layout_dock_root(layout, rand() % windows, panel, zone, 0.0f); break;
        case 3: ca_dock_layout_float(layout, panel, 0, 0, 100, 100); break;
        case 4: ca_dock_layout_remove(layout, panel); break;
        case 5: {
            const int source = 1 + rand() % (windows > 1 ? windows - 1 : 1);
            const int dest = rand() % windows;
            const int target = ca_dock_first_group(layout, ca_dock_layout_window_root(layout, dest));
            ca_dock_layout_dock_window(layout, source, dest, target, zone, -1, 0.0f);
            break;
        }
        case 6: ca_dock_layout_close(layout, panel); break;
        case 7: {
            const int home = ca_dock_layout_home_of(layout, panel);
            if (home != CA_DOCK_NONE) ca_dock_layout_add_tab(layout, home, panel, -1);
            else if (other != CA_DOCK_NONE && ca_dock_layout_window_of(layout, other) == CA_DOCK_MAIN_WINDOW)
                ca_dock_layout_set_central(layout, rand() % 4 ? other : CA_DOCK_NONE);
            break;
        }
        default:
            if (other != CA_DOCK_NONE) ca_dock_layout_set_active(layout, other, rand() % 3);
            break;
        }
        if (!ca_dock_layout_valid(layout)) {
            CHECK(!"layout invalid after random operation");
            break;
        }
        if (step % 2500 == 0) {
            char *text = save(layout);
            Ca_DockLayout *loaded = make_layout(PANELS);
            CHECK(text && ca_dock_layout_load(loaded, text));
            char *again = save(loaded);
            CHECK(again && strcmp(text, again) == 0);
            free(text);
            free(again);
            ca_dock_layout_destroy(loaded);
        }
    }
    ca_dock_layout_destroy(layout);
}

int main(void)
{
    test_registration();
    test_tabs_and_reorder();
    test_split_and_collapse();
    test_dock_root();
    test_float_and_dock_window();
    test_new_split_validation();
    test_persistence_roundtrip();
    test_load_tolerance_and_rejection();
    test_copy();
    test_central_keeps_space();
    test_close_remembers_home();
    test_dock_window_into_central();
    test_imgui_state_persists();
    test_failure_atomicity();
    test_random_operations();
    if (g_failures) {
        fprintf(stderr, "ca_dock_layout_tests: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("ca_dock_layout_tests: all passed\n");
    return 0;
}
