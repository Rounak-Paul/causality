// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "ca_internal.h"
#include "window.h"
#include "event.h"
#include "renderer.h"
#include "ui.h"
#include "css.h"
#include "style.h"
#include "widget.h"
#include "menu_storage.h"
#include "../platform/app_menu.h"
#include "../renderer/shader_cache.h"

/* Forward decls into the reactive subsystem (src/reactive/signal.c). */
void ca_reactive_flush(Ca_Instance *inst);
void ca_reactive_run_frame_effects(Ca_Instance *inst);
void ca_reactive_release_instance(Ca_Instance *inst);
bool ca_popup_system_init(Ca_Instance *inst);
void ca_popup_system_tick(Ca_Instance *inst);
void ca_popup_system_shutdown(Ca_Instance *inst);

/*
 * Create and initialise a Causality instance.
 *
 * Initialises GLFW, allocates the instance, sets up the event system, UI
 * subsystem, popup manager, and Vulkan renderer.  Font paths and the default
 * UI scale are copied from desc.  Returns NULL on any failure; all partial
 * state is cleaned up before returning.
 *
 * desc     Configuration descriptor; may be NULL for all defaults.
 * Returns  Newly allocated instance, or NULL on failure.
 */
Ca_Instance *ca_instance_create(const Ca_InstanceDesc *desc)
{
    if (!ca_window_system_init())
        return NULL;

    Ca_Instance *inst = (Ca_Instance *)CA_CALLOC(1, sizeof(Ca_Instance));
    if (!inst) {
        glfwTerminate();
        return NULL;
    }

    if (!ca_pool_init(&inst->windows, sizeof(Ca_Window),
                      ca_pool_recommended_chunk_capacity(sizeof(Ca_Window)))) {
        CA_FREE(inst);
        glfwTerminate();
        return NULL;
    }

    if (!ca_event_init(inst)) {
        ca_pool_destroy(&inst->windows, NULL, NULL);
        CA_FREE(inst);
        glfwTerminate();
        return NULL;
    }
    ca_ui_init(inst);
    if (!ca_popup_system_init(inst)) {
        ca_event_shutdown(inst);
        ca_pool_destroy(&inst->windows, NULL, NULL);
        CA_FREE(inst);
        glfwTerminate();
        return NULL;
    }

    /* Cache font settings from descriptor */
    if (desc && desc->font_path)
        snprintf(inst->font_path, sizeof(inst->font_path), "%s", desc->font_path);
    if (desc && desc->bold_font_path)
        snprintf(inst->bold_font_path, sizeof(inst->bold_font_path), "%s", desc->bold_font_path);
    {
        float s = desc ? desc->default_ui_scale : 0.0f;
        if (s < 0.25f && s > 0.0f) s = 0.25f;
        if (s > 4.0f)  s = 4.0f;
        inst->default_ui_scale = s; /* 0 means "unset" → windows default to 1.0 */
    }
    inst->disable_vsync = desc && desc->disable_vsync;
    ca_shader_cache_init_dir(inst, desc ? desc->shader_cache_dir : NULL);

    if (!ca_renderer_init(inst, desc)) {
        ca_popup_system_shutdown(inst);
        ca_ui_shutdown(inst);
        ca_event_shutdown(inst);
        ca_pool_destroy(&inst->windows, NULL, NULL);
        CA_FREE(inst);
        glfwTerminate();
        return NULL;
    }

    inst->system_stylesheet = ca_style_create_system_stylesheet();
    if (!inst->system_stylesheet)
        fprintf(stderr, "[causality] warning: failed to load system styles\n");

    printf("[causality] instance created (%s)\n",
           desc && desc->app_name ? desc->app_name : "unnamed");
    return inst;
}

/*
 * Destroy an instance and release all associated resources.
 *
 * Shuts down the popup system, all windows, the renderer, the UI subsystem,
 * the event system, and the reactive runtime before freeing the instance.
 *
 * instance  Instance to destroy; no-op if NULL.
 */
void ca_instance_destroy(Ca_Instance *instance)
{
    if (!instance) return;
    ca_widget_ctx_release_instance(instance);
    ca_popup_system_shutdown(instance);
    /* Destroy windows first — their Vulkan surfaces / swapchains
       require the device to still be alive for proper cleanup. */
    ca_window_system_shutdown(instance);
    ca_renderer_shutdown(instance);
    ca_pool_destroy(&instance->windows, NULL, NULL);
    ca_ui_shutdown(instance);
    ca_event_shutdown(instance);
    ca_reactive_release_instance(instance);
    ca_menu_storage_destroy(&instance->app_menu_storage,
                            &instance->app_menus);
    ca_css_destroy(instance->system_stylesheet);
    CA_FREE(instance);
    printf("[causality] instance destroyed\n");
}

/*
 * Advance one application frame.
 *
 * Processes OS window events, ticks the popup manager, flushes reactive
 * effects, runs every registered frame effect, runs the UI update pass,
 * and submits a renderer frame.
 *
 * instance  Instance to tick.
 * Returns   true while at least one window is open; false when all are closed.
 */
