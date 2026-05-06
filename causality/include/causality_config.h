// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.
//
// causality_config.h — compile-time tunables.
//
// All limits below have safe defaults. Override any of them by defining
// the macro on the compiler command line (e.g. -DCA_MAX_NODES_PER_WINDOW=8192)
// or by defining it before this header is included.

#pragma once

/* ---- Renderer ---- */

#ifndef CA_FRAMES_IN_FLIGHT
#  define CA_FRAMES_IN_FLIGHT 2
#endif

#ifndef CA_MAX_SWAPCHAIN_IMAGES
#  define CA_MAX_SWAPCHAIN_IMAGES 8
#endif

/* ---- Per-window pools ---- */

#ifndef CA_MAX_NODES_PER_WINDOW
#  define CA_MAX_NODES_PER_WINDOW 2048
#endif

#ifndef CA_MAX_NODE_CHILDREN
#  define CA_MAX_NODE_CHILDREN 256
#endif

#ifndef CA_MAX_DRAW_CMDS_PER_WINDOW
#  define CA_MAX_DRAW_CMDS_PER_WINDOW 8192
#endif

#ifndef CA_MAX_TRANSITIONS_PER_NODE
#  define CA_MAX_TRANSITIONS_PER_NODE 4
#endif

/* ---- Reactive signals ---- */

#ifndef CA_MAX_SIGNAL_SUBSCRIBERS
#  define CA_MAX_SIGNAL_SUBSCRIBERS 64
#endif

#ifndef CA_MAX_SIGNALS_PER_INSTANCE
#  define CA_MAX_SIGNALS_PER_INSTANCE 4096
#endif

#ifndef CA_MAX_EFFECTS_PER_INSTANCE
#  define CA_MAX_EFFECTS_PER_INSTANCE 2048
#endif

#ifndef CA_MAX_SIGNAL_DEPS
#  define CA_MAX_SIGNAL_DEPS 32
#endif

/* ---- Widget pools (per window) ---- */

#ifndef CA_MAX_LABELS_PER_WINDOW
#  define CA_MAX_LABELS_PER_WINDOW 512
#endif
#ifndef CA_MAX_BUTTONS_PER_WINDOW
#  define CA_MAX_BUTTONS_PER_WINDOW 384
#endif
#ifndef CA_MAX_INPUTS_PER_WINDOW
#  define CA_MAX_INPUTS_PER_WINDOW 64
#endif
#ifndef CA_MAX_CHECKBOXES_PER_WINDOW
#  define CA_MAX_CHECKBOXES_PER_WINDOW 64
#endif
#ifndef CA_MAX_RADIOS_PER_WINDOW
#  define CA_MAX_RADIOS_PER_WINDOW 64
#endif
#ifndef CA_MAX_SLIDERS_PER_WINDOW
#  define CA_MAX_SLIDERS_PER_WINDOW 32
#endif
#ifndef CA_MAX_TOGGLES_PER_WINDOW
#  define CA_MAX_TOGGLES_PER_WINDOW 32
#endif
#ifndef CA_MAX_PROGRESS_PER_WINDOW
#  define CA_MAX_PROGRESS_PER_WINDOW 32
#endif
#ifndef CA_MAX_SELECTS_PER_WINDOW
#  define CA_MAX_SELECTS_PER_WINDOW 16
#endif
#ifndef CA_MAX_TABBARS_PER_WINDOW
#  define CA_MAX_TABBARS_PER_WINDOW 8
#endif
#ifndef CA_MAX_TREENODES_PER_WINDOW
#  define CA_MAX_TREENODES_PER_WINDOW 256
#endif
#ifndef CA_MAX_TABLES_PER_WINDOW
#  define CA_MAX_TABLES_PER_WINDOW 8
#endif
#ifndef CA_MAX_TOOLTIPS_PER_WINDOW
#  define CA_MAX_TOOLTIPS_PER_WINDOW 32
#endif
#ifndef CA_MAX_CTXMENUS_PER_WINDOW
#  define CA_MAX_CTXMENUS_PER_WINDOW 512
#endif
#ifndef CA_MAX_MODALS_PER_WINDOW
#  define CA_MAX_MODALS_PER_WINDOW 4
#endif
#ifndef CA_MAX_SPLITTERS_PER_WINDOW
#  define CA_MAX_SPLITTERS_PER_WINDOW 16
#endif
#ifndef CA_MAX_VIEWPORTS_PER_WINDOW
#  define CA_MAX_VIEWPORTS_PER_WINDOW 8
#endif
#ifndef CA_MAX_MENUBARS_PER_WINDOW
#  define CA_MAX_MENUBARS_PER_WINDOW 2
#endif

/* ---- Text storage ---- */

#ifndef CA_LABEL_TEXT_MAX
#  define CA_LABEL_TEXT_MAX 256
#endif
#ifndef CA_BUTTON_TEXT_MAX
#  define CA_BUTTON_TEXT_MAX 128
#endif
#ifndef CA_INPUT_TEXT_MAX
#  define CA_INPUT_TEXT_MAX 512
#endif
#ifndef CA_OPTION_TEXT_MAX
#  define CA_OPTION_TEXT_MAX 128
#endif
#ifndef CA_CHAR_BUF_MAX
#  define CA_CHAR_BUF_MAX 32
#endif
