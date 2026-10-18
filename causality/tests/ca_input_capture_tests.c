// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

typedef struct GLFWwindow GLFWwindow;

#include "ca_internal.h"
#include "widget.h"
#include "node.h"
#include "paint.h"
#include "ca_reactive.h"
#include "../src/reactive/reactive.h"

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

    window.mouse_x = window.mouse_press_x = 50.0;
    window.mouse_y = window.mouse_press_y = 50.0;
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
    window.mouse_x = window.mouse_press_x = 160.0;
    window.mouse_y = window.mouse_press_y = 160.0;
    window.mouse_click_this_frame = true;
    ca_widget_input_pass(&window);
    CHECK(top_clicks == 1);
    CHECK(under_clicks == 0);

    overlay_node->desc.hidden = true;
    window.mouse_x = window.mouse_press_x = 50.0;
    window.mouse_y = window.mouse_press_y = 50.0;
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

/** Records the initial event of a gesture that was released before the input pass. */
static void record_drag_start(const Ca_DragEvent *event, void *user_data)
{
    *(Ca_DragEvent *)user_data = *event;
}

/** Batched motion and release must retain the press location used to choose a drag source. */
static bool test_batched_drag_press_location(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    window.instance = &instance;
    window.ui_scale = 1.0f;
    CHECK(ca_node_system_init(&window));
    CHECK(ca_node_root(&window));
    Ca_DragEvent recorded = {0};
    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)window.root);
    Ca_Div *drag = ca_div_begin(&(Ca_DivDesc){
        .on_drag_start = record_drag_start, .drag_data = &recorded,
    });
    ca_div_end();
    ca_div_end();
    ca_widget_ctx_leave();
    place_node(window.root, 0, 0, 200, 200);
    place_node((Ca_Node *)drag, 10, 10, 30, 30);
    window.mouse_press_x = window.mouse_press_y = 20;
    window.mouse_x = window.mouse_y = 140;
    window.mouse_click_this_frame = true;
    ca_widget_input_pass(&window);
    CHECK(recorded.window == &window);
    CHECK(recorded.start_x == 20 && recorded.start_y == 20);
    CHECK(recorded.x == 140 && recorded.y == 140);
    CHECK(!window.user_drag_active);
    ca_node_system_shutdown(&window);
    ca_widget_ctx_release_instance(&instance);
    return true;
}

/** Selection shortcuts and replacement apply to the complete selected UTF-8 range. */
static bool test_input_selection_replacement(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    window.instance = &instance;
    window.ui_scale = 1.0f;
    CHECK(ca_node_system_init(&window));
    CHECK(ca_node_root(&window));
    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)window.root);
    Ca_TextInput *input = ca_input(&(Ca_InputDesc){ .text = "previous path" });
    ca_div_end();
    ca_widget_ctx_leave();
    ca_input_focus(input);
    uint32_t ch = 'x';
    window.char_buf = &ch;
    window.char_count = 1;
    ca_widget_input_pass(&window);
    CHECK(strcmp(input->text, "x") == 0);
    int key = CA_KEY_A, mods = CA_MOD_SUPER;
    window.char_count = 0;
    window.key_buf = &key;
    window.key_mods_buf = &mods;
    window.key_count = 1;
    ca_widget_input_pass(&window);
    CHECK(input->sel_start == 0 && input->cursor == 1);
    key = CA_KEY_BACKSPACE;
    mods = 0;
    ca_widget_input_pass(&window);
    CHECK(input->text[0] == 0);
    window.char_buf = NULL;
    window.key_buf = window.key_mods_buf = NULL;
    ca_node_system_shutdown(&window);
    ca_widget_ctx_release_instance(&instance);
    return true;
}

/** Emits one retained child from an independently owned builder. */
static void nested_content_builder(Ca_Div *div, void *user_data)
{
    (void)div; (void)user_data;
    ca_div_begin(&(Ca_DivDesc){ .id = "nested-content" });
    ca_div_end();
}

