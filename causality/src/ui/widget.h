/* widget.h — internal widget implementation helpers */
#pragma once

#include "ca_internal.h"

/* Check mouse position against button hit-boxes and fire callbacks.
   Called once per window per frame inside ca_ui_update. */
void ca_widget_input_pass(Ca_Window *win);

/* Activate / deactivate the widget build context for a window.
   Used by ca_ui_update to wrap the on_frame callback so that
   widget creation functions work inside per-frame callbacks. */
void ca_widget_ctx_enter(Ca_Window *win);
void ca_widget_ctx_leave(void);
