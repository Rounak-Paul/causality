// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* inline_layout.c — inline formatting context (Ca_DivDesc.inline_flow).

   Generalizes the existing single-string word-wrap algorithm (see
   layout.c's measure_wrapped_text_height / paint.c's paint_text_wrapped)
   to span MULTIPLE sibling child nodes on shared lines, so mixed content
   like plain text + a bold child + a link child wraps together the way
   a real inline formatting context does, instead of each child getting
   its own independent box. */

#include "inline_layout.h"
#include "font.h"

#include <stdlib.h>
#include <string.h>

static float node_ui_scale(const Ca_Node *node)
{
    return (node && node->window && node->window->ui_scale > 0.0f)
        ? node->window->ui_scale
        : 1.0f;
}

/* Resolved font metrics for one child's text runs, cached per-child for
   the duration of one ca_inline_layout call so repeated glyph lookups
   inside the word-splitting loop don't re-resolve the tier every time. */
typedef struct RunFont {
    Ca_FontTier *tier;
    float        cs;
    float        desired_size;
    float        metric_scale;   /* for ascent/descent/line_height       */
    float        line_height;
    float        ascent, descent; /* in the SAME scaled space as line_height */
} RunFont;

static bool resolve_run_font(Ca_Node *child, RunFont *out)
{
    Ca_Window *win = child->window;
    if (!win || !win->instance || !win->instance->font) return false;
    Ca_Font *font = win->instance->font;

    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    out->cs = font->content_scale / ui_s;
    out->desired_size = child->desc.font_size > 0.0f
        ? child->desc.font_size : font->default_size;
    out->tier = ca_font_select_tier_for_size(font, out->desired_size * ui_s,
                                             child->desc.font_bold);
    if (!out->tier) return false;

    float font_scale = out->desired_size / out->tier->logical_px;
    out->metric_scale = font_scale * ui_s;
    out->line_height = (out->tier->ascent - out->tier->descent + out->tier->line_gap)
        * out->metric_scale;
    if (out->line_height < 1.0f) out->line_height = out->desired_size * ui_s * 1.3f;
    out->ascent  = out->tier->ascent  * out->metric_scale;
    out->descent = out->tier->descent * out->metric_scale;
    return true;
}

static float word_width(const RunFont *rf, const char *start, uint32_t len)
{
    float w = 0.0f;
    const char *p = start;
    const char *end = start + len;
    while (p < end) {
        uint32_t cp = ca_utf8_decode(&p);
        Ca_FontTier *glyph_tier = rf->tier;
        Ca_Glyph *g = ca_font_glyph_from_tier(rf->tier, cp, &glyph_tier);
        if (!g) continue;
        float cs_eff = ca_font_glyph_cs_eff(glyph_tier, rf->desired_size, rf->cs);
        w += g->xadvance / cs_eff;
    }
    return w;
}

static bool ensure_run_capacity(Ca_InlineLayout *layout, uint32_t needed)
{
    if (needed <= layout->run_capacity) return true;
    uint32_t new_cap = layout->run_capacity ? layout->run_capacity * 2 : 16;
    if (new_cap < needed) new_cap = needed;
    Ca_InlineRun *grown = realloc(layout->runs, new_cap * sizeof(Ca_InlineRun));
    if (!grown) return false;
    layout->runs = grown;
    layout->run_capacity = new_cap;
    return true;
}

/* Appends one resolved run, growing the array as needed. Returns false
   (silently dropping the run) only on allocation failure — matches the
   rest of the layout engine's fail-soft-under-memory-pressure posture
   (e.g. layout_scratch_init's capacity check in layout.c). */
static bool push_run(Ca_InlineLayout *layout, Ca_Node *child,
                     const char *text_start, uint32_t text_len,
                     float x, float w, uint32_t line_index)
{
    if (!ensure_run_capacity(layout, layout->run_count + 1)) return false;
    Ca_InlineRun *run = &layout->runs[layout->run_count++];
    run->child      = child;
    run->text_start = text_start;
    run->text_len   = text_len;
    run->x          = x;
    run->w          = w;
    run->line_index = line_index;
    /* y/h/baseline_y filled in by the caller once the line's height
       (tallest run on that line) is known — see the finalize pass below. */
    run->y = run->h = run->baseline_y = 0.0f;
    return true;
}

/* One line's worth of runs, tracked while packing so baseline alignment
   (tallest run's ascent wins) can be resolved once the line is closed. */
typedef struct PendingLine {
    uint32_t first_run;   /* index into layout->runs where this line starts */
    float    max_ascent;
    float    max_descent; /* stored as a positive magnitude */
    float    max_line_height;
} PendingLine;

static void finalize_line(Ca_InlineLayout *layout, const PendingLine *pl,
                          float line_top_y)
{
    float line_h = pl->max_ascent + pl->max_descent;
    if (line_h < pl->max_line_height) line_h = pl->max_line_height;
    float baseline = line_top_y + pl->max_ascent;

    for (uint32_t i = pl->first_run; i < layout->run_count; ++i) {
        Ca_InlineRun *run = &layout->runs[i];
        run->y = line_top_y;
        run->h = line_h;
        run->baseline_y = baseline;
    }
}

