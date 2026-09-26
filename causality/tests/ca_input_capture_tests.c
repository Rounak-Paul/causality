// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

typedef struct GLFWwindow GLFWwindow;

#include "ca_internal.h"
#include "widget.h"
#include "node.h"

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

/** Verifies reconciled text replacement leaves editing positions valid. */
static bool test_reconciled_input_text(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    Ca_Node root = {0};
    window.instance = &instance;
    window.ui_scale = 1.0f;
    window.root = &root;
    root.window = &window;
    root.in_use = true;
    CHECK(ca_pool_init(&window.node_pool, sizeof(Ca_Node), 4));
    CHECK(ca_pool_init(&window.input_pool, sizeof(Ca_TextInput), 4));

    uint32_t typed = 'x';
    window.char_buf = &typed;
    const char *values[] = {"previous commit message", "", "short", "\xc3\xa9"};
    Ca_TextInput *input = NULL;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        ca_widget_ctx_enter(&window);
        ca_reconcile_begin((Ca_Div *)&root);
        Ca_TextInput *next = ca_input(&(Ca_InputDesc){ .text = values[i] });
        ca_div_end();
        ca_widget_ctx_leave();
        CHECK(next && (!input || next == input));
        input = next;
        CHECK(input->cursor == (int)strlen(values[i]));
        CHECK(input->sel_start == -1);

        window.focused_node = input->node;
        window.char_buf[0] = 'x';
        window.char_count = 1;
        ca_widget_input_pass(&window);
        CHECK(input->cursor == (int)strlen(values[i]) + 1);
        CHECK(input->text[input->cursor - 1] == 'x');
        window.char_count = 0;
        input->sel_start = 0;
    }

    input->cursor = 0;
    input->sel_start = 1;
    char unchanged[CA_INPUT_TEXT_MAX];
    snprintf(unchanged, sizeof(unchanged), "%s", input->text);
    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)&root);
    CHECK(ca_input(&(Ca_InputDesc){ .text = unchanged }) == input);
    ca_div_end();
    ca_widget_ctx_leave();
    CHECK(input->cursor == 0 && input->sel_start == 1);

    ca_node_clear(&root);
    ca_pool_destroy(&window.input_pool, NULL, NULL);
    ca_pool_destroy(&window.node_pool, NULL, NULL);
    ca_widget_ctx_release_instance(&instance);
    return true;
}

/** Counts activations of the test button. */
static void count_click(Ca_Button *button, void *user_data)
{
    (void)button;
    ++*(int *)user_data;
}

/** Verifies app-owned keyboard routing never activates or Tab-focuses a button. */
static bool test_app_keyboard_blocks_button_activation(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    Ca_Node root = {0};
    window.instance = &instance;
    window.ui_scale = 1.0f;
    window.root = &root;
    root.window = &window;
    root.in_use = true;
    CHECK(ca_pool_init(&window.node_pool, sizeof(Ca_Node), 4));
    CHECK(ca_pool_init(&window.button_pool, sizeof(Ca_Button), 2));
    CHECK(ca_pool_init(&window.input_pool, sizeof(Ca_TextInput), 2));

    int clicks = 0;
    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)&root);
    Ca_Button *button = ca_btn_begin(&(Ca_BtnDesc){
        .text = "Open", .on_click = count_click, .click_data = &clicks });
    ca_btn_end();
    Ca_TextInput *input = ca_input(&(Ca_InputDesc){ .text = "" });
    ca_div_end();
    ca_widget_ctx_leave();
    CHECK(button && button->keyboard_focusable && input);

    int keys[2] = { CA_KEY_ENTER, CA_KEY_SPACE };
    int actions[2] = { 1, 1 };
    int mods[2] = { 0, 0 };
    window.key_buf = keys;
    window.key_action_buf = actions;
    window.key_mods_buf = mods;

    window.focused_node = button->node;
    window.key_count = 1;
    ca_widget_input_pass(&window);
    CHECK(clicks == 1);

    memset(window.key_consumed, 0, sizeof(window.key_consumed));
    ca_window_set_app_keyboard(&window, true);
    CHECK(window.focused_node == NULL);
    window.focused_node = button->node;
    window.key_count = 2;
    ca_widget_input_pass(&window);
    CHECK(clicks == 1);
    CHECK(!ca_window_key_consumed(&window, CA_KEY_ENTER));

    window.focused_node = NULL;
    keys[0] = CA_KEY_TAB;
    window.key_count = 1;
    ca_widget_input_pass(&window);
    CHECK(window.focused_node == NULL);
    CHECK(!ca_window_key_consumed(&window, CA_KEY_TAB));

    window.key_count = 0;
    window.focused_node = input->node;
    ca_window_set_app_keyboard(&window, true);
    CHECK(window.focused_node == input->node);
    ca_window_clear_focus(&window);
    CHECK(window.focused_node == NULL);

    ca_node_clear(&root);
    ca_pool_destroy(&window.input_pool, NULL, NULL);
    ca_pool_destroy(&window.button_pool, NULL, NULL);
    ca_pool_destroy(&window.node_pool, NULL, NULL);
    ca_widget_ctx_release_instance(&instance);
    return true;
}

/** Runs focused input ownership regression tests. */
int main(void)
{
    if (!test_reconciled_input_text()) return 1;
    if (!test_focused_button_capture()) return 1;
    if (!test_splitter_pointer_capture()) return 1;
    if (!test_exclusive_keyboard_capture()) return 1;
    if (!test_consumed_key_query()) return 1;
    if (!test_app_keyboard_blocks_button_activation()) return 1;
    puts("causality input capture tests passed");
    return 0;
}
