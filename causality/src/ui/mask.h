// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* mask.h — CSS `mask-image: linear-gradient(...)` alpha masks.

   A mask is parsed once into a list of 32-bit words interned in the
   process-wide pool (motion_lists.h); styles, descriptors and draw commands
   carry only its handle. Layout of the words:

     word 0          gradient angle in degrees (float bits; CSS: 0 = to top)
     per stop, 3     position percent (float bits, NaN = unpositioned),
                     position px offset (float bits), stop alpha (float bits)

   A stop's position is `percent * gradient-line-length + px`. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** One linear piece of a resolved mask: alpha ramps a0 -> a1 over [t0, t1) px along the gradient line. */
typedef struct {
    float t0, t1, a0, a1;
} Ca_MaskSegment;

/** Orientation of a resolved mask's gradient line for a box. */
typedef struct {
    float angle;          /* gradient angle in radians (CSS: 0 = to top) */
    float dir_x, dir_y;   /* unit direction, y down */
    float half_length;    /* half the gradient line length in px */
} Ca_MaskLine;

/** Number of stops a mask stores (0 for an invalid handle). */
uint32_t ca_mask_stop_count(uint32_t handle);

/**
 * Resolves a mask for a box.
 *
 * Positions are fixed up as CSS specifies: a missing first/last position is
 * 0%/100%, runs of missing positions are spread evenly, and positions never
 * decrease. Segments that are fully transparent or empty are omitted; the
 * first and last segments extend to +-1e9 so the end alphas hold outside
 * the stops.
 *
 * @param handle   Mask handle; 0 yields no segments.
 * @param width    Box width in px.
 * @param height   Box height in px.
 * @param px_scale Factor applied to the stops' px offsets (the window UI scale).
 * @param line     Receives the gradient line; may be NULL.
 * @param out      Receives up to `capacity` segments; may be NULL when `capacity` is 0.
 * @param capacity Capacity of `out`.
 * @return The number of segments the mask resolves to (may exceed `capacity`;
 *         call again with a larger buffer).
 */
uint32_t ca_mask_resolve(uint32_t handle, float width, float height, float px_scale,
                         Ca_MaskLine *line, Ca_MaskSegment *out, uint32_t capacity);

/**
 * Resolves a backdrop cutout: a rounded window, inset from the element's box,
 * where the backdrop filter does not apply.
 *
 * Stored as five float words: top, right, bottom, left (px) and radius (px).
 *
 * @param handle   Cutout handle; 0 yields false.
 * @param px_scale Factor applied to every length (the window UI scale).
 * @param out      Receives left, top, right, bottom insets and the radius.
 * @return true when the handle names a cutout.
 */
bool ca_cutout_resolve(uint32_t handle, float px_scale, float out[5]);

/**
 * Resolves an ambient-light handle.
 *
 * @param handle   Ambient handle; 0 yields false.
 * @param px_scale Factor applied to the blur and reach lengths (the window UI scale).
 * @param out      Receives the blur length (px), the reach (px) and the strength.
 * @return true when the handle names an ambient light.
 */
bool ca_ambient_resolve(uint32_t handle, float px_scale, float out[3]);