/** Closing the outer reconcile frame must retain children emitted by its own builder. */
static bool test_builder_inside_reconciled_container(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    window.instance = &instance;
    window.ui_scale = 1.0f;
    CHECK(ca_node_system_init(&window));
    Ca_Node *root = ca_node_root(&window);
    CHECK(root);
    for (int round = 0; round < 3; ++round) {
        ca_widget_ctx_enter(&window);
        ca_reconcile_begin((Ca_Div *)root);
        Ca_Div *host = ca_div_begin(&(Ca_DivDesc){ .id = "nested-host" });
        CHECK(ca_div_set_builder(host, nested_content_builder, NULL));
        ca_div_end();
        ca_div_end();
        ca_widget_ctx_leave();
        CHECK(root->child_count == 1);
        CHECK(((Ca_Node *)host)->child_count == 1);
        CHECK(strcmp(((Ca_Node *)host)->children[0]->id, "nested-content") == 0);
        ca_div_invalidate(host);
        CHECK(((Ca_Node *)host)->child_count == 1);
    }
    ca_node_system_shutdown(&window);
    ca_reactive_shutdown(&instance);
    ca_widget_ctx_release_instance(&instance);
    return true;
}

/** Records whether a gesture started on a tab or on the unused strip. */
static void record_tab_drag(Ca_TabBar *tabs, int index, const Ca_DragEvent *event, void *user_data)
{
    (void)tabs; (void)event;
    *(int *)user_data = index;
}

/** Overflowing tab strips reveal focused tabs and consume horizontal wheel gestures. */
static bool test_tab_overflow_navigation(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    window.instance = &instance;
    window.ui_scale = 1.0f;
    CHECK(ca_node_system_init(&window));
    Ca_Node *root = ca_node_root(&window);
    CHECK(root);
    int drag_index = -99;
    const char *labels[] = { "First", "Second", "Third" };
    instance.stylesheet = ca_css_parse("tabbar { overflow: hidden; }");
    CHECK(instance.stylesheet);
    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)root);
    Ca_TabBar *tabs = ca_tabs(&(Ca_TabBarDesc){ .labels = labels, .count = 3,
        .on_drag_start = record_tab_drag, .drag_data = &drag_index });
    ca_div_end();
    ca_widget_ctx_leave();
    CHECK(tabs);
    place_node(root, 0, 0, 200, 100);
    place_node(tabs->node, 0, 0, 80, 25);
    tabs->node->desc.overflow_x = 1;
    tabs->node->content_w = 200;
    ca_tabs_set_active(tabs, 2);
    CHECK(tabs->node->scroll_x > 0);
    float offset = tabs->node->scroll_x;
    window.mouse_x = window.mouse_y = 10;
    window.scroll_this_frame = true;
    window.scroll_dx = 1;
    ca_widget_input_pass(&window);
    CHECK(tabs->node->scroll_x < offset);
    ca_tabs_set_active(tabs, 0);
    CHECK(tabs->node->scroll_x == 0);
    tabs->node->scroll_x = 20;
    ca_tabs_set_active(tabs, 0);
    CHECK(tabs->node->scroll_x == 0);
    place_node(tabs->tab_nodes[0], 0, 0, 25, 25);
    window.scroll_this_frame = false;
    window.mouse_click_this_frame = true;
    window.mouse_press_x = 70;
    window.mouse_press_y = 10;
    ca_widget_input_pass(&window);
    CHECK(drag_index == -1);
    window.mouse_press_x = 10;
    ca_widget_input_pass(&window);
    CHECK(drag_index == 0);
    ca_node_system_shutdown(&window);
    ca_css_destroy(instance.stylesheet);
    ca_widget_ctx_release_instance(&instance);
    return true;
}

/** Record which retained row supplied a dispatched context action. */
static void record_context_row(int item, void *user_data)
{
    int *row = user_data;
    *row += item + 1;
}

