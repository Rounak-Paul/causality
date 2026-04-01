/* causality/src/window.h — internal window subsystem */
#pragma once

#include "ca_internal.h"

bool        ca_window_system_init(void);
void        ca_window_system_shutdown(Ca_Instance *inst);
bool        ca_window_system_tick(Ca_Instance *inst);

CA_API Ca_Window  *ca_window_create(Ca_Instance *inst, const Ca_WindowDesc *desc);
CA_API void        ca_window_destroy(Ca_Window *window);
GLFWwindow *ca_window_glfw(const Ca_Window *window);
void        ca_window_resize_pass(Ca_Window *window);

