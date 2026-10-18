// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

typedef struct GLFWwindow GLFWwindow;
#include "motion.h"
#include "node.h"
#include "style.h"
#include "widget.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

/* A throwaway window with one node, enough to run motion without a renderer. */
typedef struct {
    Ca_Instance instance;
    Ca_Window   window;
    Ca_Node    *node;
} Rig;

static bool rig_init(Rig *rig, const char *css)
{
    memset(rig, 0, sizeof(*rig));
    rig->window.instance = &rig->instance;
    rig->window.ui_scale = 1.0f;
    rig->instance.palette.text_bright = 0xEEEEEEFFu;
    rig->instance.palette.text_dim = 0x888888FFu;
    if (css) rig->instance.stylesheet = ca_css_parse(css);
    if (css && !rig->instance.stylesheet) return false;
    if (!ca_node_system_init(&rig->window)) return false;
    const Ca_NodeDesc desc = {0};
    rig->node = ca_node_add(ca_node_root(&rig->window), &desc);
    rig->node->window = &rig->window;
    return rig->node != NULL;
}

static void rig_destroy(Rig *rig)
{
    ca_node_system_shutdown(&rig->window);
    ca_css_destroy(rig->instance.stylesheet);
}

static Ca_EasingDef linear_easing(void)
{
    return (Ca_EasingDef){ CA_EASING_LINEAR, { 0 } };
}

/** Replaces a node's zipped transition list with `count` specs. */
static void set_transitions(Ca_Node *node, const Ca_TransitionSpec *specs, uint32_t count)
{
    CA_FREE(node->transition_specs);
    node->transition_specs = CA_CALLOC(count ? count : 1u, sizeof(Ca_TransitionSpec));
    memcpy(node->transition_specs, specs, sizeof(Ca_TransitionSpec) * count);
    node->transition_spec_count = count;
}

/** Replaces a node's zipped animation list with one spec (or none). */
static void set_animation(Ca_Node *node, const Ca_AnimSpec *spec)
{
    CA_FREE(node->anim_specs);
    node->anim_specs = spec ? CA_CALLOC(1, sizeof(Ca_AnimSpec)) : NULL;
    if (spec) *node->anim_specs = *spec;
    node->anim_spec_count = spec ? 1u : 0u;
}

static Ca_AnimSpec anim_spec(const char *name, float duration, float delay, float iterations,
                             uint8_t direction, uint8_t fill)
{
    return (Ca_AnimSpec){ .name = ca_css_name_hash(name), .duration = duration, .delay = delay,
                          .iterations = iterations, .direction = direction, .fill = fill,
                          .easing = linear_easing() };
}

/** The zipped lists repeat the shorter ones cyclically and carry per-item timing. */
static bool test_store_zips_lists_cyclically(void)
{
    Ca_Stylesheet *ss = ca_css_parse(
        ".a { transition: left 1s, top 2s, opacity 3s; transition-delay: 100ms, 200ms; }"
        ".b { animation: one 1s, two 2s; animation-iteration-count: 3; animation-play-state: paused, running; }");
    CHECK(ss && ss->rule_count == 2);
    Ca_ResolvedStyle rs = {0};
    for (int r = 0; r < 2; ++r)
        for (int i = 0; i < ss->rules[r].decl_count; ++i)
            ca_style_apply_one_declaration(&rs, ss->rules[r].decls[i].prop, &ss->rules[r].decls[i].value);

    Ca_Node node = {0};
    ca_motion_store(&node, &rs);
    CHECK(node.transition_spec_count == 3);
    CHECK(node.transition_specs[0].delay == 0.1f && node.transition_specs[1].delay == 0.2f);
    CHECK(node.transition_specs[2].delay == 0.1f);   /* two delays cycle over three properties */
    CHECK(node.transition_specs[2].duration == 3.0f && node.transition_specs[2].prop == CA_CSS_PROP_OPACITY);
    CHECK(node.anim_spec_count == 2);
    CHECK(node.anim_specs[0].iterations == 3.0f && node.anim_specs[1].iterations == 3.0f);
    CHECK(node.anim_specs[1].name == ca_css_name_hash("two") && node.anim_specs[1].duration == 2.0f);
    CHECK(node.anim_specs[0].paused && !node.anim_specs[1].paused);

    /* Storing the same lists again keeps the zipped specs as they are. */
    const Ca_TransitionSpec *before = node.transition_specs;
    ca_motion_store(&node, &rs);
    CHECK(node.transition_specs == before);

    ca_motion_node_destroy(&node);
    ca_css_destroy(ss);
    return true;
}

