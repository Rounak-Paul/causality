// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "motion.h"
#include "motion_lists.h"
#include "node.h"
#include "style.h"
#include "widget.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* ============================================================
   ANIMATABLE PROPERTIES
   ============================================================ */

enum { COMP_FLOAT, COMP_COLOR, COMP_TEXT };

typedef struct {
    uint8_t  kind;      /* COMP_* */
    uint16_t offset;    /* byte offset into Ca_NodeDesc (unused for COMP_TEXT) */
} MotionComp;

/* One animatable CSS property: the desc fields it drives and how it is named. */
typedef struct {
    int        prop;        /* Ca_CssPropId naming this entry */
    int        groups[2];   /* CA_MOTION_GROUP_* shorthands that also cover it */
    int        aux[5];      /* further properties whose declaration sets this entry */
    bool       layout;      /* changing it requires a relayout */
    bool       gradient;    /* only meaningful between two gradients of the same type */
    float      lo;          /* lowest value a float component may take */
    uint8_t    count;
    MotionComp comp[6];
} MotionProp;

#define F(field) { COMP_FLOAT, (uint16_t)offsetof(Ca_NodeDesc, field) }
#define C(field) { COMP_COLOR, (uint16_t)offsetof(Ca_NodeDesc, field) }
#define T()      { COMP_TEXT, 0 }
#define SIGNED   (-1e30f)
#define P(id)    CA_CSS_PROP_##id
#define G(id)    CA_MOTION_GROUP_##id

static const MotionProp k_props[] = {
    { P(BACKGROUND_COLOR), {G(BACKGROUND), 0}, { P(BACKGROUND) }, false, false, 0.0f, 1, { C(background) } },
    /* A gradient's end colour, angle and centre; its first colour is the background colour above. */
    { P(BACKGROUND),       {G(BACKGROUND), 0},
      { P(GRADIENT_COLOR2), P(GRADIENT_ANGLE), P(GRADIENT_CX), P(GRADIENT_CY) },
      false, true, SIGNED, 4, { C(gradient_color2), F(gradient_angle), F(gradient_cx), F(gradient_cy) } },
    { P(COLOR),            {0, 0}, {0}, false, false, 0.0f, 1, { T() } },
    /* desc.opacity == 0 means "unset", so the floor keeps a faded-out node faded out. */
    { P(OPACITY),          {0, 0}, {0}, false, false, 0.001f, 1, { F(opacity) } },
    { P(WIDTH),            {0, 0}, {0}, true,  false, 0.0f, 1, { F(width) } },
    { P(HEIGHT),           {0, 0}, {0}, true,  false, 0.0f, 1, { F(height) } },
    { P(MIN_WIDTH),        {0, 0}, {0}, true,  false, 0.0f, 1, { F(min_w) } },
    { P(MAX_WIDTH),        {0, 0}, {0}, true,  false, 0.0f, 1, { F(max_w) } },
    { P(MIN_HEIGHT),       {0, 0}, {0}, true,  false, 0.0f, 1, { F(min_h) } },
    { P(MAX_HEIGHT),       {0, 0}, {0}, true,  false, 0.0f, 1, { F(max_h) } },
    { P(LEFT),             {0, 0}, {0}, true,  false, SIGNED, 1, { F(pos_x) } },
    { P(RIGHT),            {0, 0}, {0}, true,  false, SIGNED, 1, { F(pos_right) } },
    { P(TOP),              {0, 0}, {0}, true,  false, SIGNED, 1, { F(pos_y) } },
    { P(BOTTOM),           {0, 0}, {0}, true,  false, SIGNED, 1, { F(pos_bottom) } },
    { P(PADDING_TOP),      {G(PADDING), 0}, {0}, true, false, 0.0f, 1, { F(padding_top) } },
    { P(PADDING_RIGHT),    {G(PADDING), 0}, {0}, true, false, 0.0f, 1, { F(padding_right) } },
    { P(PADDING_BOTTOM),   {G(PADDING), 0}, {0}, true, false, 0.0f, 1, { F(padding_bottom) } },
    { P(PADDING_LEFT),     {G(PADDING), 0}, {0}, true, false, 0.0f, 1, { F(padding_left) } },
    { P(MARGIN_TOP),       {G(MARGIN), 0}, {0}, true, false, SIGNED, 1, { F(margin_top) } },
    { P(MARGIN_RIGHT),     {G(MARGIN), 0}, {0}, true, false, SIGNED, 1, { F(margin_right) } },
    { P(MARGIN_BOTTOM),    {G(MARGIN), 0}, {0}, true, false, SIGNED, 1, { F(margin_bottom) } },
    { P(MARGIN_LEFT),      {G(MARGIN), 0}, {0}, true, false, SIGNED, 1, { F(margin_left) } },
    { P(GAP),              {0, 0}, {0}, true,  false, 0.0f, 1, { F(gap) } },
    { P(ROW_GAP),          {0, 0}, {0}, true,  false, 0.0f, 1, { F(row_gap) } },
    { P(COLUMN_GAP),       {0, 0}, {0}, true,  false, 0.0f, 1, { F(column_gap) } },
    { P(FONT_SIZE),        {0, 0}, {0}, true,  false, 0.0f, 1, { F(font_size) } },
    { P(LETTER_SPACING),   {0, 0}, {0}, true,  false, SIGNED, 1, { F(letter_spacing) } },
    { P(BORDER_RADIUS),    {0, 0}, {0}, false, false, 0.0f, 1, { F(corner_radius) } },
    { P(BORDER_COLOR),     {G(BORDER), G(BORDER_COLOR)},
      { P(BORDER_TOP_COLOR), P(BORDER_RIGHT_COLOR), P(BORDER_BOTTOM_COLOR), P(BORDER_LEFT_COLOR) },
      false, false, 0.0f, 5, { C(border_color), C(border_top_c), C(border_right_c), C(border_bottom_c), C(border_left_c) } },
    { P(BORDER_WIDTH),     {G(BORDER), G(BORDER_WIDTH)},
      { P(BORDER_TOP_WIDTH), P(BORDER_RIGHT_WIDTH), P(BORDER_BOTTOM_WIDTH), P(BORDER_LEFT_WIDTH) },
      true, false, 0.0f, 5, { F(border_width), F(border_top_w), F(border_right_w), F(border_bottom_w), F(border_left_w) } },
    { P(BOX_SHADOW),       {0, 0},
      { P(SHADOW_OFFSET_X), P(SHADOW_OFFSET_Y), P(SHADOW_BLUR), P(SHADOW_COLOR) },
      false, false, SIGNED, 4, { F(shadow_offset_x), F(shadow_offset_y), F(shadow_blur), C(shadow_color) } },
    { P(GLOW_RADIUS),      {0, 0}, {0}, false, false, 0.0f, 1, { F(glow_radius) } },
    { P(GLOW_COLOR),       {0, 0}, {0}, false, false, 0.0f, 1, { C(glow_color) } },
    { P(BACKDROP_FILTER),  {0, 0}, {0}, false, false, 0.0f, 1, { F(backdrop_blur) } },
};