void ca_inline_layout(Ca_Node *node, float content_w)
{
    if (!node) return;
    if (content_w < 1.0f) content_w = 0.0f;

    if (!node->inline_layout) {
        node->inline_layout = calloc(1, sizeof(Ca_InlineLayout));
        if (!node->inline_layout) return;
    }
    Ca_InlineLayout *layout = node->inline_layout;
    layout->run_count = 0; /* keep runs/run_capacity — reuse allocation */

    PendingLine pl = { .first_run = 0, .max_ascent = 0, .max_descent = 0,
                       .max_line_height = 0 };
    float cursor_x = 0.0f;
    float line_top_y = 0.0f;
    uint32_t line_index = 0;
    bool line_has_content = false;

    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *child = node->children[i];
        if (!child || child->desc.hidden || child->desc.position != CA_POSITION_RELATIVE)
            continue;

        if (child->widget_type == CA_WIDGET_LABEL && child->widget) {
            Ca_Label *lbl = (Ca_Label *)child->widget;
            if (!lbl->in_use) continue;
            const char *txt = ca_label_get_text(lbl);
            if (!txt || txt[0] == '\0') continue;

            RunFont rf;
            if (!resolve_run_font(child, &rf)) continue;
            float space_w = word_width(&rf, " ", 1);

            const char *p = txt;
            while (*p) {
                const char *word_start = p;
                while (*p && *p != ' ' && *p != '\n') {
                    const char *before = p;
                    ca_utf8_decode(&p);
                    (void)before;
                }
                uint32_t word_len = (uint32_t)(p - word_start);
                float w = word_len ? word_width(&rf, word_start, word_len) : 0.0f;

                if (word_len > 0) {
                    float needed = (line_has_content ? space_w : 0.0f) + w;
                    if (line_has_content && cursor_x + needed > content_w) {
                        finalize_line(layout, &pl, line_top_y);
                        line_top_y += (pl.max_ascent + pl.max_descent > pl.max_line_height
                            ? pl.max_ascent + pl.max_descent : pl.max_line_height);
                        line_index++;
                        cursor_x = 0.0f;
                        line_has_content = false;
                        pl = (PendingLine){ .first_run = layout->run_count,
                                            .max_ascent = 0, .max_descent = 0,
                                            .max_line_height = 0 };
                        needed = w;
                    }
                    if (line_has_content) cursor_x += space_w;

                    if (!push_run(layout, child, word_start, word_len,
                                  cursor_x, w, line_index))
                        goto done;
                    cursor_x += w;
                    line_has_content = true;
                    if (rf.ascent > pl.max_ascent) pl.max_ascent = rf.ascent;
                    if (-rf.descent > pl.max_descent) pl.max_descent = -rf.descent;
                    if (rf.line_height > pl.max_line_height) pl.max_line_height = rf.line_height;
                }

                if (*p == '\n') {
                    finalize_line(layout, &pl, line_top_y);
                    line_top_y += (pl.max_ascent + pl.max_descent > pl.max_line_height
                        ? pl.max_ascent + pl.max_descent : pl.max_line_height);
                    line_index++;
                    cursor_x = 0.0f;
                    line_has_content = false;
                    pl = (PendingLine){ .first_run = layout->run_count,
                                        .max_ascent = 0, .max_descent = 0,
                                        .max_line_height = 0 };
                    p++;
                } else if (*p == ' ') {
                    p++;
                }
            }
        } else {
            /* Non-text inline child: one unbreakable box (e.g. a small
               inline image). Sized by its own explicit width/height;
               0 falls back to a small fixed box rather than 0×0, so a
               misconfigured inline child stays visible/debuggable
               instead of silently collapsing to nothing. */
            float cw = child->desc.width  > 0.0f ? child->desc.width  : 20.0f * node_ui_scale(child);
            float ch = child->desc.height > 0.0f ? child->desc.height : 20.0f * node_ui_scale(child);

            if (line_has_content && cursor_x + cw > content_w) {
                finalize_line(layout, &pl, line_top_y);
                line_top_y += (pl.max_ascent + pl.max_descent > pl.max_line_height
                    ? pl.max_ascent + pl.max_descent : pl.max_line_height);
                line_index++;
                cursor_x = 0.0f;
                line_has_content = false;
                pl = (PendingLine){ .first_run = layout->run_count,
                                    .max_ascent = 0, .max_descent = 0,
                                    .max_line_height = 0 };
            }

            if (!push_run(layout, child, NULL, 0, cursor_x, cw, line_index))
                goto done;
            /* A non-text run's ascent contribution is its own full
               height (it has no baseline of its own to align text
               against) — treat it as if ascent == box height, descent
               0, so the line grows to fit it without misplacing text
               baselines sharing the line. */
            if (ch > pl.max_ascent) pl.max_ascent = ch;
            if (ch > pl.max_line_height) pl.max_line_height = ch;
            cursor_x += cw;
            line_has_content = true;
        }
    }

    if (line_has_content || layout->run_count == 0)
        finalize_line(layout, &pl, line_top_y);

done:
    layout->line_count = line_index + (line_has_content ? 1 : 0);
    if (layout->line_count == 0) layout->line_count = 1;
    layout->content_h = line_top_y +
        (pl.max_ascent + pl.max_descent > pl.max_line_height
            ? pl.max_ascent + pl.max_descent : pl.max_line_height);
}

void ca_inline_layout_destroy(Ca_InlineLayout *layout)
{
    if (!layout) return;
    free(layout->runs);
    layout->runs = NULL;
    layout->run_count = layout->run_capacity = 0;
}