/** Forty transitions and forty animations on one node all take effect. */
static bool test_store_handles_long_lists(void)
{
    char css[8192] = ".x { transition:";
    char anim[8192] = " animation:";
    for (int i = 0; i < 40; ++i) {
        char item[64];
        snprintf(item, sizeof(item), "%s p%d %ds", i ? "," : "", i, i + 1);
        strncat(css, item, sizeof(css) - strlen(css) - 1);
        snprintf(item, sizeof(item), "%s a%d %ds", i ? "," : "", i, i + 1);
        strncat(anim, item, sizeof(anim) - strlen(anim) - 1);
    }
    strncat(css, ";", sizeof(css) - strlen(css) - 1);
    strncat(css, anim, sizeof(css) - strlen(css) - 1);
    strncat(css, "; }", sizeof(css) - strlen(css) - 1);
    Ca_Stylesheet *ss = ca_css_parse(css);
    CHECK(ss && ss->rule_count == 1);
    Ca_ResolvedStyle rs = {0};
    for (int i = 0; i < ss->rules[0].decl_count; ++i)
        ca_style_apply_one_declaration(&rs, ss->rules[0].decls[i].prop, &ss->rules[0].decls[i].value);
    Ca_Node node = {0};
    ca_motion_store(&node, &rs);
    CHECK(node.transition_spec_count == 40 && node.anim_spec_count == 40);
    CHECK(node.transition_specs[39].duration == 40.0f && node.anim_specs[39].duration == 40.0f);
    ca_motion_node_destroy(&node);
    ca_css_destroy(ss);
    return true;
}

/** A transition holds the old value through its delay, eases, lands exactly, then goes quiet. */
static bool test_transition_runs_with_delay(void)
{
    Rig rig;
    CHECK(rig_init(&rig, NULL));
    Ca_Node *node = rig.node;
    set_transitions(node, &(Ca_TransitionSpec){ CA_MOTION_ALL, 1.0f, 0.5f, linear_easing() }, 1);

    node->desc.background = 0x000000FFu;
    node->desc.width = 100.0f;
    const Ca_NodeDesc old = node->desc;
    node->desc.background = 0xFF0000FFu;
    node->desc.width = 200.0f;
    ca_motion_begin_transitions(node, &old, 0u, 0u);

    CHECK(node->desc.background == 0x000000FFu && node->desc.width == 100.0f);   /* no flash of the target */
    CHECK(node->transition_storage.count == 2);
    for (size_t i = 0; i < node->transition_storage.count; ++i) node->transitions[i].start_time = 10.0;

    bool layout = false;
    CHECK(ca_motion_tick(node, 10.2, &layout));
    CHECK(node->desc.background == 0x000000FFu && node->desc.width == 100.0f);   /* still in the delay */

    CHECK(ca_motion_tick(node, 11.0, &layout));   /* half way through the 1s after the 0.5s delay */
    const uint32_t red = node->desc.background >> 24;
    CHECK(red >= 126 && red <= 129);
    CHECK(fabsf(node->desc.width - 150.0f) < 0.01f);
    CHECK(layout);

    CHECK(ca_motion_tick(node, 13.0, &layout));   /* lands on the final value and is still reported busy */
    CHECK(node->desc.background == 0xFF0000FFu && node->desc.width == 200.0f);
    CHECK(!ca_motion_tick(node, 13.1, &layout));

    rig_destroy(&rig);
    return true;
}

