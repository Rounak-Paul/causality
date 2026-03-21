/* widget.h — internal widget implementation helpers */
#pragma once

#include "ca_internal.h"

/* Check mouse position against button hit-boxes and fire callbacks.
   Called once per window per frame inside ca_ui_update. */
void ca_widget_input_pass(Ca_Window *win);