bool ca_instance_tick(Ca_Instance *instance)
{
    ca_profile_begin(instance, "Platform Events");
    bool window_open = ca_window_system_tick(instance);
    ca_profile_end(instance, "Platform Events");
    if (!window_open) return false;

    ca_profile_begin(instance, "Platform Popups");
    ca_popup_system_tick(instance);
    ca_profile_end(instance, "Platform Popups");

    /* Run reactive effects scheduled since the previous tick. */
    ca_profile_begin(instance, "Platform Reactive");
    ca_reactive_flush(instance);
    ca_profile_end(instance, "Platform Reactive");

    /* Run every ca_frame_effect unconditionally, every tick. */
    ca_profile_begin(instance, "Platform Frame Effects");
    ca_reactive_run_frame_effects(instance);
    ca_profile_end(instance, "Platform Frame Effects");

    ca_profile_begin(instance, "Platform UI");
    ca_ui_update(instance);
    ca_profile_end(instance, "Platform UI");

    ca_profile_begin(instance, "Platform Renderer");
    ca_renderer_frame(instance);
    ca_profile_end(instance, "Platform Renderer");
    return true;
}

void ca_instance_set_profile_hooks(Ca_Instance *instance,
                                   const Ca_ProfileHooks *hooks)
{
    if (!instance) return;
    instance->profile_hooks = hooks ? *hooks : (Ca_ProfileHooks){0};
}

/*
 * Wake the event loop from another thread or an idle wait.
 *
 * Posts an empty GLFW event, causing glfwWaitEvents() to return immediately
 * so the main loop can process any pending reactive updates.
 */
void ca_instance_wake(void)
{
    glfwPostEmptyEvent();
}

/**
 * Schedule a frame without turning the instance into a polling loop.
 *
 * @param instance Instance whose event loop should wake.
 * @param delay_seconds Non-negative delay before the requested frame.
 */
void ca_instance_request_frame_after(Ca_Instance *instance,
                                     double delay_seconds)
{
    if (!instance) return;
    if (delay_seconds < 0.0) delay_seconds = 0.0;

    const double deadline = glfwGetTime() + delay_seconds;
    if (!instance->frame_deadline_pending ||
        deadline < instance->frame_deadline) {
        instance->frame_deadline = deadline;
        instance->frame_deadline_pending = true;
    }
}

/*
 * Control whether the instance runs in continuous (polling) or event-driven mode.
 *
 * instance    Instance to configure; no-op if NULL.
 * continuous  true = call glfwPollEvents each tick; false = glfwWaitEvents.
 */
void ca_instance_set_continuous(Ca_Instance *instance, bool continuous)
{
    if (!instance) return;
    instance->continuous = continuous;
}

/*
 * Set the background callback inherited by windows without an override.
 *
 * instance   Instance whose windows inherit the callback.
 * fn         Background callback, or NULL to clear it.
 * user_data  Opaque callback data.
 */
