// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

typedef struct GLFWwindow GLFWwindow;
#include "node.h"
#include "style.h"
#include "widget.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

/** Tab headers borrow the bar's widget; dropping them must not release the bar. */
static bool test_trimming_tab_headers_keeps_bar_widget(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    window.instance = &instance;
    CHECK(ca_node_system_init(&window));

    Ca_Node *root = ca_node_root(&window);
    CHECK(root);
    Ca_NodeDesc desc = {0};
    Ca_Node *bar = ca_node_add(root, &desc);
    CHECK(bar);
    Ca_TabBar *tabs = ca_pool_acquire(&window.tabbar_pool);
    CHECK(tabs);
    tabs->in_use = true;
    tabs->node = bar;
    CHECK(ca_dyn_array_init(&tabs->tab_node_storage, sizeof(Ca_Node *)));
    CHECK(ca_dyn_array_init(&tabs->label_storage, sizeof(Ca_OptionText)));
    CHECK(ca_dyn_array_init(&tabs->text_color_storage, sizeof(uint32_t)));
    CHECK(ca_dyn_array_reserve(&tabs->tab_node_storage, 2));
    bar->widget_type = CA_WIDGET_TABBAR;
    bar->widget = tabs;
    bar->elem_type = CA_ELEM_TABBAR;

    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < 3; ++i) {
            Ca_Node *tab = ca_node_add(bar, &desc);
            CHECK(tab);
            tab->widget_type = CA_WIDGET_TABBAR;
            tab->widget = tabs;
            tab->elem_type = CA_ELEM_TAB;
        }
        CHECK(bar->child_count == 3);
        ca_node_trim_children(bar, 0);
        CHECK(bar->child_count == 0);
        CHECK(tabs->in_use);
        CHECK(tabs->tab_node_storage.data != NULL);
    }

    ca_node_system_shutdown(&window);
    return true;
}

/** The indicator snaps to the first laid-out active tab, then eases to a newly active one. */
static bool test_indicator_slides_to_active_tab(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    window.instance = &instance;
    CHECK(ca_node_system_init(&window));

    Ca_NodeDesc desc = {0};
    Ca_Node *bar = ca_node_add(ca_node_root(&window), &desc);
    Ca_Node *indicator = ca_node_add(bar, &desc);
    Ca_Node *tab_a = ca_node_add(bar, &desc);
    Ca_Node *tab_b = ca_node_add(bar, &desc);
    CHECK(bar && indicator && tab_a && tab_b);
    bar->x = 10.0f; bar->y = 20.0f; bar->desc.padding_left = 2.0f; bar->desc.padding_top = 2.0f;
    tab_a->x = 12.0f;  tab_a->y = 22.0f; tab_a->w = 50.0f; tab_a->h = 20.0f;
    tab_b->x = 64.0f;  tab_b->y = 22.0f; tab_b->w = 80.0f; tab_b->h = 20.0f;

    Ca_Node *headers[2] = { tab_a, tab_b };
    Ca_TabBar *tabs = ca_pool_acquire(&window.tabbar_pool);
    CHECK(tabs);
    tabs->in_use = true;
    tabs->node = bar;
    tabs->indicator = indicator;
    tabs->tab_nodes = headers;
    tabs->count = 2;
    tabs->active = 0;

    CHECK(!ca_tabbar_tick(&window, 1.0));
    CHECK(tabs->ind_valid && tabs->ind_cur[0] == 0.0f && tabs->ind_cur[2] == 50.0f);
    CHECK(!indicator->desc.hidden && indicator->desc.position == CA_POSITION_ABSOLUTE);

    tabs->active = 1;
    CHECK(ca_tabbar_tick(&window, 2.0));
    CHECK(tabs->ind_cur[0] == 0.0f);
    /* Mid-flight the leading (right) edge is ahead of the lagging (left) edge:
       the pill is stretched beyond both resting widths and squashed in height. */
    CHECK(ca_tabbar_tick(&window, 2.0 + CA_TABBAR_SLIDE_SECONDS * 0.4));
    CHECK(tabs->ind_cur[0] > 0.0f && tabs->ind_cur[0] < 52.0f);
    CHECK(tabs->ind_cur[2] > 80.0f);
    CHECK(tabs->ind_cur[3] < 20.0f);
    CHECK(tabs->ind_cur[0] + tabs->ind_cur[2] < 132.0f * 1.25f);
    CHECK(!ca_tabbar_tick(&window, 2.0 + CA_TABBAR_SLIDE_SECONDS * 2.0));
    CHECK(tabs->ind_cur[0] == 52.0f && tabs->ind_cur[2] == 80.0f);
    CHECK(indicator->desc.pos_x == 52.0f && indicator->base_desc.width == 80.0f);

    /* A tab that never gets a rect must not keep the frame loop alive forever. */
    tab_b->w = 0.0f;
    tabs->active = 1;
    int busy_frames = 0;
    for (int i = 0; i < 10; ++i) if (ca_tabbar_tick(&window, 5.0 + i)) busy_frames++;
    CHECK(busy_frames == 3);

    ca_node_system_shutdown(&window);
    return true;
}

int main(void)
{
    struct { const char *name; bool (*fn)(void); } tests[] = {
        { "trimming_tab_headers_keeps_bar_widget", test_trimming_tab_headers_keeps_bar_widget },
        { "indicator_slides_to_active_tab", test_indicator_slides_to_active_tab },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        bool ok = tests[i].fn();
        printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (!ok) failed++;
    }
    return failed ? 1 : 0;
}
