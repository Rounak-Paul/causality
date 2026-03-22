/* ui.h — top-level internal UI entry points */
#pragma once

#include "ca_internal.h"

/* Called once by ca_instance_create / ca_instance_destroy. */
void ca_ui_init(Ca_Instance *inst);
void ca_ui_shutdown(Ca_Instance *inst);

/* Called by ca_window_create / ca_window_destroy. */
void ca_ui_window_init(Ca_Window *win);
void ca_ui_window_shutdown(Ca_Window *win);

/* Called every frame inside ca_instance_tick, before ca_renderer_frame. */
void ca_ui_update(Ca_Instance *inst);
