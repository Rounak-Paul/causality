// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "ca_internal.h"
#include "window.h"
#include "event.h"
#include "renderer.h"
#include "ui.h"
#include "css.h"

/* Forward decls into the reactive subsystem (src/reactive/signal.c). */
void ca_reactive_flush(Ca_Instance *inst);
void ca_reactive_release_instance(Ca_Instance *inst);
void ca_popup_system_init(Ca_Instance *inst);
void ca_popup_system_tick(Ca_Instance *inst);
void ca_popup_system_shutdown(Ca_Instance *inst);

Ca_Instance *ca_instance_create(const Ca_InstanceDesc *desc)
{
    if (!ca_window_system_init())
        return NULL;

    Ca_Instance *inst = (Ca_Instance *)CA_CALLOC(1, sizeof(Ca_Instance));
    if (!inst) {
        glfwTerminate();
        return NULL;
    }

    ca_event_init(inst);
    ca_ui_init(inst);
    ca_popup_system_init(inst);

    /* Cache font settings from descriptor */
    if (desc && desc->font_path)
        snprintf(inst->font_path, sizeof(inst->font_path), "%s", desc->font_path);
    if (desc && desc->bold_font_path)
        snprintf(inst->bold_font_path, sizeof(inst->bold_font_path), "%s", desc->bold_font_path);
    inst->font_size_px = (desc && desc->font_size_px > 0.0f)
                         ? desc->font_size_px : 12.0f;

    {
        float s = desc ? desc->default_ui_scale : 0.0f;
        if (s < 0.25f && s > 0.0f) s = 0.25f;
        if (s > 4.0f)  s = 4.0f;
        inst->default_ui_scale = s; /* 0 means "unset" → windows default to 1.0 */
    }

    if (!ca_renderer_init(inst, desc)) {
        ca_ui_shutdown(inst);
        ca_event_shutdown(inst);
        CA_FREE(inst);
        glfwTerminate();
        return NULL;
    }

    printf("[causality] instance created (%s)\n",
           desc && desc->app_name ? desc->app_name : "unnamed");
    return inst;
}

void ca_instance_destroy(Ca_Instance *instance)
{
    if (!instance) return;
    ca_popup_system_shutdown(instance);
    /* Destroy windows first — their Vulkan surfaces / swapchains
       require the device to still be alive for proper cleanup. */
    ca_window_system_shutdown(instance);
    ca_renderer_shutdown(instance);
    ca_ui_shutdown(instance);
    ca_event_shutdown(instance);
    ca_reactive_release_instance(instance);
    CA_FREE(instance);
    printf("[causality] instance destroyed\n");
}

bool ca_instance_tick(Ca_Instance *instance)
{
    if (!ca_window_system_tick(instance)) return false;
    ca_popup_system_tick(instance);
    /* Run reactive effects scheduled since the previous tick. */
    ca_reactive_flush(instance);
    ca_ui_update(instance);
    ca_renderer_frame(instance);
    return true;
}

void ca_instance_wake(void)
{
    glfwPostEmptyEvent();
}

void ca_instance_set_continuous(Ca_Instance *instance, bool continuous)
{
    if (!instance) return;
    instance->continuous = continuous;
}

void ca_instance_set_stylesheet(Ca_Instance *instance, Ca_Stylesheet *ss)
{
    if (!instance) return;
    instance->stylesheet = ss;
}

void ca_instance_set_scale(Ca_Instance *instance, float scale)
{
    if (!instance) return;
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 4.0f)  scale = 4.0f;

    float old_scale = (instance->default_ui_scale > 0.0f) ? instance->default_ui_scale : 1.0f;
    instance->default_ui_scale = scale;

    /* Apply immediately to every currently open window so a runtime
       scale change takes effect without needing to reopen windows. */
    for (int i = 0; i < CA_MAX_WINDOWS_TOTAL; i++) {
        Ca_Window *w = &instance->windows[i];
        if (!w->in_use) continue;
        w->ui_scale = scale;
        w->titlebar_needs_rebuild = true;

        /* Schedule a deferred rescale of the static content tree.
           Done at the start of the next frame (before input/drag processing)
           to avoid corrupting layout values mid-slider-drag.
           pending_scale_ratio accumulates if called multiple times per frame. */
        if (old_scale > 0.0f && scale != old_scale) {
            float ratio = scale / old_scale;
            w->pending_scale_ratio = (w->pending_scale_ratio > 0.0f)
                ? w->pending_scale_ratio * ratio
                : ratio;
        }

        /* Recompute status bar height from the stored raw (unscaled) height. */
        if (w->status_bar_node && w->status_bar_raw_height > 0.0f) {
            w->status_bar_height = w->status_bar_raw_height * scale;
            w->status_bar_node->desc.height = w->status_bar_height;
            w->status_bar_node->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
            w->statusbar_needs_rebuild = true;
        }

        if (w->root)
            w->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
    }
}

float ca_instance_get_scale(const Ca_Instance *instance)
{
    if (!instance) return 1.0f;
    return (instance->default_ui_scale > 0.0f) ? instance->default_ui_scale : 1.0f;
}

