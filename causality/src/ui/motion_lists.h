// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* motion_lists.h — interned comma lists for CSS transition / animation values.

   A `transition: a 1s, b 2s` declaration is a list of any length. Resolved
   styles are copied by value all over the style cascade, so they cannot own
   a variable-length list; instead each list is stored once, immutably, in a
   process-wide pool and a resolved style holds its 32-bit handle. Equal
   lists share one entry, so the pool grows only with the number of distinct
   lists an application ever writes. */
#pragma once

#include "ca_resolved_style.h"

#include <stdint.h>

typedef enum {
    CA_MOTION_LIST_TRANSITION_PROPERTY,   /* int           */
    CA_MOTION_LIST_TRANSITION_DURATION,   /* float seconds */
    CA_MOTION_LIST_TRANSITION_DELAY,      /* float seconds */
    CA_MOTION_LIST_TRANSITION_TIMING,     /* Ca_EasingDef  */
    CA_MOTION_LIST_ANIMATION_NAME,        /* uint32_t name hash */
    CA_MOTION_LIST_ANIMATION_DURATION,    /* float seconds */
    CA_MOTION_LIST_ANIMATION_DELAY,       /* float seconds */
    CA_MOTION_LIST_ANIMATION_TIMING,      /* Ca_EasingDef  */
    CA_MOTION_LIST_ANIMATION_ITERATIONS,  /* float, infinity allowed */
    CA_MOTION_LIST_ANIMATION_DIRECTION,   /* uint8_t Ca_AnimDirection */
    CA_MOTION_LIST_ANIMATION_FILL,        /* uint8_t Ca_AnimFill */
    CA_MOTION_LIST_ANIMATION_PLAY_STATE,  /* uint8_t: 0 running, 1 paused */
    CA_MOTION_LIST_KIND_COUNT
} Ca_MotionListKind;

/**
 * Stores a list in the pool, or finds the identical one already there.
 *
 * @param kind  Which list this is; fixes the item type.
 * @param items `count` items of that type.
 * @param count Number of items; 0 yields handle 0 (an empty list).
 * @return A handle for ca_motion_list_items, or 0 for an empty list or on
 *         allocation failure.
 */
uint32_t ca_motion_list_intern(Ca_MotionListKind kind, const void *items, uint32_t count);

/**
 * Looks a list up.
 *
 * @param handle Handle from ca_motion_list_intern; 0 means an empty list.
 * @param count  Receives the number of items (0 for an invalid handle).
 * @return The items, valid for the life of the process, or NULL when empty.
 */
const void *ca_motion_list_items(uint32_t handle, uint32_t *count);