#undef F
#undef C
#undef T
#undef P
#undef G

#define PROP_COUNT (sizeof(k_props) / sizeof(k_props[0]))

static int comp_floats(uint8_t kind)
{
    return kind == COMP_FLOAT ? 1 : 4;
}

/** Total floats an entry carries. */
static int entry_floats(const MotionProp *e)
{
    int n = 0;
    for (int i = 0; i < e->count; ++i) n += comp_floats(e->comp[i].kind);
    return n;
}

static bool entry_has_text(const MotionProp *e)
{
    for (int i = 0; i < e->count; ++i)
        if (e->comp[i].kind == COMP_TEXT) return true;
    return false;
}

static void unpack_rgba(uint32_t color, float out[4])
{
    out[0] = (float)((color >> 24) & 0xFFu);
    out[1] = (float)((color >> 16) & 0xFFu);
    out[2] = (float)((color >> 8) & 0xFFu);
    out[3] = (float)(color & 0xFFu);
}

static uint32_t pack_rgba(const float in[4])
{
    uint32_t c[4];
    for (int i = 0; i < 4; ++i) {
        const float v = fminf(fmaxf(in[i], 0.0f), 255.0f);
        c[i] = (uint32_t)(v + 0.5f);
    }
    return (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
}

/**
 * Reads an entry's values from a descriptor.
 *
 * @param e    Property entry.
 * @param desc Descriptor to read.
 * @param text Text colour, for COMP_TEXT components.
 * @param out  Receives the entry's floats in component order.
 */
static void read_values(const MotionProp *e, const Ca_NodeDesc *desc, uint32_t text,
                        float out[CA_ANIM_MAXF])
{
    int n = 0;
    for (int i = 0; i < e->count; ++i) {
        const MotionComp *c = &e->comp[i];
        if (c->kind == COMP_FLOAT) {
            out[n++] = *(const float *)((const char *)desc + c->offset);
        } else if (c->kind == COMP_COLOR) {
            unpack_rgba(*(const uint32_t *)((const char *)desc + c->offset), out + n);
            n += 4;
        } else {
            unpack_rgba(text, out + n);
            n += 4;
        }
    }
}

/** Writes an entry's desc-backed components; text components are left to the caller. */
static void write_desc(Ca_NodeDesc *desc, const MotionProp *e, const float *v)
{
    int n = 0;
    for (int i = 0; i < e->count; ++i) {
        const MotionComp *c = &e->comp[i];
        if (c->kind == COMP_FLOAT)
            *(float *)((char *)desc + c->offset) = fmaxf(v[n], e->lo);
        else if (c->kind == COMP_COLOR)
            *(uint32_t *)((char *)desc + c->offset) = pack_rgba(v + n);
        n += comp_floats(c->kind);
    }
}

/** Writes an entry's values into a node: desc fields, plus the widget's text colour. */
static void write_values(Ca_Node *node, const MotionProp *e, const float *v)
{
    write_desc(&node->desc, e, v);
    int n = 0;
    for (int i = 0; i < e->count; ++i) {
        if (e->comp[i].kind == COMP_TEXT) ca_widget_set_text_color(node, pack_rgba(v + n));
        n += comp_floats(e->comp[i].kind);
    }
}

static bool values_equal(const float *a, const float *b, int n)
{
    for (int i = 0; i < n; ++i)
        if (fabsf(a[i] - b[i]) > 1e-4f) return false;
    return true;
}

/** The text colour a node shows: its own, or the palette default when it has none. */
static uint32_t effective_text(const Ca_Node *node, uint32_t raw)
{
    return raw ? raw : ca_widget_default_text_color(node);
}

/** Whether a transition-property value (property id or group) covers this entry. */
static bool spec_covers(int spec_prop, const MotionProp *e)
{
    if (spec_prop == 0) return false;
    return spec_prop == CA_MOTION_ALL || spec_prop == e->prop ||
           spec_prop == e->groups[0] || spec_prop == e->groups[1];
}

/** Whether a resolved style carries a declaration for any property this entry is made of. */
static bool entry_set_in(const MotionProp *e, const Ca_ResolvedStyle *rs)
{
    if (ca_style_prop_is_set(rs, (Ca_CssPropId)e->prop)) return true;
    for (int i = 0; i < 5 && e->aux[i]; ++i)
        if (ca_style_prop_is_set(rs, (Ca_CssPropId)e->aux[i])) return true;
    return false;
}

/* ============================================================
   EASING
   ============================================================ */

/* One axis of a cubic bezier with end points (0,0) and (1,1). */
static float bezier_axis(float a, float b, float s)
{
    const float inv = 1.0f - s;
    return 3.0f * inv * inv * s * a + 3.0f * inv * s * s * b + s * s * s;
}

float ca_easing_eval(const Ca_EasingDef *easing, float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (!easing || easing->kind != CA_EASING_BEZIER) return t;
    /* x is monotonic for x1, x2 in [0, 1]: bisect for the parameter at x = t. */
    float lo = 0.0f, hi = 1.0f, s = t;
    for (int i = 0; i < 24; ++i) {
        if (bezier_axis(easing->bezier[0], easing->bezier[2], s) < t) lo = s; else hi = s;
        s = 0.5f * (lo + hi);
    }
    return bezier_axis(easing->bezier[1], easing->bezier[3], s);
}

static Ca_EasingDef ease_default(void)
{
    return (Ca_EasingDef){ CA_EASING_BEZIER, { 0.25f, 0.10f, 0.25f, 1.00f } };
}

/* ============================================================
   STORING THE SPECS
   ============================================================ */

/** Item `i` of a pool list repeated cyclically; `fallback` when the list is empty. */
#define CYCLE(type, items, count, i, fallback) \
    ((count) ? ((const type *)(items))[(i) % (count)] : (fallback))

void ca_motion_store(Ca_Node *node, const Ca_ResolvedStyle *rs)
{
    const Ca_MotionLists *m = &rs->motion;
    if (node->motion_applied_valid && memcmp(&node->motion_applied, m, sizeof(*m)) == 0) return;
    node->motion_applied = *m;
    node->motion_applied_valid = true;

    uint32_t n_prop = 0, n_dur = 0, n_delay = 0, n_timing = 0;
    const int *props = ca_motion_list_items(m->transition_property, &n_prop);
    const float *durations = ca_motion_list_items(m->transition_duration, &n_dur);
    const float *delays = ca_motion_list_items(m->transition_delay, &n_delay);
    const Ca_EasingDef *timings = ca_motion_list_items(m->transition_timing, &n_timing);

    CA_FREE(node->transition_specs);
    node->transition_specs = n_prop ? CA_CALLOC(n_prop, sizeof(Ca_TransitionSpec)) : NULL;
    node->transition_spec_count = node->transition_specs ? n_prop : 0;
    for (uint32_t i = 0; i < node->transition_spec_count; ++i) {
        node->transition_specs[i] = (Ca_TransitionSpec){
            .prop     = props[i],
            .duration = CYCLE(float, durations, n_dur, i, 0.0f),
            .delay    = CYCLE(float, delays, n_delay, i, 0.0f),
            .easing   = CYCLE(Ca_EasingDef, timings, n_timing, i, ease_default()),
        };
    }
    node->transition_stretch = m->stretch;

    uint32_t n_name = 0, n_adur = 0, n_adelay = 0, n_atiming = 0, n_iter = 0, n_dir = 0, n_fill = 0, n_play = 0;
    const uint32_t *names = ca_motion_list_items(m->anim_name, &n_name);
    const float *adur = ca_motion_list_items(m->anim_duration, &n_adur);
    const float *adelay = ca_motion_list_items(m->anim_delay, &n_adelay);
    const Ca_EasingDef *atiming = ca_motion_list_items(m->anim_timing, &n_atiming);
    const float *iters = ca_motion_list_items(m->anim_iterations, &n_iter);
    const uint8_t *dirs = ca_motion_list_items(m->anim_direction, &n_dir);
    const uint8_t *fills = ca_motion_list_items(m->anim_fill, &n_fill);
    const uint8_t *plays = ca_motion_list_items(m->anim_play_state, &n_play);

    CA_FREE(node->anim_specs);
    node->anim_specs = n_name ? CA_CALLOC(n_name, sizeof(Ca_AnimSpec)) : NULL;
    node->anim_spec_count = 0;
    for (uint32_t i = 0; node->anim_specs && i < n_name; ++i) {
        if (names[i] == 0) continue;
        node->anim_specs[node->anim_spec_count++] = (Ca_AnimSpec){
            .name       = names[i],
            .duration   = CYCLE(float, adur, n_adur, i, 0.0f),
            .delay      = CYCLE(float, adelay, n_adelay, i, 0.0f),
            .iterations = CYCLE(float, iters, n_iter, i, 1.0f),
            .direction  = CYCLE(uint8_t, dirs, n_dir, i, (uint8_t)CA_ANIM_NORMAL),
            .fill       = CYCLE(uint8_t, fills, n_fill, i, (uint8_t)CA_ANIM_FILL_NONE),
            .paused     = CYCLE(uint8_t, plays, n_play, i, (uint8_t)0) != 0,
            .easing     = CYCLE(Ca_EasingDef, atiming, n_atiming, i, ease_default()),
        };
    }
}

const Ca_TransitionSpec *ca_motion_find_spec(const Ca_Node *node, int prop)
{
    for (int i = (int)node->transition_spec_count - 1; i >= 0; --i) {
        const int p = node->transition_specs[i].prop;
        if (p == CA_MOTION_ALL || (p != 0 && p == prop)) return &node->transition_specs[i];
    }
    return NULL;
}

static const Ca_TransitionSpec *spec_for_entry(const Ca_Node *node, const MotionProp *e)
{
    for (int i = (int)node->transition_spec_count - 1; i >= 0; --i)
        if (spec_covers(node->transition_specs[i].prop, e)) return &node->transition_specs[i];
    return NULL;
}

/* ============================================================
   TRANSITIONS
   ============================================================ */

/** The node's active transition for `entry`, or NULL. */
static Ca_Transition *transition_running(Ca_Node *node, uint8_t entry)
{
    for (size_t i = 0; i < node->transition_storage.count; ++i) {
        Ca_Transition *t = ca_dyn_array_at(&node->transition_storage, i);
        if (t->active && t->entry == entry) return t;
    }
    return NULL;
}

/** The node's transition slot for `entry`: the running one, else a free one, else a new one. */
static Ca_Transition *transition_acquire(Ca_Node *node, uint8_t entry)
{
    if (node->transition_storage.element_size == 0 &&
        !ca_dyn_array_init(&node->transition_storage, sizeof(Ca_Transition)))
        return NULL;
    Ca_Transition *free_slot = NULL;
    for (size_t i = 0; i < node->transition_storage.count; ++i) {
        Ca_Transition *t = ca_dyn_array_at(&node->transition_storage, i);
        if (t->active && t->entry == entry) return t;
        if (!t->active && !free_slot) free_slot = t;
    }
    if (free_slot) return free_slot;
    const size_t index = node->transition_storage.count;
    if (!ca_dyn_array_resize(&node->transition_storage, index + 1u)) return NULL;
    node->transitions = node->transition_storage.data;
    return ca_dyn_array_at(&node->transition_storage, index);
}

void ca_motion_begin_transitions(Ca_Node *node, const Ca_NodeDesc *old,
                                 uint32_t old_text, uint32_t new_text)
{
    if (node->transition_spec_count == 0) return;
    const uint32_t from_text = effective_text(node, old_text);
    const uint32_t to_text = effective_text(node, new_text);
    for (size_t e = 0; e < PROP_COUNT; ++e) {
        const MotionProp *entry = &k_props[e];
        if (entry->gradient && old->gradient_type != node->desc.gradient_type) continue;
        float from[CA_ANIM_MAXF], to[CA_ANIM_MAXF];
        read_values(entry, old, from_text, from);
        read_values(entry, &node->desc, to_text, to);
        const int n = entry_floats(entry);
        Ca_Transition *running = transition_running(node, (uint8_t)e);
        if (values_equal(from, to, n)) {
            /* The style is back on the value already shown: a transition still
               heading for some other target must not carry on to it. */
            if (running) running->active = false;
            continue;
        }
        /* Already heading for exactly this target: let it finish rather than restart. */
        if (running && values_equal(running->to, to, n)) continue;
        const Ca_TransitionSpec *spec = spec_for_entry(node, entry);
        if (!spec || (spec->duration <= 0.0f && spec->delay <= 0.0f)) continue;
        Ca_Transition *t = transition_acquire(node, (uint8_t)e);
        if (!t) continue;

        memset(t, 0, sizeof(*t));
        t->entry      = (uint8_t)e;
        t->active     = true;
        t->start_time = glfwGetTime();
        t->duration   = spec->duration;
        t->delay      = spec->delay;
        t->easing     = spec->easing;
        t->text_end   = new_text;
        memcpy(t->from, from, sizeof(float) * (size_t)n);
        memcpy(t->to, to, sizeof(float) * (size_t)n);
        write_values(node, entry, from);

        /* A transition can begin during the input pass, after the transition
           tick already ran this frame; wake an idle event loop so the next
           tick advances it. */
        glfwPostEmptyEvent();
    }
}

void ca_motion_follow_layout(Ca_Node *node, const Ca_NodeDesc *prev_css,
                             const Ca_NodeDesc *now_css, const Ca_ResolvedStyle *now)
{
    for (size_t e = 0; e < PROP_COUNT; ++e) {
        const MotionProp *entry = &k_props[e];
        if (!entry->layout) continue;
        float before[CA_ANIM_MAXF], after[CA_ANIM_MAXF];
        read_values(entry, prev_css, 0u, before);
        read_values(entry, now_css, 0u, after);
        if (values_equal(before, after, entry_floats(entry))) continue;
        /* A property the new style no longer sets falls back to the node's own value. */
        if (!entry_set_in(entry, now)) read_values(entry, &node->base_desc, 0u, after);
        write_desc(&node->desc, entry, after);
    }
}

static bool tick_transitions(Ca_Node *node, double now, bool *layout_affected)
{
    bool busy = false;
    for (size_t i = 0; i < node->transition_storage.count; ++i) {
        Ca_Transition *t = &node->transitions[i];
        if (!t->active) continue;
        const MotionProp *entry = &k_props[t->entry];
        const float elapsed = (float)(now - t->start_time) - t->delay;
        const int n = entry_floats(entry);
        float v[CA_ANIM_MAXF];
        bool landed = false;
        if (elapsed < 0.0f) {
            memcpy(v, t->from, sizeof(float) * (size_t)n);
        } else {
            float progress = t->duration > 0.0f ? elapsed / t->duration : 1.0f;
            if (progress >= 1.0f) {
                progress = 1.0f;
                t->active = false;
                landed = true;
            }
            const float k = ca_easing_eval(&t->easing, progress);
            for (int j = 0; j < n; ++j) v[j] = t->from[j] + (t->to[j] - t->from[j]) * k;
        }
        write_values(node, entry, v);
        /* A text colour that settles on "no colour of its own" goes back to the palette default. */
        if (landed && entry_has_text(entry) && t->text_end == 0u) ca_widget_set_text_color(node, 0u);
        if (entry->layout) *layout_affected = true;
        /* The frame that lands on the final value counts as busy so it is painted. */
        busy = true;
    }
    return busy;
}

/* ============================================================
   ANIMATIONS
   ============================================================ */

/* One keyframed value of a track. */
typedef struct {
    float        offset;
    bool         has_ease;
    Ca_EasingDef ease;
    float        value[CA_ANIM_MAXF];
} Stop;

/* One animated property of a running animation: its keyframed values over 0..1. */
typedef struct {
    uint8_t  entry;
    uint32_t stop_count;
    Stop    *stops;
    float    base[CA_ANIM_MAXF];     /* the node's value when the animation began */
    uint32_t base_text_raw;          /* its own text colour then (0 = palette default) */
} Track;

struct Ca_NodeAnim {
    Ca_AnimSpec spec;
    double      start;
    double      paused_at;     /* when the current pause began; meaningful while `paused` */
    uint64_t    memory_key;    /* identity of the node's place in the tree, see "continuity" */
    bool        missing;       /* no @keyframes of that name */
    bool        done;          /* finite animation that has finished */
    bool        applied;       /* tracks have written to the node (and need restoring) */
    bool        paused;
    uint32_t    track_count;
    Track      *tracks;
};

typedef struct {
    Ca_ResolvedStyle rs;
    Ca_NodeDesc      desc;
    uint32_t         text;
} KeyframeState;

static void anim_free(Ca_NodeAnim *a)
{
    for (uint32_t t = 0; t < a->track_count; ++t) CA_FREE(a->tracks[t].stops);
    CA_FREE(a->tracks);
    a->tracks = NULL;
    a->track_count = 0;
}

/** The stylesheet holding @keyframes `name`, searched author first, then system. */
static const Ca_Stylesheet *keyframes_owner(const Ca_Node *node, uint32_t name,
                                            const Ca_CssKeyframes **found)
{
    const Ca_Instance *instance = node->window ? node->window->instance : NULL;
    *found = NULL;
    if (!instance) return NULL;
    const Ca_Stylesheet *sheets[2] = { instance->stylesheet, instance->system_stylesheet };
    for (int i = 0; i < 2; ++i) {
        *found = ca_css_find_keyframes(sheets[i], name);
        if (*found) return sheets[i];
    }
    return NULL;
}

/** Compiles a named @keyframes block into per-property tracks for one node. */
static void anim_compile(Ca_Node *node, Ca_NodeAnim *a)
{
    const Ca_CssKeyframes *kf;
    const Ca_Stylesheet *owner = keyframes_owner(node, a->spec.name, &kf);
    if (!kf || kf->frame_count == 0) {
        a->missing = true;
        return;
    }
    const uint32_t frames = (uint32_t)kf->frame_count;
    KeyframeState *states = CA_CALLOC(frames, sizeof(KeyframeState));
    Track *tracks = CA_CALLOC(PROP_COUNT, sizeof(Track));
    if (!states || !tracks) {
        CA_FREE(states);
        CA_FREE(tracks);
        a->missing = true;
        return;
    }
    for (uint32_t f = 0; f < frames; ++f)
        ca_widget_resolve_keyframe(node, owner, kf->frames[f].decls, kf->frames[f].decl_count,
                                   &states[f].rs, &states[f].desc, &states[f].text);

    const uint32_t text_raw = ca_widget_text_color(node);
    const uint32_t text_now = effective_text(node, text_raw);
    uint32_t track_count = 0;
    for (size_t e = 0; e < PROP_COUNT; ++e) {
        const MotionProp *entry = &k_props[e];
        Stop *stops = CA_CALLOC(frames + 2u, sizeof(Stop));
        if (!stops) continue;
        uint32_t count = 0;
        float base[CA_ANIM_MAXF];
        read_values(entry, &node->desc, text_now, base);
        const int n = entry_floats(entry);

        for (uint32_t f = 0; f < frames; ++f) {
            if (!entry_set_in(entry, &states[f].rs)) continue;
            /* A gradient can only animate toward another gradient of the same type. */
            if (entry->gradient && states[f].desc.gradient_type != node->desc.gradient_type) continue;
            Stop *s = &stops[count++];
            s->offset = kf->frames[f].offset;
            read_values(entry, &states[f].desc, effective_text(node, states[f].text), s->value);
            if (entry->prop == CA_CSS_PROP_OPACITY) s->value[0] = states[f].rs.opacity;
            s->has_ease = kf->frames[f].has_timing;
            s->ease = (Ca_EasingDef){ kf->frames[f].timing_kind, {
                kf->frames[f].timing_bezier[0], kf->frames[f].timing_bezier[1],
                kf->frames[f].timing_bezier[2], kf->frames[f].timing_bezier[3] } };
        }
        if (count == 0) {
            CA_FREE(stops);
            continue;
        }

        /* A property missing from the 0% or 100% stop animates from / to the node's own value. */
        if (stops[0].offset > 0.0f) {
            memmove(&stops[1], &stops[0], sizeof(Stop) * count);
            memset(&stops[0], 0, sizeof(Stop));
            memcpy(stops[0].value, base, sizeof(float) * (size_t)n);
            count++;
        }
        if (stops[count - 1].offset < 1.0f) {
            stops[count].offset = 1.0f;
            memcpy(stops[count].value, base, sizeof(float) * (size_t)n);
            count++;
        }
        Track *tr = &tracks[track_count++];
        tr->entry = (uint8_t)e;
        tr->stops = stops;
        tr->stop_count = count;
        memcpy(tr->base, base, sizeof(base));
        tr->base_text_raw = text_raw;
    }
    CA_FREE(states);
    if (track_count == 0) {
        CA_FREE(tracks);
        return;
    }
    a->tracks = tracks;
    a->track_count = track_count;
}

/** Interpolated values of one track at animation progress p in [0, 1]. */
static void track_eval(const Track *tr, float p, const Ca_EasingDef *fallback, float out[CA_ANIM_MAXF])
{
    uint32_t i = 0;
    while (i + 2 < tr->stop_count && p > tr->stops[i + 1].offset) ++i;
    const float span = tr->stops[i + 1].offset - tr->stops[i].offset;
    float t = span > 1e-6f ? (p - tr->stops[i].offset) / span : 1.0f;
    t = fminf(fmaxf(t, 0.0f), 1.0f);
    const float k = ca_easing_eval(tr->stops[i].has_ease ? &tr->stops[i].ease : fallback, t);
    const int n = entry_floats(&k_props[tr->entry]);
    for (int j = 0; j < n; ++j)
        out[j] = tr->stops[i].value[j] + (tr->stops[i + 1].value[j] - tr->stops[i].value[j]) * k;
}

static void anim_apply(Ca_Node *node, Ca_NodeAnim *a, float p, bool *layout_affected)
{
    for (uint32_t t = 0; t < a->track_count; ++t) {
        float v[CA_ANIM_MAXF];
        track_eval(&a->tracks[t], p, &a->spec.easing, v);
        write_values(node, &k_props[a->tracks[t].entry], v);
        if (k_props[a->tracks[t].entry].layout) *layout_affected = true;
    }
    a->applied = true;
}

static void anim_restore(Ca_Node *node, Ca_NodeAnim *a, bool *layout_affected)
{
    if (!a->applied) return;
    for (uint32_t t = 0; t < a->track_count; ++t) {
        const MotionProp *entry = &k_props[a->tracks[t].entry];
        write_values(node, entry, a->tracks[t].base);
        if (entry_has_text(entry) && a->tracks[t].base_text_raw == 0u) ca_widget_set_text_color(node, 0u);
        if (entry->layout) *layout_affected = true;
    }
    a->applied = false;
}

/** Progress within the current iteration with animation-direction applied. */
static float directed_progress(uint8_t direction, long iteration, float p)
{
    const bool odd = (iteration & 1L) != 0;
    const bool reversed = direction == CA_ANIM_REVERSE ||
                          (direction == CA_ANIM_ALTERNATE && odd) ||
                          (direction == CA_ANIM_ALTERNATE_REVERSE && !odd);
    return reversed ? 1.0f - p : p;
}

/** Progress at which a finished animation holds: the end of its last iteration. */
static float final_progress(const Ca_AnimSpec *spec)
{
    const float whole = floorf(spec->iterations);
    const float frac = spec->iterations - whole;
    const long last = frac > 0.0f ? (long)whole : (long)whole - 1;
    return directed_progress(spec->direction, last < 0 ? 0 : last, frac > 0.0f ? frac : 1.0f);
}

/* ---- continuity ----------------------------------------------------------
   Builders rebuild whole subtrees, so a node running an animation can be
   destroyed and recreated in the same place every frame. Each running
   animation therefore records its progress against the node's position in
   the tree (ids, child indices, element types and classes along its
   ancestry); a recreated node at the same position resumes it instead of
   restarting. A record nobody refreshes for a while is dropped, so an
   animation that really went away starts afresh when it comes back. */

#define MEMORY_LIFETIME_SECONDS 1.0

typedef struct {
    uint64_t key;
    uint32_t name;
    double   start;
    double   paused_at;
    double   seen;
    bool     done, paused;
} MemoryEntry;

static uint64_t mix(uint64_t hash, uint64_t value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash * 1099511628211ull;
}

static uint64_t hash_text(const char *text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
        hash ^= *c;
        hash *= 1099511628211ull;
    }
    return hash;
}