/** Specs pick which properties transition: groups cover their members, `none` and omissions do not. */
static bool test_transition_matching(void)
{
    Rig rig;
    CHECK(rig_init(&rig, NULL));
    Ca_Node *node = rig.node;

    set_transitions(node, &(Ca_TransitionSpec){ CA_MOTION_GROUP_PADDING, 1.0f, 0.0f, linear_easing() }, 1);
    Ca_NodeDesc old = node->desc;
    node->desc.padding_left = 20.0f;
    node->desc.opacity = 0.5f;
    ca_motion_begin_transitions(node, &old, 0u, 0u);
    CHECK(node->transition_storage.count == 1);        /* padding-left only; opacity has no spec */
    CHECK(node->desc.padding_left == 0.0f && node->desc.opacity == 0.5f);

    set_transitions(node, &(Ca_TransitionSpec){ 0, 1.0f, 0.0f, linear_easing() }, 1);   /* none */
    old = node->desc;
    node->desc.padding_top = 9.0f;
    ca_motion_begin_transitions(node, &old, 0u, 0u);
    CHECK(node->desc.padding_top == 9.0f);              /* applied instantly */

    set_transitions(node, &(Ca_TransitionSpec){ CA_CSS_PROP_PADDING_TOP, 0.0f, 0.0f, linear_easing() }, 1);
    old = node->desc;
    node->desc.padding_top = 3.0f;
    ca_motion_begin_transitions(node, &old, 0u, 0u);
    CHECK(node->desc.padding_top == 3.0f);              /* zero duration and delay: instant */

    /* Later entries win, as in CSS: `all` first, then a slower `opacity`. */
    const Ca_TransitionSpec both[2] = {
        { CA_MOTION_ALL, 1.0f, 0.0f, linear_easing() },
        { CA_CSS_PROP_OPACITY, 4.0f, 0.0f, linear_easing() },
    };
    set_transitions(node, both, 2);
    CHECK(ca_motion_find_spec(node, CA_CSS_PROP_OPACITY)->duration == 4.0f);
    CHECK(ca_motion_find_spec(node, CA_CSS_PROP_WIDTH)->duration == 1.0f);

    rig_destroy(&rig);
    return true;
}

/** Leaving a state before its transition ends cancels it; re-asking for the same target does not restart it. */
static bool test_transition_retarget_and_cancel(void)
{
    Rig rig;
    CHECK(rig_init(&rig, NULL));
    Ca_Node *node = rig.node;
    set_transitions(node, &(Ca_TransitionSpec){ CA_CSS_PROP_OPACITY, 1.0f, 0.0f, linear_easing() }, 1);

    node->desc.opacity = 1.0f;
    Ca_NodeDesc old = node->desc;
    node->desc.opacity = 0.5f;                                /* "hover" */
    ca_motion_begin_transitions(node, &old, 0u, 0u);
    CHECK(node->desc.opacity == 1.0f && node->transitions[0].active);

    /* "Unhover" in the same frame: the style is back on what is shown, so nothing may carry on to 0.5. */
    old = node->desc;
    ca_motion_begin_transitions(node, &old, 0u, 0u);
    CHECK(!node->transitions[0].active);
    bool layout = false;
    ca_motion_tick(node, 100.0, &layout);
    CHECK(node->desc.opacity == 1.0f);

    /* Re-resolving to the target a transition is already heading for leaves it running. */
    old = node->desc;
    node->desc.opacity = 0.5f;
    ca_motion_begin_transitions(node, &old, 0u, 0u);
    node->transitions[0].start_time = 200.0;
    ca_motion_tick(node, 200.5, &layout);
    CHECK(fabsf(node->desc.opacity - 0.75f) < 0.001f);
    old = node->desc;                                          /* shown 0.75, resolved target still 0.5 */
    node->desc.opacity = 0.5f;
    const double started = node->transitions[0].start_time;
    ca_motion_begin_transitions(node, &old, 0u, 0u);
    CHECK(node->transitions[0].start_time == started);

    rig_destroy(&rig);
    return true;
}

