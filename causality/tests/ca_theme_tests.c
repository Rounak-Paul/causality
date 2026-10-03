// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "causality.h"
#include "css.h"

#include <stdio.h>
#include <string.h>

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

/** The built-in palette is fully populated with distinct depth levels. */
static bool test_default_theme(void)
{
    const Ca_Theme t = ca_theme_default();
    CHECK(t.bg_base && t.bg_elevated && t.bg_surface && t.bg_overlay);
    CHECK(t.text_bright && t.text_muted && t.text_dim);
    CHECK(t.accent && t.on_accent && t.success && t.warning && t.danger);
    CHECK(t.bg_base != t.bg_surface && t.bg_surface != t.bg_overlay);
    CHECK(t.text_bright != t.text_muted && t.text_muted != t.text_dim);
    return true;
}

int main(void)
{
    if (!test_set_color_var()) return 1;
    if (!test_default_theme()) return 1;
    printf("causality_theme_tests passed\n");
    return 0;
}
