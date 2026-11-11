// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* motion.h — CSS transitions and keyframe animations.

   Transitions: when a node's resolved style changes an animatable property
   and a `transition` entry covers it, the node eases from the old value to
   the new one. Animations: `animation` names an `@keyframes` block whose
   stops drive the node's properties for as long as the animation runs.
   Both write straight into node->desc each frame, ahead of layout. */
#pragma once

#include "ca_internal.h"

/**
 * Maps linear progress through a timing function.
 *
 * @param easing Timing function; linear when NULL or of an unknown kind.
 * @param t      Linear progress, clamped to [0, 1].
 * @return Eased progress; a bezier with y outside [0, 1] overshoots 0..1.
 */
float ca_easing_eval(const Ca_EasingDef *easing, float t);

/**
 * Zips a resolved style's transition and animation lists onto a node.
 *
 * Each list property repeats cyclically against the longest list, as in
 * CSS. Running animations are reconciled with the new list on the next tick.
 *
 * @param node Node receiving the specs.
 * @param rs   Resolved style carrying the lists.
 */
void ca_motion_store(Ca_Node *node, const Ca_ResolvedStyle *rs);

/**
 * Starts transitions for every animatable property whose value changed.
 *
 * Call right after a node's desc was re-resolved. A started transition
 * puts the old value back in node->desc so the node does not flash its
 * target for a frame before the first tick.
 *
 * @param node     Node whose desc just changed.
 * @param old      Its desc before the change.
 * @param old_text Its text colour before the change (0 = palette default).
 * @param new_text Its text colour after the change.
 */
void ca_motion_begin_transitions(Ca_Node *node, const Ca_NodeDesc *old,
                                 uint32_t old_text, uint32_t new_text);

/**
 * Applies the layout properties a pseudo-state change moved.
 *
 * Widgets override some layout fields after CSS, so a pseudo-state
 * re-resolve leaves layout alone; this lets `:hover { padding: ... }` and
 * the like through for exactly the properties whose resolved value
 * changed, by writing them into node->desc (the transition machinery then
 * eases them).
 *
 * @param node     Node being re-resolved.
 * @param prev_css Descriptor the previous resolved style produces.
 * @param now_css  Descriptor the new resolved style produces.
 * @param now      The new resolved style (tells which properties it sets).
 */
void ca_motion_follow_layout(Ca_Node *node, const Ca_NodeDesc *prev_css,
                             const Ca_NodeDesc *now_css, const Ca_ResolvedStyle *now);

/**
 * Advances a node's transitions and animations to `now`.
 *
 * @param node            Node to step.
 * @param now             Current time in seconds (the backend's monotonic clock).
 * @param layout_affected Set to true when a layout-affecting property moved.
 * @return true while anything is still running, including the frame that
 *         lands on a final value, so the caller repaints it.
 */
bool ca_motion_tick(Ca_Node *node, double now, bool *layout_affected);

/**
 * Steps every animation and transition in a subtree that has just been shown, so the first frame
 * painted after it appears is already the animations' first frame rather than the resting state.
 *
 * @param node Root of the subtree.
 * @param now  Current time in seconds.
 */
void ca_motion_tick_shown(Ca_Node *node, double now);

/**
 * Writes a node's running animation values back after its style was re-resolved from CSS
 * (a hover change, a class change), which resets animated properties to their resting values
 * and would otherwise paint one unanimated frame before the next tick.
 *
 * @param node Node whose style was just re-resolved.
 * @param now  Current time in seconds.
 */
void ca_motion_reassert(Ca_Node *node, double now);

/**
 * Drops continuity records of animations nobody has refreshed lately.
 *
 * @param win Window whose records are pruned.
 * @param now Current time in seconds (the backend's monotonic clock).
 */
void ca_motion_window_tick(Ca_Window *win, double now);

/** The transition entry covering `prop` (last match wins), or NULL. */
const Ca_TransitionSpec *ca_motion_find_spec(const Ca_Node *node, int prop);

/** Frees a node's running-animation storage. */
void ca_motion_node_destroy(Ca_Node *node);
