// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

typedef struct GLFWwindow GLFWwindow;

#include "ca_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                  \
                    __FILE__, __LINE__, #condition);                            \
            return false;                                                       \
        }                                                                       \
    } while (0)

/** Verifies button focus does not claim unrelated gameplay keyboard input. */
static bool test_focused_button_capture(void)
{
    Ca_Window window = {0};
    Ca_Node button = {0};
    button.in_use = true;
    button.window = &window;
    button.widget_type = CA_WIDGET_BUTTON;
    window.focused_node = &button;
    window.hovered_node = &button;

    bool pointer = false;
    bool keyboard = true;
    ca_window_input_capture(&window, &pointer, &keyboard);
    CHECK(pointer);
    CHECK(!keyboard);
    return true;
}

/** Verifies splitter panes pass pointer input while the divider captures it. */
static bool test_splitter_pointer_capture(void)
{
    Ca_Window window = {0};
    Ca_Node splitter = {0};
    Ca_Node viewport = {0};
    splitter.in_use = true;
    splitter.window = &window;
    splitter.widget_type = CA_WIDGET_SPLITTER;
    viewport.in_use = true;
    viewport.window = &window;
    viewport.widget_type = CA_WIDGET_VIEWPORT;
    viewport.parent = &splitter;

    window.hovered_node = &viewport;
    bool pointer = true;
    ca_window_input_capture(&window, &pointer, NULL);
    CHECK(!pointer);

    window.hovered_node = &splitter;
    pointer = false;
    ca_window_input_capture(&window, &pointer, NULL);
    CHECK(pointer);

    CHECK(ca_pool_init(&window.splitter_pool, sizeof(Ca_Splitter), 1));
    Ca_Splitter *active = ca_pool_acquire(&window.splitter_pool);
    CHECK(active);
    active->in_use = true;
    active->dragging = true;
    window.hovered_node = &viewport;
    pointer = false;
    ca_window_input_capture(&window, &pointer, NULL);
    CHECK(pointer);
    ca_pool_destroy(&window.splitter_pool, NULL, NULL);
    return true;
}

/** Verifies text editing and modal ancestry retain exclusive keyboard input. */
static bool test_exclusive_keyboard_capture(void)
{
    Ca_Window window = {0};
    Ca_Node input = {0};
    input.in_use = true;
    input.window = &window;
    input.widget_type = CA_WIDGET_TEXT_INPUT;
    window.focused_node = &input;

    bool keyboard = false;
    ca_window_input_capture(&window, NULL, &keyboard);
    CHECK(keyboard);

    Ca_Node modal = {0};
    Ca_Node button = {0};
    modal.in_use = true;
    modal.window = &window;
    modal.widget_type = CA_WIDGET_MODAL;
    button.in_use = true;
    button.window = &window;
    button.widget_type = CA_WIDGET_BUTTON;
    button.parent = &modal;
    window.focused_node = &button;
    keyboard = false;
    ca_window_input_capture(&window, NULL, &keyboard);
    CHECK(keyboard);
    return true;
}

/** Verifies consumed widget keys are reported independently by key code. */
static bool test_consumed_key_query(void)
{
    Ca_Window window = {0};
    window.key_consumed[CA_KEY_ENTER] = true;
    CHECK(ca_window_key_consumed(&window, CA_KEY_ENTER));
    CHECK(!ca_window_key_consumed(&window, CA_KEY_W));
    CHECK(!ca_window_key_consumed(&window, CA_KEY_UNKNOWN));
    CHECK(!ca_window_key_consumed(&window, CA_KEY_MENU + 1));
    return true;
}

/** Runs focused input ownership regression tests. */
int main(void)
{
    if (!test_focused_button_capture()) return 1;
    if (!test_splitter_pointer_capture()) return 1;
    if (!test_exclusive_keyboard_capture()) return 1;
    if (!test_consumed_key_query()) return 1;
    puts("causality input capture tests passed");
    return 0;
}