void ca_instance_set_bg_render(Ca_Instance *instance,
                               Ca_BgRenderFn fn,
                               void *user_data)
{
    if (!instance) return;
    instance->default_bg_render_fn = fn;
    instance->default_bg_render_data = user_data;
    for (size_t i = 0; i < ca_pool_slot_count(&instance->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(instance->windows, Ca_Window, i);
        if (window->in_use && !window->bg_render_fn)
            window->needs_render = true;
    }
}

/*
 * Attach a parsed CSS stylesheet to the instance.
 *
 * instance  Instance to configure; no-op if NULL.
 * ss        Stylesheet to use, or NULL to clear the current stylesheet.
 */
void ca_instance_set_stylesheet(Ca_Instance *instance, Ca_Stylesheet *ss)
{
    if (!instance) return;
    instance->stylesheet = ss;
}

/* Re-resolve every styled node and schedule layout and paint work. */
void ca_instance_refresh_styles(Ca_Instance *instance)
{
    if (!instance || (!instance->system_stylesheet && !instance->stylesheet)) return;
    for (size_t wi = 0; wi < ca_pool_slot_count(&instance->windows); ++wi) {
        Ca_Window *window = CA_POOL_AT(instance->windows, Ca_Window, wi);
        if (!window->in_use || ca_pool_slot_count(&window->node_pool) == 0)
            continue;
        for (uint32_t ni = 0u; ni < ca_pool_slot_count(&window->node_pool); ++ni) {
            Ca_Node *node = CA_POOL_AT(window->node_pool, Ca_Node, ni);
            if (!node->in_use) continue;
            ca_widget_refresh_css(node);
            node->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
        }
        /* ca_widget_refresh_css resets each node to base_desc then re-applies
           CSS.  Two system-managed nodes have C-programmed properties that CSS
           must not override:

           1. status_bar_node — no CSS class; its height comes from C code
              (ca_window_set_status_bar).  Restore it here so layout is correct.

           2. content_root — ca_ui_begin force-sets flex_grow=1 and clears the
              height/height_pct fields so the user's CSS height (e.g. "100%")
              does not compete with the title-bar/status-bar strips in the root
              flex column.  Without this restore the content_root claims the
              entire window height, pushing the status bar off-screen. */
        if (window->status_bar_node && window->status_bar_height > 0.0f) {
            window->status_bar_node->desc.height = window->status_bar_height;
            window->status_bar_node->desc.hidden = (window->status_bar_fn == NULL);
        }
        if (window->content_root) {
            window->content_root->desc.flex_grow  = 1.0f;
            window->content_root->desc.height     = 0.0f;
            window->content_root->desc.height_pct = false;
        }
        window->titlebar_needs_rebuild = true;
        window->needs_render = true;
    }
    ca_instance_wake();
}

/*
 * Set the global UI scale applied to every current and future window.
 *
 * Clamps scale to [0.25, 4.0].  Immediately rescales all open windows,
 * schedules a deferred layout rescale on each, and recomputes status-bar
 * heights from their raw (unscaled) values.
 *
 * instance  Instance to configure; no-op if NULL.
 * scale     Desired scale factor; 1.0 = no scaling.
 */
void ca_instance_set_scale(Ca_Instance *instance, float scale)
{
    if (!instance) return;
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 4.0f)  scale = 4.0f;

    instance->default_ui_scale = scale;

    /* Apply immediately to every currently open window so a runtime
       scale change takes effect without needing to reopen windows. */
    for (size_t i = 0; i < ca_pool_slot_count(&instance->windows); ++i) {
        Ca_Window *w = CA_POOL_AT(instance->windows, Ca_Window, i);
        if (!w->in_use) continue;
        float old_scale = (w->ui_scale > 0.0f) ? w->ui_scale : 1.0f;
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

    ca_instance_wake();
}

/*
 * Return the current instance-wide UI scale.
 *
 * instance  Instance to query; returns 1.0 if NULL.
 * Returns   Scale factor, defaulting to 1.0 if none has been set.
 */
float ca_instance_get_scale(const Ca_Instance *instance)
{
    if (!instance) return 1.0f;
    return (instance->default_ui_scale > 0.0f) ? instance->default_ui_scale : 1.0f;
}

/*
 * Register the application-level menu bar.
 *
 * Deep-copies all menu and item data into the instance so the caller may free
 * or modify the descriptors immediately after this call.  Sub-items are copied
 * one level deep (no recursive nesting).
 *
 * On macOS the native [NSApp mainMenu] is rebuilt immediately.
 * On other platforms the stored data is read by ca_ui_begin each frame.
 *
 * instance    Owning Ca_Instance.
 * menus       Array of top-level menu descriptors.
 * menu_count  Number of elements in menus.
 */
void ca_instance_set_app_menus(Ca_Instance       *instance,
                               const Ca_MenuDesc *menus,
                               int                menu_count)
{
    if (!instance || !menus || menu_count <= 0) {
        if (instance) {
            ca_menu_storage_resize(&instance->app_menu_storage,
                                   &instance->app_menus, 0);
            instance->app_menu_count = 0;
            ca_app_menu_set(instance);
        }
        return;
    }

    int count = menu_count;
    Ca_DynArray new_storage = { 0 };
    Ca_MenuBarMenu *new_menus = NULL;
    if (!ca_menu_storage_resize(&new_storage, &new_menus, (size_t)count))
        return;

    for (int mi = 0; mi < count; mi++) {
        const Ca_MenuDesc *src = &menus[mi];
        Ca_MenuBarMenu    *dst = &new_menus[mi];

        snprintf(dst->label, sizeof(dst->label), "%s", src->label ? src->label : "");

        int ic = src->item_count > 0 && src->items ? src->item_count : 0;
        if (!ca_menu_item_storage_resize(dst, (size_t)ic)) goto copy_failed;

        for (int ii = 0; ii < ic; ii++) {
            const Ca_MenuItemDesc *si = &src->items[ii];
            Ca_MenuBarItem        *di = &dst->items[ii];

            snprintf(di->label, sizeof(di->label), "%s", si->label ? si->label : "");
            di->action       = si->action;
            di->action_data  = si->action_data;
            di->separator    = si->separator;

            int sc = si->sub_item_count > 0 && si->sub_items
                ? si->sub_item_count : 0;
            if (!ca_menu_sub_item_storage_resize(di, (size_t)sc))
                goto copy_failed;

            for (int ki = 0; ki < sc; ki++) {
                const Ca_MenuItemDesc *ss = &si->sub_items[ki];
                snprintf(di->sub_items[ki].label, sizeof(di->sub_items[ki].label),
                         "%s", ss->label ? ss->label : "");
                di->sub_items[ki].action      = ss->action;
                di->sub_items[ki].action_data = ss->action_data;
            }
        }
    }

    ca_menu_storage_destroy(&instance->app_menu_storage,
                            &instance->app_menus);
    instance->app_menu_storage = new_storage;
    instance->app_menus = new_menus;
    instance->app_menu_count = count;
    ca_app_menu_set(instance);
    return;

copy_failed:
    ca_menu_storage_destroy(&new_storage, &new_menus);
}