/** Text colours transition too, from and to the palette default, and settle back on "no colour". */
static bool test_text_colour_transitions(void)
{
    Rig rig;
    CHECK(rig_init(&rig, NULL));
    Ca_Node *node = rig.node;
    Ca_Label label = {0};
    node->widget_type = CA_WIDGET_LABEL;
    node->widget = &label;
    set_transitions(node, &(Ca_TransitionSpec){ CA_CSS_PROP_COLOR, 1.0f, 0.0f, linear_easing() }, 1);

    /* No colour of its own (palette 0xEEEEEEFF) -> red. */
    const Ca_NodeDesc old = node->desc;
    ca_widget_set_text_color(node, 0xFF0000FFu);
    ca_motion_begin_transitions(node, &old, 0u, 0xFF0000FFu);
    CHECK(label.color == 0xEEEEEEFFu);                 /* the old, palette-default look is held */
    node->transitions[0].start_time = 20.0;
    bool layout = false;
    CHECK(ca_motion_tick(node, 20.5, &layout));
    const uint32_t g = (label.color >> 16) & 0xFFu;
    CHECK(g > 0x50 && g < 0x90);                       /* half way between 0xEE and 0x00 */
    CHECK(ca_motion_tick(node, 22.0, &layout));
    CHECK(label.color == 0xFF0000FFu);

    /* Red -> no colour: lands back on "palette default", not on a frozen explicit colour. */
    ca_widget_set_text_color(node, 0u);
    ca_motion_begin_transitions(node, &old, 0xFF0000FFu, 0u);
    CHECK(label.color == 0xFF0000FFu);
    node->transitions[0].start_time = 30.0;
    CHECK(ca_motion_tick(node, 32.0, &layout));
    CHECK(label.color == 0u);

    rig_destroy(&rig);
    return true;
}

/** Gradients transition their end colour, angle and centre; switching gradient type is instant. */
static bool test_gradient_transitions(void)
{
    Rig rig;
    CHECK(rig_init(&rig, NULL));
    Ca_Node *node = rig.node;
    set_transitions(node, &(Ca_TransitionSpec){ CA_MOTION_GROUP_BACKGROUND, 1.0f, 0.0f, linear_easing() }, 1);

    node->desc.gradient_type = 2;
    node->desc.gradient_color2 = 0x000000FFu;
    node->desc.gradient_angle = 0.0f;
    Ca_NodeDesc old = node->desc;
    node->desc.gradient_color2 = 0xFF0000FFu;
    node->desc.gradient_angle = 90.0f;
    ca_motion_begin_transitions(node, &old, 0u, 0u);
    CHECK(node->desc.gradient_color2 == 0x000000FFu && node->desc.gradient_angle == 0.0f);
    for (size_t i = 0; i < node->transition_storage.count; ++i) node->transitions[i].start_time = 5.0;
    bool layout = false;
    ca_motion_tick(node, 5.5, &layout);
    CHECK(fabsf(node->desc.gradient_angle - 45.0f) < 0.01f);
    CHECK(((node->desc.gradient_color2 >> 24) & 0xFFu) >= 126 && ((node->desc.gradient_color2 >> 24) & 0xFFu) <= 129);
    ca_motion_tick(node, 7.0, &layout);
    CHECK(node->desc.gradient_color2 == 0xFF0000FFu && node->desc.gradient_angle == 90.0f);

    /* Solid -> gradient has nothing to interpolate between: applied at once. */
    old = node->desc;
    node->desc.gradient_type = 3;
    node->desc.gradient_color2 = 0x00FF00FFu;
    ca_motion_begin_transitions(node, &old, 0u, 0u);
    CHECK(node->desc.gradient_color2 == 0x00FF00FFu);

    rig_destroy(&rig);
    return true;
}

/** Keyframes interpolate between stops, honour direction and iterations, and restore the base. */
static bool test_animation_playback(void)
{
    Rig rig;
    CHECK(rig_init(&rig, "@keyframes grow { from { width: 100px; } to { width: 200px; } }"));
    Ca_Node *node = rig.node;
    node->desc.width = 50.0f;
    const Ca_AnimSpec spec = anim_spec("grow", 1.0f, 0.0f, 2.0f, CA_ANIM_ALTERNATE, CA_ANIM_FILL_NONE);
    set_animation(node, &spec);

    bool layout = false;
    CHECK(ca_motion_tick(node, 100.0, &layout));              /* starts, applies progress 0 */
    CHECK(node->desc.width == 100.0f && layout);
    CHECK(ca_motion_tick(node, 100.5, &layout) && fabsf(node->desc.width - 150.0f) < 0.01f);
    CHECK(ca_motion_tick(node, 101.25, &layout) && fabsf(node->desc.width - 175.0f) < 0.01f);   /* 2nd pass runs backwards */
    CHECK(ca_motion_tick(node, 102.5, &layout));              /* finished: lands, restores the base */
    CHECK(node->desc.width == 50.0f);
    CHECK(!ca_motion_tick(node, 102.6, &layout) && node->desc.width == 50.0f);

    set_animation(node, NULL);                                /* animation removed from the CSS */
    CHECK(!ca_motion_tick(node, 103.0, &layout) && node->anim_count == 0);

    rig_destroy(&rig);
    return true;
}