/** Identity of a node's place in the tree. */
static uint64_t node_place_key(const Ca_Node *node)
{
    uint64_t hash = 1469598103934665603ull;
    for (const Ca_Node *n = node; n; n = n->parent) {
        uint64_t index = 0;
        if (n->parent)
            for (uint32_t i = 0; i < n->parent->child_count; ++i)
                if (n->parent->children[i] == n) { index = i + 1u; break; }
        hash = mix(hash, n->id[0] ? hash_text(n->id) : index);
        hash = mix(hash, (uint64_t)n->elem_type);
        hash = mix(hash, hash_text(n->classes));
    }
    return hash;
}

static MemoryEntry *memory_find(Ca_Window *win, uint64_t key, uint32_t name)
{
    for (size_t i = 0; i < win->motion_memory.count; ++i) {
        MemoryEntry *m = ca_dyn_array_at(&win->motion_memory, i);
        if (m->key == key && m->name == name) return m;
    }
    return NULL;
}

static MemoryEntry *memory_acquire(Ca_Window *win, uint64_t key, uint32_t name)
{
    MemoryEntry *m = memory_find(win, key, name);
    if (m) return m;
    if (win->motion_memory.element_size == 0 &&
        !ca_dyn_array_init(&win->motion_memory, sizeof(MemoryEntry)))
        return NULL;
    const size_t index = win->motion_memory.count;
    if (!ca_dyn_array_resize(&win->motion_memory, index + 1u)) return NULL;
    m = ca_dyn_array_at(&win->motion_memory, index);
    *m = (MemoryEntry){ .key = key, .name = name };
    return m;
}

