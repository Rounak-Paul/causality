// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "ca_internal.h"
#include "window.h"

#include <string.h>

typedef struct Ca_PopupEntry {
    char             title[CA_POPUP_TITLE_MAX];
    char             message[CA_POPUP_TEXT_MAX];
    Ca_PopupButtons  buttons;
    Ca_PopupResultFn on_result;
    void            *result_data;
} Ca_PopupEntry;

static void popup_copy_entry(Ca_PopupEntry *dst, const Ca_PopupDesc *src)
{
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->title, sizeof(dst->title), "%s",
             (src && src->title && src->title[0]) ? src->title : "Message");
    snprintf(dst->message, sizeof(dst->message), "%s",
             (src && src->message) ? src->message : "");
    dst->buttons = src ? src->buttons : CA_POPUP_BUTTONS_OK;
    if (dst->buttons < CA_POPUP_BUTTONS_OK || dst->buttons > CA_POPUP_BUTTONS_YES_NO)
        dst->buttons = CA_POPUP_BUTTONS_OK;
    dst->on_result = src ? src->on_result : NULL;
    dst->result_data = src ? src->result_data : NULL;
}

static void popup_emit_result(const Ca_PopupEntry *entry, Ca_PopupResult result)
{
    if (entry && entry->on_result)
        entry->on_result(result, entry->result_data);
}

static bool popup_queue_push(Ca_Instance *inst, const Ca_PopupEntry *entry)
{
    if (!inst || !entry) return false;
    if (inst->popup_queue_count >= CA_POPUP_QUEUE_MAX)
        return false;
    int idx = inst->popup_queue_count++;
    snprintf(inst->popup_queue[idx].title, sizeof(inst->popup_queue[idx].title), "%s", entry->title);
    snprintf(inst->popup_queue[idx].message, sizeof(inst->popup_queue[idx].message), "%s", entry->message);
    inst->popup_queue[idx].buttons = entry->buttons;
    inst->popup_queue[idx].on_result = entry->on_result;
    inst->popup_queue[idx].result_data = entry->result_data;
    return true;
}

static bool popup_queue_push_front(Ca_Instance *inst, const Ca_PopupEntry *entry)
{
    if (!inst || !entry) return false;
    if (inst->popup_queue_count >= CA_POPUP_QUEUE_MAX)
        return false;
    memmove(&inst->popup_queue[1], &inst->popup_queue[0],
            (size_t)inst->popup_queue_count * sizeof(inst->popup_queue[0]));
    snprintf(inst->popup_queue[0].title, sizeof(inst->popup_queue[0].title), "%s", entry->title);
    snprintf(inst->popup_queue[0].message, sizeof(inst->popup_queue[0].message), "%s", entry->message);
    inst->popup_queue[0].buttons = entry->buttons;
    inst->popup_queue[0].on_result = entry->on_result;
    inst->popup_queue[0].result_data = entry->result_data;
    inst->popup_queue_count++;
    return true;
}

static bool popup_queue_pop(Ca_Instance *inst, Ca_PopupEntry *out)
{
    if (!inst || !out || inst->popup_queue_count <= 0) return false;
    snprintf(out->title, sizeof(out->title), "%s", inst->popup_queue[0].title);
    snprintf(out->message, sizeof(out->message), "%s", inst->popup_queue[0].message);
    out->buttons = inst->popup_queue[0].buttons;
    out->on_result = inst->popup_queue[0].on_result;
    out->result_data = inst->popup_queue[0].result_data;
    memmove(&inst->popup_queue[0], &inst->popup_queue[1],
            (size_t)(inst->popup_queue_count - 1) * sizeof(inst->popup_queue[0]));
    inst->popup_queue_count--;
    return true;
}

static void popup_mark_and_close(Ca_Instance *inst, Ca_PopupResult result)
{
    if (!inst) return;
    inst->popup_pending_result = result;
    if (inst->popup_window && inst->popup_window->in_use)
        ca_window_close(inst->popup_window);
}

