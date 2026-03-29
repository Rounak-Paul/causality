#include "ca_internal.h"
#include "window.h"
#include "event.h"
#include "renderer.h"
#include "ui.h"
#include "css.h"

#include <stdlib.h>

Ca_Instance *ca_instance_create(const Ca_InstanceDesc *desc)
{
    if (!ca_window_system_init())
        return NULL;

    Ca_Instance *inst = (Ca_Instance *)calloc(1, sizeof(Ca_Instance));
    if (!inst) {
        glfwTerminate();
        return NULL;
    }

    ca_event_init(inst);
    ca_ui_init(inst);

    /* Cache font settings from descriptor */
    if (desc && desc->font_path)
        snprintf(inst->font_path, sizeof(inst->font_path), "%s", desc->font_path);
    if (desc && desc->bold_font_path)
        snprintf(inst->bold_font_path, sizeof(inst->bold_font_path), "%s", desc->bold_font_path);
    inst->font_size_px = (desc && desc->font_size_px > 0.0f)
                         ? desc->font_size_px : 12.0f;

    if (!ca_renderer_init(inst, desc)) {
        ca_ui_shutdown(inst);
        ca_event_shutdown(inst);
        free(inst);
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
    /* Destroy windows first — their Vulkan surfaces / swapchains
       require the device to still be alive for proper cleanup. */
    ca_window_system_shutdown(instance);
    ca_renderer_shutdown(instance);
    ca_ui_shutdown(instance);
    ca_event_shutdown(instance);
    free(instance);
    printf("[causality] instance destroyed\n");
}

bool ca_instance_tick(Ca_Instance *instance)
{
    if (!ca_window_system_tick(instance)) return false;
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