/** Fill modes hold the first or last state outside the running window; implicit stops use the base. */
static bool test_animation_fill_and_implicit_stops(void)
{
    Rig rig;
    CHECK(rig_init(&rig, "@keyframes to_wide { to { width: 200px; } }"));
    Ca_Node *node = rig.node;
    node->desc.width = 50.0f;
    const Ca_AnimSpec spec = anim_spec("to_wide", 1.0f, 1.0f, 1.0f, CA_ANIM_NORMAL, CA_ANIM_FILL_BOTH);
    set_animation(node, &spec);

    bool layout = false;
    ca_motion_tick(node, 10.0, &layout);                      /* in the 1s delay, fill backwards */
    CHECK(node->desc.width == 50.0f);                          /* the implicit 0% stop is the base */
    ca_motion_tick(node, 11.5, &layout);
    CHECK(fabsf(node->desc.width - 125.0f) < 0.01f);           /* halfway from the base to 200 */
    ca_motion_tick(node, 13.0, &layout);                      /* finished, fill forwards holds 200 */
    CHECK(node->desc.width == 200.0f);
    node->desc.width = 50.0f;                                  /* a rebuild resets the desc to its CSS value */
    ca_motion_tick(node, 13.1, &layout);
    CHECK(node->desc.width == 200.0f);                         /* the held state is written again */

    rig_destroy(&rig);
    return true;
}

/** An unknown @keyframes name animates nothing and never touches the node. */
static bool test_animation_missing_keyframes(void)
{
    Rig rig;
    CHECK(rig_init(&rig, ".x { color: #fff; }"));
    Ca_Node *node = rig.node;
    node->desc.width = 50.0f;
    const Ca_AnimSpec spec = anim_spec("nope", 1.0f, 0.0f, 1.0f, CA_ANIM_NORMAL, CA_ANIM_FILL_NONE);
    set_animation(node, &spec);
    bool layout = false;
    CHECK(!ca_motion_tick(node, 1.0, &layout));
    CHECK(node->desc.width == 50.0f && !layout);
    rig_destroy(&rig);
    return true;
}

/** `paused` freezes an animation's progress; resuming carries on from where it stopped. */
static bool test_animation_play_state(void)
{
    Rig rig;
    CHECK(rig_init(&rig, "@keyframes grow { from { width: 100px; } to { width: 200px; } }"));
    Ca_Node *node = rig.node;
    node->desc.width = 50.0f;
    Ca_AnimSpec spec = anim_spec("grow", 1.0f, 0.0f, 1.0f, CA_ANIM_NORMAL, CA_ANIM_FILL_NONE);
    set_animation(node, &spec);

    bool layout = false;
    ca_motion_tick(node, 0.0, &layout);
    ca_motion_tick(node, 0.25, &layout);
    CHECK(fabsf(node->desc.width - 125.0f) < 0.01f);

    node->anim_specs[0].paused = true;                        /* the pause is noticed on the next tick, at 30% */
    CHECK(!ca_motion_tick(node, 0.30, &layout));              /* nothing is moving: no more frames wanted */
    CHECK(fabsf(node->desc.width - 130.0f) < 0.01f);
    node->desc.width = 50.0f;                                  /* a rebuild resets the desc */
    ca_motion_tick(node, 5.0, &layout);                       /* a long pause changes nothing */
    CHECK(fabsf(node->desc.width - 130.0f) < 0.01f);

    node->anim_specs[0].paused = false;                       /* resume: carries on from 30% */
    CHECK(ca_motion_tick(node, 5.0, &layout));
    CHECK(fabsf(node->desc.width - 130.0f) < 0.01f);
    ca_motion_tick(node, 5.25, &layout);
    CHECK(fabsf(node->desc.width - 155.0f) < 0.01f);

    rig_destroy(&rig);
    return true;
}