static void popup_on_ok(Ca_Button *btn, void *user_data)
{
    (void)btn;
    popup_mark_and_close((Ca_Instance *)user_data, CA_POPUP_RESULT_OK);
}

static void popup_on_cancel(Ca_Button *btn, void *user_data)
{
    (void)btn;
    popup_mark_and_close((Ca_Instance *)user_data, CA_POPUP_RESULT_CANCEL);
}

static void popup_on_yes(Ca_Button *btn, void *user_data)
{
    (void)btn;
    popup_mark_and_close((Ca_Instance *)user_data, CA_POPUP_RESULT_YES);
}

static void popup_on_no(Ca_Button *btn, void *user_data)
{
    (void)btn;
    popup_mark_and_close((Ca_Instance *)user_data, CA_POPUP_RESULT_NO);
}

static void popup_build_ui(Ca_Instance *inst, Ca_Window *win, const Ca_PopupEntry *entry)
{
    if (!inst || !win || !entry) return;

    ca_ui_begin(win, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .id        = "ca-popup-root",
        .style     = "ca-popup-root",
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .id        = "ca-popup-card",
        .style     = "ca-popup-card",
    });

    ca_text(&(Ca_TextDesc){
        .text  = entry->title,
        .id    = "ca-popup-title",
        .style = "ca-popup-title",
    });

    ca_text(&(Ca_TextDesc){
        .text  = entry->message,
        .id    = "ca-popup-message",
        .style = "ca-popup-message",
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .id        = "ca-popup-actions",
        .style     = "ca-popup-actions",
    });

    if (entry->buttons == CA_POPUP_BUTTONS_OK) {
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "OK",
            .on_click   = popup_on_ok,
            .click_data = inst,
            .id         = "ca-popup-ok",
            .style      = "ca-popup-btn ca-popup-btn-primary",
        });
        ca_btn_end();
    } else if (entry->buttons == CA_POPUP_BUTTONS_OK_CANCEL) {
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "Cancel",
            .on_click   = popup_on_cancel,
            .click_data = inst,
            .id         = "ca-popup-cancel",
            .style      = "ca-popup-btn",
        });
        ca_btn_end();
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "OK",
            .on_click   = popup_on_ok,
            .click_data = inst,
            .id         = "ca-popup-ok",
            .style      = "ca-popup-btn ca-popup-btn-primary",
        });
        ca_btn_end();
    } else {
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "No",
            .on_click   = popup_on_no,
            .click_data = inst,
            .id         = "ca-popup-no",
            .style      = "ca-popup-btn",
        });
        ca_btn_end();
        ca_btn_begin(&(Ca_BtnDesc){
            .text       = "Yes",
            .on_click   = popup_on_yes,
            .click_data = inst,
            .id         = "ca-popup-yes",
            .style      = "ca-popup-btn ca-popup-btn-primary",
        });
        ca_btn_end();
    }

    ca_div_end();
    ca_div_end();
    ca_ui_end();
}

static bool popup_activate(Ca_Instance *inst, const Ca_PopupEntry *entry)
{
    if (!inst || !entry) return false;

    Ca_Window *win = inst->popup_window;
    if (!win || !win->in_use) {
        win = ca_window_create_reserved(inst, &(Ca_WindowDesc){
            .title  = entry->title,
            .width  = 440,
            .height = 220,
        }, 0);
        if (!win) return false;
        inst->popup_window = win;
    }

    ca_window_set_title(win, entry->title);

    snprintf(inst->popup_current.title, sizeof(inst->popup_current.title), "%s", entry->title);
    snprintf(inst->popup_current.message, sizeof(inst->popup_current.message), "%s", entry->message);
    inst->popup_current.buttons     = entry->buttons;
    inst->popup_current.on_result   = entry->on_result;
    inst->popup_current.result_data = entry->result_data;

    inst->popup_pending_result = CA_POPUP_RESULT_NONE;
    inst->popup_active = true;

    popup_build_ui(inst, win, entry);

    GLFWwindow *glfw = ca_window_glfw(win);
    if (glfw) glfwFocusWindow(glfw);

    return true;
}

