/* paint.h — internal header for the paint (draw command generation) pass */
#pragma once

#include "ca_internal.h"

/* Walk the dirty-content nodes and write Ca_DrawCmd entries, then generate
   glyph draw commands for label/button text if a font is loaded.
   The draw_cmd_count is reset by the caller (ui_update) before this runs,
   so this always does a full rebuild of the draw list. */
void ca_paint_pass(Ca_Instance *inst, Ca_Window *win);