/** var() inside @keyframes resolves against the :root variables. */
static bool test_keyframes_resolve_var(void)
{
    Rig rig;
    CHECK(rig_init(&rig, ":root { --target: 300px; --alias: var(--target); } @keyframes grow { to { width: var(--alias); } }"));
    Ca_Node *node = rig.node;
    node->desc.width = 100.0f;
    const Ca_AnimSpec spec = anim_spec("grow", 1.0f, 0.0f, 1.0f, CA_ANIM_NORMAL, CA_ANIM_FILL_FORWARDS);
    set_animation(node, &spec);
    bool layout = false;
    ca_motion_tick(node, 1.0, &layout);
    ca_motion_tick(node, 1.5, &layout);
    CHECK(fabsf(node->desc.width - 200.0f) < 0.01f);           /* halfway from 100 to the variable's 300 */
    ca_motion_tick(node, 3.0, &layout);
    CHECK(node->desc.width == 300.0f);
    rig_destroy(&rig);
    return true;
}

/** Keyframes animate gradients: both colours and the angle, from the node's own gradient. */
static bool test_gradient_keyframes(void)
{
    Rig rig;
    CHECK(rig_init(&rig, "@keyframes shift { to { background: linear-gradient(180deg, #ff0000, #0000ff); } }"));
    Ca_Node *node = rig.node;
    node->desc.gradient_type = 2;
    node->desc.background = 0x000000FFu;
    node->desc.gradient_color2 = 0x000000FFu;
    node->desc.gradient_angle = 0.0f;
    const Ca_AnimSpec spec = anim_spec("shift", 1.0f, 0.0f, 1.0f, CA_ANIM_NORMAL, CA_ANIM_FILL_FORWARDS);
    set_animation(node, &spec);
    bool layout = false;
    ca_motion_tick(node, 0.0, &layout);
    ca_motion_tick(node, 0.5, &layout);
    CHECK(fabsf(node->desc.gradient_angle - 90.0f) < 0.5f);
    CHECK(((node->desc.background >> 24) & 0xFFu) >= 126 && ((node->desc.background >> 24) & 0xFFu) <= 129);
    CHECK(((node->desc.gradient_color2 >> 8) & 0xFFu) >= 126 && ((node->desc.gradient_color2 >> 8) & 0xFFu) <= 129);
    ca_motion_tick(node, 2.0, &layout);
    CHECK(node->desc.background == 0xFF0000FFu && node->desc.gradient_color2 == 0x0000FFFFu);
    CHECK(node->desc.gradient_angle == 180.0f);
    rig_destroy(&rig);
    return true;
}

/** A node recreated in the same place resumes its animation; one that really left starts over. */
static bool test_animation_survives_rebuild(void)
{
    Rig rig;
    CHECK(rig_init(&rig, "@keyframes grow { from { width: 100px; } to { width: 200px; } }"));
    const Ca_AnimSpec spec = anim_spec("grow", 4.0f, 0.0f, 1.0f, CA_ANIM_NORMAL, CA_ANIM_FILL_NONE);
    bool layout = false;

    Ca_Node *node = rig.node;
    snprintf(node->id, sizeof(node->id), "card");
    set_animation(node, &spec);
    ca_motion_tick(node, 50.0, &layout);
    ca_motion_tick(node, 51.0, &layout);                       /* 25% in */
    CHECK(fabsf(node->desc.width - 125.0f) < 0.01f);

    /* A builder rebuilds the subtree: same id, same place, brand new node. */
    ca_node_remove(node);
    const Ca_NodeDesc blank = {0};
    Ca_Node *again = ca_node_add(ca_node_root(&rig.window), &blank);
    again->window = &rig.window;
    snprintf(again->id, sizeof(again->id), "card");
    set_animation(again, &spec);
    again->desc.width = 50.0f;
    ca_motion_tick(again, 51.5, &layout);
    CHECK(fabsf(again->desc.width - 137.5f) < 0.01f);          /* 37.5% in, not 0% */

    /* Gone for longer than the continuity window, it starts over. */
    ca_node_remove(again);
    ca_motion_window_tick(&rig.window, 51.5 + 5.0);
    Ca_Node *fresh = ca_node_add(ca_node_root(&rig.window), &blank);
    fresh->window = &rig.window;
    snprintf(fresh->id, sizeof(fresh->id), "card");
    set_animation(fresh, &spec);
    fresh->desc.width = 50.0f;
    ca_motion_tick(fresh, 60.0, &layout);
    CHECK(fresh->desc.width == 100.0f);

    rig_destroy(&rig);
    return true;
}