static void memory_forget(Ca_Window *win, uint64_t key, uint32_t name)
{
    for (size_t i = 0; i < win->motion_memory.count; ++i) {
        MemoryEntry *m = ca_dyn_array_at(&win->motion_memory, i);
        if (m->key != key || m->name != name) continue;
        *m = *(MemoryEntry *)ca_dyn_array_at(&win->motion_memory, win->motion_memory.count - 1);
        ca_dyn_array_resize(&win->motion_memory, win->motion_memory.count - 1);
        return;
    }
}

void ca_motion_window_tick(Ca_Window *win, double now)
{
    for (size_t i = win->motion_memory.count; i-- > 0;) {
        const MemoryEntry *m = ca_dyn_array_at(&win->motion_memory, i);
        if (now - m->seen <= MEMORY_LIFETIME_SECONDS) continue;
        *(MemoryEntry *)ca_dyn_array_at(&win->motion_memory, i) =
            *(MemoryEntry *)ca_dyn_array_at(&win->motion_memory, win->motion_memory.count - 1);
        ca_dyn_array_resize(&win->motion_memory, win->motion_memory.count - 1);
    }
}

/** Brings a node's running animations in line with its current `animation` list. */
static void anim_reconcile(Ca_Node *node, double now, bool *layout_affected)
{
    const uint32_t specs = node->anim_spec_count;
    bool *taken = specs ? CA_CALLOC(specs, sizeof(bool)) : NULL;
    if (specs && !taken) return;

    uint32_t kept = 0;
    for (uint32_t i = 0; i < node->anim_count; ++i) {
        Ca_NodeAnim *a = &node->anims[i];
        int match = -1;
        for (uint32_t s = 0; s < specs; ++s)
            if (!taken[s] && node->anim_specs[s].name == a->spec.name) { match = (int)s; break; }
        if (match < 0) {
            anim_restore(node, a, layout_affected);
            if (node->window) memory_forget(node->window, a->memory_key, a->spec.name);
            anim_free(a);
            continue;
        }
        taken[match] = true;
        a->spec = node->anim_specs[match];
        if (kept != i) node->anims[kept] = *a;
        ++kept;
    }
    node->anim_count = kept;

    uint32_t pending = 0;
    for (uint32_t s = 0; s < specs; ++s) pending += taken[s] ? 0u : 1u;
    if (pending) {
        Ca_NodeAnim *grown = CA_CALLOC(kept + pending, sizeof(Ca_NodeAnim));
        if (grown) {
            if (kept) memcpy(grown, node->anims, sizeof(Ca_NodeAnim) * kept);
            CA_FREE(node->anims);
            node->anims = grown;
            for (uint32_t s = 0; s < specs; ++s) {
                if (taken[s]) continue;
                Ca_NodeAnim *a = &node->anims[node->anim_count++];
                a->spec = node->anim_specs[s];
                a->start = now;
                a->memory_key = node_place_key(node);
                const MemoryEntry *remembered = node->window ? memory_find(node->window, a->memory_key, a->spec.name) : NULL;
                if (remembered) {
                    a->start = remembered->start;
                    a->done = remembered->done;
                    a->paused = remembered->paused;
                    a->paused_at = remembered->paused_at;
                }
                anim_compile(node, a);
            }
        }
    }
    CA_FREE(taken);
}