/** Reconciliation must refresh each emitted row's attachments before input dispatch. */
static bool test_reconciled_context_targets(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    window.instance = &instance;
    window.ui_scale = 1.0f;
    CHECK(ca_node_system_init(&window));
    Ca_Node *root = ca_node_root(&window);
    const char *keys[] = {"rectangle", "child-group", "another-rectangle"};
    int old_rows[3] = {0}, rows[3] = {0};
    const int orders[3][3] = {{0,1,2}, {0,1,2}, {2,0,1}};
    for (size_t round = 0; round < 3; ++round) {
        Ca_Tooltip *tooltips[3] = {0};
        ca_widget_ctx_enter(&window);
        ca_reconcile_begin((Ca_Div *)root);
        for (size_t i = 0; i < 3; ++i) {
            int index = orders[round][i];
            ca_btn_begin(&(Ca_BtnDesc){.id = keys[index], .text = keys[index]});
            ca_btn_end();
            const char *items[] = {"Toggle visibility"};
            ca_context_menu(&(Ca_CtxMenuDesc){.items = items, .item_count = 1,
                .on_select = record_context_row, .select_data = round ? &rows[index] : &old_rows[index]});
            tooltips[i] = ca_tooltip(&(Ca_TooltipDesc){.text = keys[index]});
        }
        ca_div_end();
        ca_widget_ctx_leave();
        place_node(root, 0, 0, 400, 500);
        for (size_t i = 0; i < 3; ++i) place_node(root->children[i], 0, (float)i * 40, 200, 30);
        for (size_t i = 0; i < 3; ++i) {
            int index = orders[round][i];
            Ca_Node *node = root->children[i];
            place_node(node, 0, (float)i * 40, 200, 30);
            Ca_CtxMenu *menu = NULL;
            for (size_t j = 0; j < ca_pool_slot_count(&window.ctxmenu_pool); ++j) {
                Ca_CtxMenu *candidate = CA_POOL_AT(window.ctxmenu_pool, Ca_CtxMenu, j);
                if (candidate->in_use && candidate->node == node) menu = candidate;
            }
            CHECK(menu && menu->select_data == (round ? &rows[index] : &old_rows[index]));
            CHECK(tooltips[i] && tooltips[i]->node == node);
            window.mouse_x = 10; window.mouse_y = node->y + 10;
            window.mouse_buttons[1] = true;
            ca_widget_input_pass(&window);
            CHECK(!menu->open);
            window.mouse_buttons[1] = false;
            ca_widget_input_pass(&window);
            CHECK(menu->open);
            window.mouse_click_this_frame = true;
            window.mouse_press_x = 20; window.mouse_press_y = (int)(menu->open_y + 15);
            ca_widget_input_pass(&window);
            window.mouse_click_this_frame = false;
            CHECK(!menu->open);
        }
    }
    for (size_t i = 0; i < 3; ++i) CHECK(old_rows[i] == 1 && rows[i] == 2);
    ca_node_system_shutdown(&window);
    ca_widget_ctx_release_instance(&instance);
    return true;
}

/** Route each wheel axis to its nested owner and pan horizontal-only headers. */
static bool test_nested_axis_scrolling(void)
{
    Ca_Instance instance = {0};
    Ca_Window window = {0};
    window.instance = &instance;
    window.ui_scale = 1;
    CHECK(ca_node_system_init(&window));
    Ca_Node *root = ca_node_root(&window);
    ca_widget_ctx_enter(&window);
    ca_reconcile_begin((Ca_Div *)root);
    Ca_Node *horizontal = (Ca_Node *)ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .id = "horizontal-owner"});
    Ca_Node *vertical = (Ca_Node *)ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL});
    ca_div_end(); ca_div_end(); ca_div_end();
    ca_widget_ctx_leave();
    place_node(root, 0, 0, 200, 150);
    place_node(horizontal, 0, 0, 200, 150);
    place_node(vertical, 0, 50, 200, 100);
    horizontal->desc.overflow_x = 2; horizontal->content_w = 2000;
    vertical->desc.overflow_y = 2; vertical->content_h = 400;
    Ca_Signal *offset = ca_get_scroll_x_signal(&window, "horizontal-owner");
    CHECK(offset && ca_signal_get_float(offset) == 0);
    CHECK(!ca_get_scroll_x_signal(&window, "missing"));
    window.mouse_x = 50; window.mouse_y = 80;
    window.scroll_this_frame = true; window.scroll_dx = -2;
    ca_widget_input_pass(&window);
    CHECK(horizontal->scroll_x > 0 && vertical->scroll_y == 0);
    CHECK(ca_signal_get_float(offset) == horizontal->scroll_x);
    float x = horizontal->scroll_x;
    window.scroll_dx = 0; window.scroll_dy = -2;
    ca_widget_input_pass(&window);
    CHECK(horizontal->scroll_x == x && vertical->scroll_y > 0);
    window.mouse_y = 10;
    ca_widget_input_pass(&window);
    CHECK(horizontal->scroll_x > x);
    window.scroll_dx = -1e9; window.scroll_dy = 0;
    ca_widget_input_pass(&window);
    CHECK(horizontal->scroll_x == 1800);
    window.scroll_dx = 1e9;
    ca_widget_input_pass(&window);
    CHECK(horizontal->scroll_x == 0 && ca_signal_get_float(offset) == 0);
    ca_scroll_wheel_x(&window, "horizontal-owner", -1);
    CHECK(horizontal->scroll_x > 0 && ca_signal_get_float(offset) == horizontal->scroll_x);
    ca_scroll_wheel_x(&window, "missing", -1);
    ca_node_system_shutdown(&window);
    ca_widget_ctx_release_instance(&instance);
    return true;
}