/** A pseudo-state rule that moves a layout property takes effect, and transitions. */
static bool test_hover_layout_follows_and_transitions(void)
{
    Rig rig;
    CHECK(rig_init(&rig, ".x { padding: 4px; transition: padding 1s linear; } .x:hover { padding: 12px; }"));
    Ca_Node *node = rig.node;
    snprintf(node->classes, sizeof(node->classes), "x");
    node->elem_type = CA_ELEM_DIV;
    node->base_desc = node->desc;
    node->has_base_desc = true;

    ca_widget_reapply_css(node);                               /* primes the style cache for the resting state */
    node->desc.padding_left = node->desc.padding_top = node->desc.padding_right =
        node->desc.padding_bottom = 4.0f;                      /* what apply_css put there */

    rig.window.hovered_node = node;
    ca_widget_reapply_css(node);
    CHECK(node->desc.padding_left == 4.0f);                    /* held at the old value while it transitions */
    CHECK(node->transition_storage.count >= 1);
    for (size_t i = 0; i < node->transition_storage.count; ++i) node->transitions[i].start_time = 1.0;
    bool layout = false;
    CHECK(ca_motion_tick(node, 1.5, &layout) && layout);
    CHECK(fabsf(node->desc.padding_left - 8.0f) < 0.01f);
    CHECK(ca_motion_tick(node, 3.0, &layout));
    CHECK(node->desc.padding_left == 12.0f && node->desc.padding_bottom == 12.0f);

    rig.window.hovered_node = NULL;                            /* unhover: back to 4, eased */
    ca_widget_reapply_css(node);
    CHECK(node->desc.padding_left == 12.0f);
    for (size_t i = 0; i < node->transition_storage.count; ++i)
        if (node->transitions[i].active) node->transitions[i].start_time = 10.0;
    ca_motion_tick(node, 10.5, &layout);
    CHECK(fabsf(node->desc.padding_left - 8.0f) < 0.01f);
    ca_motion_tick(node, 12.0, &layout);
    CHECK(node->desc.padding_left == 4.0f);

    rig_destroy(&rig);
    return true;
}

int main(void)
{
    struct { const char *name; bool (*fn)(void); } tests[] = {
        { "store_zips_lists_cyclically", test_store_zips_lists_cyclically },
        { "store_handles_long_lists", test_store_handles_long_lists },
        { "transition_runs_with_delay", test_transition_runs_with_delay },
        { "transition_matching", test_transition_matching },
        { "transition_retarget_and_cancel", test_transition_retarget_and_cancel },
        { "text_colour_transitions", test_text_colour_transitions },
        { "gradient_transitions", test_gradient_transitions },
        { "animation_playback", test_animation_playback },
        { "animation_fill_and_implicit_stops", test_animation_fill_and_implicit_stops },
        { "animation_missing_keyframes", test_animation_missing_keyframes },
        { "animation_play_state", test_animation_play_state },
        { "keyframes_resolve_var", test_keyframes_resolve_var },
        { "gradient_keyframes", test_gradient_keyframes },
        { "animation_survives_rebuild", test_animation_survives_rebuild },
        { "hover_layout_follows_and_transitions", test_hover_layout_follows_and_transitions },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        const bool ok = tests[i].fn();
        printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (!ok) failed++;
    }
    return failed ? 1 : 0;
}