static bool tick_animations(Ca_Node *node, double now, bool *layout_affected)
{
    if (node->anim_spec_count == 0 && node->anim_count == 0) return false;
    anim_reconcile(node, now, layout_affected);
    bool busy = false;
    for (uint32_t i = 0; i < node->anim_count; ++i) {
        Ca_NodeAnim *a = &node->anims[i];
        const Ca_AnimSpec *spec = &a->spec;

        /* animation-play-state: a pause freezes progress; resuming shifts the start by the pause. */
        double clock = now;
        if (spec->paused) {
            if (!a->paused) { a->paused = true; a->paused_at = now; }
            clock = a->paused_at;
        } else if (a->paused) {
            a->start += now - a->paused_at;
            a->paused = false;
        }
        if (node->window) {
            MemoryEntry *m = memory_acquire(node->window, a->memory_key, spec->name);
            if (m) {
                m->start = a->start;
                m->paused_at = a->paused_at;
                m->seen = now;
                m->done = a->done;
                m->paused = a->paused;
            }
        }
        if (a->missing || a->track_count == 0) continue;

        const bool holds_end = spec->fill == CA_ANIM_FILL_FORWARDS || spec->fill == CA_ANIM_FILL_BOTH;
        const bool holds_start = spec->fill == CA_ANIM_FILL_BACKWARDS || spec->fill == CA_ANIM_FILL_BOTH;
        const float elapsed = (float)(clock - a->start) - spec->delay;

        if (elapsed < 0.0f) {
            if (holds_start) anim_apply(node, a, directed_progress(spec->direction, 0, 0.0f), layout_affected);
            if (!a->paused) busy = true;
            continue;
        }
        const float duration = spec->duration > 0.0f ? spec->duration : 0.0f;
        const float cycles = duration > 0.0f ? elapsed / duration : INFINITY;
        if (isfinite(spec->iterations) && cycles >= spec->iterations) {
            /* A held end state is rewritten every tick: a rebuilt node's desc resets to its CSS value. */
            if (holds_end) anim_apply(node, a, final_progress(spec), layout_affected);
            if (a->done) continue;
            a->done = true;
            if (!holds_end) anim_restore(node, a, layout_affected);
            busy = true;   /* repaint the frame it lands on */
            continue;
        }
        const float whole = floorf(cycles);
        anim_apply(node, a, directed_progress(spec->direction, (long)whole, cycles - whole), layout_affected);
        if (!a->paused) busy = true;
    }
    return busy;
}

/* ============================================================
   FRAME TICK
   ============================================================ */

bool ca_motion_tick(Ca_Node *node, double now, bool *layout_affected)
{
    bool busy = tick_transitions(node, now, layout_affected);
    if (tick_animations(node, now, layout_affected)) busy = true;
    return busy;
}

void ca_motion_node_destroy(Ca_Node *node)
{
    for (uint32_t i = 0; i < node->anim_count; ++i) anim_free(&node->anims[i]);
    CA_FREE(node->anims);
    CA_FREE(node->anim_specs);
    CA_FREE(node->transition_specs);
    node->anims = NULL;
    node->anim_specs = NULL;
    node->transition_specs = NULL;
    node->anim_count = node->anim_spec_count = node->transition_spec_count = 0;
    node->motion_applied_valid = false;
}