/** Drag snapshots retain their children and borders but exclude overlapping siblings and overlays. */
static bool test_isolated_drag_snapshot(void)
{
    Ca_DrawCmd cache[4] = {
        {.in_use=true,.type=CA_DRAW_RECT,.x=100,.y=50,.w=80,.h=40,.r=.1f,.a=1},
        {.in_use=true,.type=CA_DRAW_IMAGE,.x=110,.y=60,.w=30,.h=20,.r=.2f,.a=1,.image_index=7},
        {.in_use=true,.type=CA_DRAW_RECT,.x=100,.y=50,.w=80,.h=40,.r=.3f,.a=1,.border_width=1},
        {.in_use=true,.type=CA_DRAW_RECT,.x=100,.y=50,.w=80,.h=40,.r=1,.a=1,.z_index=20}
    };
    Ca_Window source = {.paint_cache=cache,.draw_cmds=cache,.draw_cmd_count=4};
    Ca_Node child = {.in_use=true,.window=&source,.cache_start=1,.cache_count=1};
    Ca_Node *children[] = {&child};
    Ca_Node dragged = {.in_use=true,.window=&source,.x=100,.y=50,.w=80,.h=40,
        .cache_count=1,.cache_post_start=2,.cache_post_count=1,.children=children,.child_count=1};
    child.parent=&dragged;
    Ca_Node root = {.draw_cmd_idx=0};
    Ca_Window target = {.root=&root,.draw_cmd_storage=CA_DYN_ARRAY_INIT(Ca_DrawCmd)};
    CHECK(ca_paint_snapshot(&target,&dragged,100,50,.5f));
    CHECK(target.paint_snapshot && root.draw_cmd_idx==-1 && target.draw_cmd_count==3);
    CHECK(target.draw_cmds[0].r==.1f && target.draw_cmds[1].r==.2f && target.draw_cmds[2].r==.3f);
    CHECK(target.draw_cmds[1].image_index==7 && target.draw_cmds[2].border_width==1);
    CHECK(target.draw_cmds[0].xf_a==.5f && target.draw_cmds[0].xf_tx==-50 && target.draw_cmds[0].xf_ty==-25);
    CHECK(cache[0].xf_active==false && source.draw_cmd_count==4);
    Ca_Instance instance={0};
    ca_paint_pass(&instance,&target);
    CHECK(target.draw_cmd_count==3);
    child.desc.hidden=true;
    CHECK(ca_paint_snapshot(&target,&dragged,100,50,1));
    CHECK(target.draw_cmd_count==2 && target.draw_cmds[1].r==.3f);
    ca_dyn_array_destroy(&target.draw_cmd_storage);
    return true;
}

typedef struct RightDragRecord { int starts, moves, ends; Ca_DragEvent last; } RightDragRecord;

/** Count thresholded start callbacks and record the originating button. */
static void right_drag_start(const Ca_DragEvent *event, void *data)
{
    RightDragRecord *record=data; ++record->starts; record->last=*event;
}
/** Count movement callbacks for an active right-button gesture. */
static void right_drag_move(const Ca_DragEvent *event, void *data)
{
    RightDragRecord *record=data; ++record->moves; record->last=*event;
}
/** Record the final release delta even when it returns to the press point. */
static void right_drag_end(const Ca_DragEvent *event, void *data)
{
    RightDragRecord *record=data; ++record->ends; record->last=*event;
}

