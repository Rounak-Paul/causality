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

/** Counts wheel callbacks received by a transparent overlay. */
static void count_scroll(double dx, double dy, void *user_data)
{
    (void)dx;
    (void)dy;
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

/** Places node n at the given window-space box. */
static void place_node(Ca_Node *n, float x, float y, float w, float h)
{
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;
}

/** Verifies a higher stacking layer occludes clicks and wheel from layers beneath it. */
static bool test_stacking_layer_occludes_pointer(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    Ca_Node root = {0};
    window.instance = &instance;
    window.ui_scale = 1.0f;
    window.root = &root;
    root.window = &window;
    root.in_use = true;
    CHECK(ca_pool_init(&window.node_pool, sizeof(Ca_Node), 8));
    CHECK(ca_pool_init(&window.button_pool, sizeof(Ca_Button), 4));

    int under_clicks = 0;
    int top_clicks = 0;
    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)&root);
    Ca_Div *scroller = ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL });
    Ca_Button *under = ca_btn_begin(&(Ca_BtnDesc){
        .text = "Under", .on_click = count_click, .click_data = &under_clicks });
    ca_btn_end();
    ca_div_end();
    Ca_Div *overlay = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL, .z_index = 40 });
    Ca_Button *top = ca_btn_begin(&(Ca_BtnDesc){
        .text = "Top", .on_click = count_click, .click_data = &top_clicks });
    ca_btn_end();
    ca_div_end();
    ca_div_end();
    ca_widget_ctx_leave();
    CHECK(scroller && under && overlay && top);

    Ca_Node *scroll_node = (Ca_Node *)scroller;
    Ca_Node *overlay_node = (Ca_Node *)overlay;
    place_node(&root, 0.0f, 0.0f, 200.0f, 200.0f);
    place_node(scroll_node, 0.0f, 0.0f, 200.0f, 200.0f);
    place_node(under->node, 0.0f, 0.0f, 100.0f, 100.0f);
    place_node(overlay_node, 0.0f, 0.0f, 200.0f, 200.0f);
    place_node(top->node, 150.0f, 150.0f, 20.0f, 20.0f);
    scroll_node->desc.overflow_y = 2;
    scroll_node->content_h = 1000.0f;

    window.mouse_x = 50.0;
    window.mouse_y = 50.0;
    window.mouse_click_this_frame = true;
    ca_widget_input_pass(&window);
    CHECK(under_clicks == 0);
    CHECK(top_clicks == 0);

    window.mouse_click_this_frame = false;
    window.scroll_this_frame = true;
    window.scroll_dy = -1.0;
    ca_widget_input_pass(&window);
    CHECK(scroll_node->scroll_y == 0.0f);

    window.scroll_this_frame = false;
    window.mouse_x = 160.0;
    window.mouse_y = 160.0;
    window.mouse_click_this_frame = true;
    ca_widget_input_pass(&window);
    CHECK(top_clicks == 1);
    CHECK(under_clicks == 0);

    overlay_node->desc.hidden = true;
    window.mouse_x = 50.0;
    window.mouse_y = 50.0;
    ca_widget_input_pass(&window);
    CHECK(under_clicks == 1);
    CHECK(top_clicks == 1);

    window.mouse_click_this_frame = false;
    window.scroll_this_frame = true;
    ca_widget_input_pass(&window);
    CHECK(scroll_node->scroll_y > 0.0f);

    ca_node_clear(&root);
    ca_pool_destroy(&window.button_pool, NULL, NULL);
    ca_pool_destroy(&window.node_pool, NULL, NULL);
    ca_widget_ctx_release_instance(&instance);
    return true;
}

/** Verifies a no-hover overlay only receives wheel through its descendants. */
static bool test_transparent_overlay_wheel_routing(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    Ca_Node root = {0};
    window.instance = &instance;
    window.ui_scale = 1.0f;
    window.root = &root;
    root.window = &window;
    root.in_use = true;
    CHECK(ca_pool_init(&window.node_pool, sizeof(Ca_Node), 8));

    int overlay_scrolls = 0;
    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)&root);
    Ca_Div *scroller = ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL });
    ca_div_end();
    Ca_Div *overlay = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .z_index = 5,
        .no_hover = true,
        .on_scroll = count_scroll,
        .scroll_data = &overlay_scrolls,
    });
    Ca_Div *overlay_child = ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL });
    ca_div_end();
    ca_div_end();
    ca_div_end();
    ca_widget_ctx_leave();
    CHECK(scroller && overlay && overlay_child);

    Ca_Node *scroll_node = (Ca_Node *)scroller;
    Ca_Node *overlay_node = (Ca_Node *)overlay;
    Ca_Node *overlay_child_node = (Ca_Node *)overlay_child;
    place_node(&root, 0.0f, 0.0f, 200.0f, 200.0f);
    place_node(scroll_node, 0.0f, 0.0f, 200.0f, 200.0f);
    place_node(overlay_node, 0.0f, 0.0f, 200.0f, 200.0f);
    place_node(overlay_child_node, 0.0f, 0.0f, 20.0f, 20.0f);
    scroll_node->desc.overflow_y = 2;
    scroll_node->content_h = 1000.0f;

    window.scroll_this_frame = true;
    window.scroll_dy = -1.0;
    window.mouse_x = 100.0;
    window.mouse_y = 100.0;
    ca_widget_input_pass(&window);
    CHECK(scroll_node->scroll_y > 0.0f);
    CHECK(overlay_scrolls == 0);

    const float scroll_y = scroll_node->scroll_y;
    window.mouse_x = 10.0;
    window.mouse_y = 10.0;
    ca_widget_input_pass(&window);
    CHECK(scroll_node->scroll_y == scroll_y);
    CHECK(overlay_scrolls == 1);

    overlay_node->desc.hidden = true;
    ca_widget_input_pass(&window);
    CHECK(scroll_node->scroll_y > scroll_y);
    CHECK(overlay_scrolls == 1);

    ca_node_clear(&root);
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
    if (!test_stacking_layer_occludes_pointer()) return 1;
    if (!test_transparent_overlay_wheel_routing()) return 1;
    puts("causality input capture tests passed");
    return 0;
}
