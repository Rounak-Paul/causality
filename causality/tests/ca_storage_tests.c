// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#include "ca_node_graph.h"
#include "css.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                  \
                    __FILE__, __LINE__, #condition);                            \
            return false;                                                       \
        }                                                                       \
    } while (0)

/** Appends a NUL-terminated fragment while retaining one trailing NUL. */
static bool append_text(Ca_DynArray *text, const char *fragment)
{
    size_t length = strlen(fragment);
    if (text->count > 0) text->count--;
    if (!ca_dyn_array_append(text, fragment, length + 1u)) return false;
    return true;
}

/** Verifies graph state grows beyond the removed fixed node-state ceiling. */
static bool test_node_graph_growth(void)
{
    Ca_NodeGraph graph;
    CHECK(ca_node_graph_init(&graph));
    char key[64];
    for (int i = 0; i < 2048; ++i) {
        snprintf(key, sizeof(key), "node_%d", i);
        Ca_NgNodeState *state =
            ca_node_graph_add_state(&graph, key, (float)i, (float)-i);
        CHECK(state);
        CHECK(state->_index == i);
    }
    CHECK(graph.node_count == 2048);
    CHECK(strcmp(ca_node_graph_state(&graph, 1537)->key, "node_1537") == 0);
    ca_node_graph_destroy(&graph);
    CHECK(graph.node_count == 0);
    return true;
}

/** Verifies stylesheet rules, selector lists, and declarations grow on demand. */
static bool test_stylesheet_growth(void)
{
    Ca_DynArray css = CA_DYN_ARRAY_INIT(char);
    CHECK(append_text(&css, ""));
    for (int rule = 0; rule < 1100; ++rule) {
        char selector[64];
        snprintf(selector, sizeof(selector), ".rule_%d", rule);
        CHECK(append_text(&css, selector));
        if (rule == 0) {
            for (int i = 1; i < 40; ++i) {
                snprintf(selector, sizeof(selector), ",.selector_%d", i);
                CHECK(append_text(&css, selector));
            }
        }
        CHECK(append_text(&css, "{"));
        for (int declaration = 0; declaration < 120; ++declaration)
            CHECK(append_text(&css, "margin-left:1px;"));
        CHECK(append_text(&css, "}"));
    }
    for (int part = 0; part < 12; ++part) {
        if (part > 0) CHECK(append_text(&css, ">"));
        CHECK(append_text(&css, "div"));
        for (int class_index = 0; class_index < 12; ++class_index) {
            char class_name[32];
            snprintf(class_name, sizeof(class_name), ".p%d_c%d", part, class_index);
            CHECK(append_text(&css, class_name));
        }
        CHECK(append_text(&css,
                          ":hover:active:focus:enabled:first-child:last-child"));
    }
    CHECK(append_text(&css, "{color:#fff;}"));

    Ca_Stylesheet *stylesheet = ca_css_parse((const char *)css.data);
    CHECK(stylesheet);
    CHECK(stylesheet->rule_count == 1101);
    CHECK(stylesheet->rules[0].selector_count == 40);
    CHECK(stylesheet->rules[0].decl_count == 120);
    Ca_CssSelector *complex = &stylesheet->rules[1100].selectors[0];
    CHECK(complex->part_count == 12);
    for (int part = 0; part < complex->part_count; ++part) {
        CHECK(complex->parts[part].class_count == 12);
        CHECK(complex->parts[part].pseudo_count == 6);
    }
    CHECK(sizeof(*stylesheet) < 512u);
    ca_css_destroy(stylesheet);
    ca_dyn_array_destroy(&css);
    return true;
}

/** Verifies responsive anchor declarations and retained stylesheet lifetime. */
static bool test_stylesheet_anchors(void)
{
    Ca_Stylesheet *stylesheet = ca_css_parse(
        ".overlay{position:absolute;right:5%;bottom:12px;}");
    CHECK(stylesheet);
    CHECK(stylesheet->ref_count == 1u);
    CHECK(stylesheet->rule_count == 1);
    CHECK(stylesheet->rules[0].decl_count == 3);
    CHECK(stylesheet->rules[0].decls[0].prop == CA_CSS_PROP_POSITION);
    CHECK(stylesheet->rules[0].decls[1].prop == CA_CSS_PROP_RIGHT);
    CHECK(stylesheet->rules[0].decls[1].value.type == CA_CSS_VAL_PERCENT);
    CHECK(stylesheet->rules[0].decls[2].prop == CA_CSS_PROP_BOTTOM);
    CHECK(ca_css_retain(stylesheet) == stylesheet);
    CHECK(stylesheet->ref_count == 2u);
    ca_css_destroy(stylesheet);
    CHECK(stylesheet->ref_count == 1u);
    ca_css_destroy(stylesheet);
    return true;
}

int main(void)
{
    if (!test_node_graph_growth() || !test_stylesheet_growth() ||
        !test_stylesheet_anchors()) return 1;
    puts("causality dynamic storage tests passed");
    return 0;
}