/** Right clicks open menus only on release; drags suppress menus and preserve left-button behavior. */
static bool test_right_drag_context_release(void)
{
    Ca_Instance instance={0}; Ca_Window window={0}; RightDragRecord record={0};
    window.instance=&instance;window.ui_scale=1;
    CHECK(ca_node_system_init(&window));CHECK(ca_node_root(&window));
    ca_widget_ctx_enter(&window);ca_reconcile_begin((Ca_Div *)window.root);
    Ca_Node *node=(Ca_Node *)ca_div_begin(&(Ca_DivDesc){.drag_buttons=3,
        .on_drag_start=right_drag_start,.on_drag=right_drag_move,.on_drag_end=right_drag_end,.drag_data=&record});
    ca_div_end();const char *items[]={"Action"};
    ca_context_menu(&(Ca_CtxMenuDesc){.items=items,.item_count=1});
    ca_div_end();ca_widget_ctx_leave();
    place_node(window.root,0,0,300,300);place_node(node,0,0,200,200);
    Ca_CtxMenu *menu=NULL;
    for (uint32_t i=0;i<ca_pool_slot_count(&window.ctxmenu_pool);++i) {
        Ca_CtxMenu *candidate=CA_POOL_AT(window.ctxmenu_pool,Ca_CtxMenu,i);
        if (candidate->in_use&&candidate->node==node) menu=candidate;
    }
    CHECK(menu);
    window.mouse_x=window.mouse_y=20;window.mouse_buttons[1]=true;
    ca_widget_input_pass(&window);CHECK(!menu->open&&record.starts==0);
    window.mouse_x=22;ca_widget_input_pass(&window);CHECK(!menu->open&&record.starts==0);
    window.mouse_buttons[1]=false;ca_widget_input_pass(&window);
    CHECK(menu->open&&record.starts==0&&record.ends==0);
    window.mouse_x=window.mouse_y=20;window.mouse_buttons[1]=true;
    ca_widget_input_pass(&window);CHECK(!menu->open);
    window.mouse_x=50;ca_widget_input_pass(&window);
    CHECK(record.starts==1&&record.moves==1&&record.last.button==1&&record.last.start_x==20);
    window.mouse_x=20;ca_widget_input_pass(&window);
    window.mouse_buttons[1]=false;ca_widget_input_pass(&window);
    CHECK(record.ends==1&&!menu->open&&!window.user_drag_active&&!window.user_drag_node);
    window.mouse_click_this_frame=true;window.mouse_press_x=window.mouse_press_y=20;
    window.mouse_buttons[0]=true;ca_widget_input_pass(&window);
    CHECK(record.starts==2&&record.last.button==0);
    window.mouse_click_this_frame=false;window.mouse_buttons[0]=false;ca_widget_input_pass(&window);
    CHECK(record.ends==2);
    /* Press and release batched into one frame still open a context menu. */
    window.right_click_this_frame=true;window.right_press_x=window.right_press_y=20;
    ca_widget_input_pass(&window);CHECK(menu->open&&record.starts==2);
    window.right_click_this_frame=false;
    ca_node_system_shutdown(&window);ca_widget_ctx_release_instance(&instance);
    return true;
}

/** A viewport redraw invalidates its retained composite even when no other UI state changes. */
static bool test_viewport_redraw_invalidates_content(void)
{
    Ca_Node node={0}; Ca_Viewport viewport={.node=&node};
    ca_viewport_request_redraw(&viewport);
    CHECK(viewport.needs_redraw);
    CHECK(node.dirty&CA_DIRTY_CONTENT);
    CHECK(!(node.dirty&CA_DIRTY_LAYOUT));
    node.dirty=0;viewport.needs_redraw=false;
    ca_viewport_request_redraw(&viewport);
    CHECK(viewport.needs_redraw&&(node.dirty&CA_DIRTY_CONTENT));
    ca_viewport_request_redraw(NULL);
    return true;
}

int main(void)
{
    if (!test_viewport_redraw_invalidates_content()) return 1;
    if (!test_right_drag_context_release()) return 1;
    if (!test_isolated_drag_snapshot()) return 1;
    if (!test_nested_axis_scrolling()) return 1;
    if (!test_reconciled_context_targets()) return 1;
    if (!test_tab_overflow_navigation()) return 1;
    if (!test_builder_inside_reconciled_container()) return 1;
    if (!test_batched_drag_press_location()) return 1;
    if (!test_input_selection_replacement()) return 1;
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