void ca_popup_system_init(Ca_Instance *inst)
{
    if (!inst) return;
    inst->popup_queue_count = 0;
    inst->popup_active = false;
    inst->popup_window = NULL;
    inst->popup_pending_result = CA_POPUP_RESULT_NONE;
    memset(&inst->popup_current, 0, sizeof(inst->popup_current));
}

void ca_popup_system_tick(Ca_Instance *inst)
{
    if (!inst) return;

    if (inst->popup_active && (!inst->popup_window || !inst->popup_window->in_use)) {
        Ca_PopupEntry entry;
        snprintf(entry.title, sizeof(entry.title), "%s", inst->popup_current.title);
        snprintf(entry.message, sizeof(entry.message), "%s", inst->popup_current.message);
        entry.buttons = inst->popup_current.buttons;
        entry.on_result = inst->popup_current.on_result;
        entry.result_data = inst->popup_current.result_data;

        Ca_PopupResult r = inst->popup_pending_result;
        if (r == CA_POPUP_RESULT_NONE)
            r = CA_POPUP_RESULT_CLOSED;

        inst->popup_active = false;
        inst->popup_window = NULL;
        inst->popup_pending_result = CA_POPUP_RESULT_NONE;
        memset(&inst->popup_current, 0, sizeof(inst->popup_current));

        popup_emit_result(&entry, r);
    }

    if (!inst->popup_active && inst->popup_queue_count > 0) {
        Ca_PopupEntry next;
        if (popup_queue_pop(inst, &next))
            (void)popup_activate(inst, &next);
    }
}

void ca_popup_system_shutdown(Ca_Instance *inst)
{
    if (!inst) return;

    if (inst->popup_active) {
        Ca_PopupEntry entry;
        snprintf(entry.title, sizeof(entry.title), "%s", inst->popup_current.title);
        snprintf(entry.message, sizeof(entry.message), "%s", inst->popup_current.message);
        entry.buttons = inst->popup_current.buttons;
        entry.on_result = inst->popup_current.on_result;
        entry.result_data = inst->popup_current.result_data;
        popup_emit_result(&entry, CA_POPUP_RESULT_CLOSED);
    }

    for (int i = 0; i < inst->popup_queue_count; i++) {
        Ca_PopupEntry entry;
        snprintf(entry.title, sizeof(entry.title), "%s", inst->popup_queue[i].title);
        snprintf(entry.message, sizeof(entry.message), "%s", inst->popup_queue[i].message);
        entry.buttons = inst->popup_queue[i].buttons;
        entry.on_result = inst->popup_queue[i].on_result;
        entry.result_data = inst->popup_queue[i].result_data;
        popup_emit_result(&entry, CA_POPUP_RESULT_CLOSED);
    }

    inst->popup_queue_count = 0;
    inst->popup_active = false;
    inst->popup_window = NULL;
    inst->popup_pending_result = CA_POPUP_RESULT_NONE;
    memset(&inst->popup_current, 0, sizeof(inst->popup_current));
}

bool ca_popup_show(Ca_Instance *instance, const Ca_PopupDesc *desc)
{
    if (!instance || !desc) return false;

    Ca_PopupEntry req;
    popup_copy_entry(&req, desc);

    if (instance->popup_active) {
        if (desc->replace_active) {
            instance->popup_pending_result = CA_POPUP_RESULT_REPLACED;
            if (instance->popup_window && instance->popup_window->in_use)
                ca_window_close(instance->popup_window);
            return popup_queue_push_front(instance, &req);
        }

        if (desc->queue_if_busy)
            return popup_queue_push(instance, &req);

        return false;
    }

    /* Defer actual window creation/UI build to popup tick to keep UI tree
       mutations out of widget callbacks and avoid context corruption. */
    return popup_queue_push_front(instance, &req);
}

bool ca_popup_is_active(const Ca_Instance *instance)
{
    return instance && instance->popup_active;
}

void ca_popup_clear_queue(Ca_Instance *instance)
{
    if (!instance) return;
    instance->popup_queue_count = 0;
}
