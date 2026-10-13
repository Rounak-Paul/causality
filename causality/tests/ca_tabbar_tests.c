// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

typedef struct GLFWwindow GLFWwindow;
#include "node.h"
#include "style.h"

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

int main(void)
{
    struct { const char *name; bool (*fn)(void); } tests[] = {
        { "trimming_tab_headers_keeps_bar_widget", test_trimming_tab_headers_keeps_bar_widget },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        bool ok = tests[i].fn();
        printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (!ok) failed++;
    }
    return failed ? 1 : 0;
}
