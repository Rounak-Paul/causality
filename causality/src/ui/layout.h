/* layout.h — internal header for the layout pass */
#pragma once

#include "ca_internal.h"

/* Recursively compute x/y/w/h for every node in the window's tree.
   Only needs to run when a node has CA_DIRTY_LAYOUT or CA_DIRTY_CHILDREN set. */
void ca_layout_pass(Ca_Window *win);
