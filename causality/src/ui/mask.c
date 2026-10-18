// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "mask.h"

#include "motion_lists.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MASK_FAR 1.0e9f

static float word_float(uint32_t word)
{
    float f;
    memcpy(&f, &word, sizeof(f));
    return f;
}

uint32_t ca_mask_stop_count(uint32_t handle)
{
    uint32_t words = 0;
    const uint32_t *w = ca_motion_list_items(handle, &words);
    return (w && words >= 1u) ? (words - 1u) / 3u : 0u;
}

/**
 * Computes every stop's final position along the gradient line.
 *
 * @param stops  Stop words (after the angle word).
 * @param count  Number of stops.
 * @param length Gradient line length in px.
 * @param scale  Factor applied to px offsets.
 * @param pos    Receives `count` positions.
 */
static void resolve_positions(const uint32_t *stops, uint32_t count, float length, float scale, float *pos)
{
    for (uint32_t i = 0; i < count; ++i) {
        const float pct = word_float(stops[i * 3u]);
        pos[i] = isnan(pct) ? NAN : pct * 0.01f * length + word_float(stops[i * 3u + 1u]) * scale;
    }
    if (isnan(pos[0])) pos[0] = 0.0f;
    if (isnan(pos[count - 1u])) pos[count - 1u] = length;
    float previous = pos[0];
    for (uint32_t i = 1; i < count; ++i) {
        if (!isnan(pos[i])) {
            if (pos[i] < previous) pos[i] = previous;
            previous = pos[i];
        }
    }
    for (uint32_t i = 1; i < count; ) {
        if (!isnan(pos[i])) { ++i; continue; }
        uint32_t end = i;
        while (isnan(pos[end])) ++end;
        const float from = pos[i - 1u], step = (pos[end] - from) / (float)(end - i + 1u);
        for (uint32_t k = i; k < end; ++k) pos[k] = from + step * (float)(k - i + 1u);
        i = end;
    }
}

uint32_t ca_mask_resolve(uint32_t handle, float width, float height, float px_scale,
                         Ca_MaskLine *line, Ca_MaskSegment *out, uint32_t capacity)
{
    uint32_t words = 0;
    const uint32_t *w = ca_motion_list_items(handle, &words);
    if (!w || words < 7u) return 0;
    const uint32_t count = (words - 1u) / 3u;
    const float radians = word_float(w[0]) * (3.14159265358979f / 180.0f);
    const float dx = sinf(radians), dy = -cosf(radians);
    const float length = fabsf(width * dx) + fabsf(height * dy);
    if (line) *line = (Ca_MaskLine){ radians, dx, dy, length * 0.5f };

    float *pos = malloc(sizeof(float) * count);
    if (!pos) return 0;
    resolve_positions(w + 1, count, length, px_scale, pos);

    uint32_t emitted = 0;
    for (uint32_t i = 0; i <= count; ++i) {
        const float t0 = i == 0 ? -MASK_FAR : pos[i - 1u];
        const float t1 = i == count ? MASK_FAR : pos[i];
        const float a0 = word_float(w[1u + (i == 0 ? 0u : i - 1u) * 3u + 2u]);
        const float a1 = word_float(w[1u + (i == count ? count - 1u : i) * 3u + 2u]);
        if (t1 <= t0 || (a0 <= 0.0f && a1 <= 0.0f)) continue;
        if (emitted < capacity) out[emitted] = (Ca_MaskSegment){ t0, t1, a0, a1 };
        ++emitted;
    }
    free(pos);
    return emitted;
}

bool ca_cutout_resolve(uint32_t handle, float px_scale, float out[5])
{
    uint32_t words = 0;
    const uint32_t *w = ca_motion_list_items(handle, &words);
    if (!w || words != 5u) return false;
    const float top = word_float(w[0]), right = word_float(w[1]);
    const float bottom = word_float(w[2]), left = word_float(w[3]), radius = word_float(w[4]);
    out[0] = left * px_scale; out[1] = top * px_scale;
    out[2] = right * px_scale; out[3] = bottom * px_scale;
    out[4] = radius * px_scale;
    return true;
}

bool ca_ambient_resolve(uint32_t handle, float px_scale, float out[3])
{
    uint32_t words = 0;
    const uint32_t *w = ca_motion_list_items(handle, &words);
    if (!w || words != 3u) return false;
    out[0] = word_float(w[0]) * px_scale;
    out[1] = word_float(w[1]) * px_scale;
    out[2] = word_float(w[2]);
    return true;
}
