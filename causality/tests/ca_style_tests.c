// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "causality.h"
#include "css.h"
#include <GLFW/glfw3.h>
#include "style.h"

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

int main(void)
{
    if (!test_set_color_var()) return 1;
    if (!test_default_palette()) return 1;
    if (!test_author_vars_override_defaults()) return 1;
    if (!test_border_shorthand_resets_sides()) return 1;
    printf("causality_style_tests passed\n");
    return 0;
}
