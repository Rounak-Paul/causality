#include <causality.h>
#include <stdio.h>
#include <string.h>

/* Forward declarations */
static void on_toggle_click(Ca_Button *btn, void *user_data);
static void on_popup_click(Ca_Button *btn, void *user_data);
static void on_check_change(Ca_Checkbox *cb, void *user_data);
static void on_slider_change(Ca_Slider *sl, void *user_data);
static void on_toggle_sw_change(Ca_Toggle *tg, void *user_data);
static void on_select_change(Ca_Select *sel, void *user_data);
static void on_tab_change(Ca_TabBar *tb, void *user_data);

/* ---- CSS Stylesheet ---- */
static const char *g_css =

    /* ---- Global / layout ---- */
    ".body {"
    "  background: #000000;"
    "  padding: 20px;"
    "  gap: 12px;"
    "}"

    "h1  { color: #cdd6f4; }"
    "h2  { color: #cdd6f4; }"
    "h3  { color: #bac2de; }"
    "h4  { color: #a6adc8; }"
    "hr  { background: #313244; height: 2px; }"

    "#hero-title { color: #89b4fa; }"

    ".muted   { color: #6c7086; font-size: 12px; }"
    ".section { color: #f5c2e7; font-size: 15px; }"
    ".body-text { color: #a6adc8; }"

    ".btn-row { gap: 8px; }"

    ".btn-toggle {"
    "  width: 100px; height: 30px;"
    "  background: #89b4fa;"
    "  border-radius: 6px;"
    "  color: #1e1e2e;"
    "  transition: background-color 0.35s;"
    "}"

    ".btn-popup {"
    "  width: 110px; height: 30px;"
    "  background: #cba6f7;"
    "  border-radius: 6px;"
    "  color: #1e1e2e;"
    "  transition: background-color 0.25s;"
    "}"

    ".btn-accent {"
    "  width: 120px; height: 30px;"
    "  background: #a6e3a1;"
    "  border-radius: 6px;"
    "  color: #1e1e2e;"
    "  transition: background-color 0.3s;"
    "}"

    ".close-btn {"
    "  width: 60px; height: 24px;"
    "  background: #f38ba8;"
    "  border-radius: 4px;"
    "  color: #1e1e2e;"
    "}"

    /* ---- Text input ---- */
    ".input-field {"
    "  width: 260px; height: 30px;"
    "  background: #313244;"
    "  border-radius: 6px;"
    "  color: #cdd6f4;"
    "  padding: 4px 8px;"
    "}"

    /* ---- Flex-wrap tag cloud ---- */
    ".tag-row {"
    "  flex-direction: row;"
    "  flex-wrap: wrap;"
    "  gap: 6px;"
    "}"
    ".tag {"
    "  background: #45475a;"
    "  border-radius: 12px;"
    "  padding: 4px 12px;"
    "  color: #cdd6f4;"
    "  font-size: 12px;"
    "}"

    /* ---- Scroll box ---- */
    ".scroll-box {"
    "  height: 110px;"
    "  background: #313244;"
    "  border-radius: 8px;"
    "  padding: 8px;"
    "  gap: 4px;"
    "  overflow: scroll;"
    "}"
    ".scroll-item {"
    "  background: #45475a;"
    "  padding: 6px 10px;"
    "  border-radius: 4px;"
    "  color: #cdd6f4;"
    "}"

    /* ---- Card ---- */
    ".card {"
    "  background: #313244;"
    "  border-radius: 8px;"
    "  padding: 12px;"
    "  gap: 6px;"
    "}"
    ".card-title { color: #89dceb; font-size: 14px; }"
    ".card-body  { color: #a6adc8; }"

    /* ---- Nested button (compound widget) ---- */
    ".btn-compound {"
    "  background: #313244;"
    "  border-radius: 8px;"
    "  padding: 10px 16px;"
    "  gap: 4px;"
    "  flex-direction: column;"
    "  transition: background-color 0.3s;"
    "}"
    ".btn-compound-label { color: #cdd6f4; font-size: 14px; }"
    ".btn-compound-hint  { color: #6c7086; font-size: 11px; }"

    /* ---- Splitter demo ---- */
    ".split-pane {"
    "  padding: 12px;"
    "  gap: 4px;"
    "}"
    ".split-left  { background: #1e1e2e; }"
    ".split-right { background: #181825; }"

    /* ---- Drag demo ---- */
    ".drag-box {"
    "  width: 120px; height: 60px;"
    "  background: #89b4fa;"
    "  border-radius: 8px;"
    "  color: #1e1e2e;"
    "  padding: 8px;"
    "}"

    /* ---- Absolute demo ---- */
    ".abs-container {"
    "  height: 120px;"
    "  background: #1e1e2e;"
    "  border-radius: 8px;"
    "}"
    ".abs-badge {"
    "  background: #f38ba8;"
    "  border-radius: 6px;"
    "  padding: 6px 14px;"
    "  color: #1e1e2e;"
    "}"
;

/* ---- Application context ---- */

typedef struct {
    Ca_Instance *instance;
} AppCtx;

/* ---- Event handlers ---- */

static void on_key(const Ca_Event *ev, void *user_data)
{
    (void)user_data;
    if (ev->key.action == CA_PRESS)
        printf("[event] key %d pressed\n", ev->key.key);
}

static void on_char(const Ca_Event *ev, void *user_data)
{
    (void)user_data;
    printf("[event] char U+%04X\n", ev->character.codepoint);
}

static void on_resize(const Ca_Event *ev, void *user_data)
{
    (void)user_data;
    printf("[event] resize %dx%d\n", ev->resize.width, ev->resize.height);
}

static void on_close(const Ca_Event *ev, void *user_data)
{
    (void)ev; (void)user_data;
    printf("[event] window closing\n");
}

/* ---- Worker thread ---- */

static void *worker_fn(void *data)
{
    (void)data;
    printf("[thread] background worker ran\n");
    return NULL;
}

/* ---- Widget state (global for simplicity) ---- */

static int       g_toggle       = 0;
static Ca_Label *g_status_label = NULL;
static Ca_Label *g_input_echo   = NULL;
static Ca_Label *g_slider_label = NULL;
static Ca_Label *g_check_label  = NULL;
static Ca_Label *g_toggle_label = NULL;
static Ca_Label *g_select_label = NULL;
static Ca_Label *g_tab_label    = NULL;
static float     g_progress_val = 0.35f;
static Ca_Label *g_drag_label   = NULL;
static Ca_Image *g_checker_img  = NULL;

/* ---- Drag demo callback ---- */

static void on_drag_move(const Ca_DragEvent *ev, void *user_data)
{
    (void)user_data;
    char buf[128];
    snprintf(buf, sizeof(buf), "Dragging: dx=%.0f  dy=%.0f", ev->dx, ev->dy);
    ca_label_set_text(g_drag_label, buf);
}

static void on_drag_end(const Ca_DragEvent *ev, void *user_data)
{
    (void)ev; (void)user_data;
    ca_label_set_text(g_drag_label, "Drag ended — grab the box again!");
}

/* ---- Button callbacks ---- */

static void on_toggle_click(Ca_Button *btn, void *user_data)
{
    (void)user_data;
    g_toggle = !g_toggle;
    ca_label_set_text(g_status_label, g_toggle ? "Toggled ON" : "Toggled OFF");
    ca_button_set_text(btn, g_toggle ? "Turn OFF" : "Turn ON");
    /* Triggers CSS transition on background-color */
    ca_button_set_background(btn, g_toggle
        ? ca_color(0.95f, 0.40f, 0.10f, 1.0f)   /* warm orange */
        : ca_color(0.54f, 0.71f, 0.98f, 1.0f));  /* #89b4fa     */
    printf("[ui] toggle = %s\n", g_toggle ? "ON" : "OFF");
}

static void on_compound_click(Ca_Button *btn, void *user_data)
{
    (void)btn; (void)user_data;
    printf("[ui] compound button clicked\n");
    ca_label_set_text(g_status_label, "Compound button clicked!");
}

static void on_accent_click(Ca_Button *btn, void *user_data)
{
    (void)btn; (void)user_data;
    printf("[ui] accent clicked\n");
    ca_label_set_text(g_status_label, "Accent button pressed");
}

/* ---- Text input callback ---- */

static void on_name_change(Ca_TextInput *input, void *user_data)
{
    (void)user_data;
    const char *text = ca_input_get_text(input);
    char buf[128];
    snprintf(buf, sizeof(buf), "You typed: %s", text);
    ca_label_set_text(g_input_echo, buf);
    printf("[input] \"%s\"\n", text);
}

/* ---- Popup component ---- */

static void on_check_change(Ca_Checkbox *cb, void *user_data)
{
    (void)user_data;
    ca_label_set_text(g_check_label, ca_checkbox_get(cb) ? "Checked!" : "Unchecked");
}

static void on_slider_change(Ca_Slider *sl, void *user_data)
{
    (void)user_data;
    char buf[64];
    snprintf(buf, sizeof(buf), "Value: %.1f", ca_slider_get(sl));
    ca_label_set_text(g_slider_label, buf);
}

static void on_toggle_sw_change(Ca_Toggle *tg, void *user_data)
{
    (void)user_data;
    ca_label_set_text(g_toggle_label, ca_toggle_get(tg) ? "ON" : "OFF");
}

static void on_select_change(Ca_Select *sel, void *user_data)
{
    (void)user_data;
    char buf[128];
    snprintf(buf, sizeof(buf), "Selected: index %d", ca_select_get(sel));
    ca_label_set_text(g_select_label, buf);
}

static void on_tab_change(Ca_TabBar *tb, void *user_data)
{
    (void)user_data;
    char buf[64];
    snprintf(buf, sizeof(buf), "Active tab: %d", ca_tabs_active(tb));
    ca_label_set_text(g_tab_label, buf);
}

static void on_close_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    ca_window_close((Ca_Window *)user_data);
}

static void popup_ui(Ca_Window *popup)
{
    ca_ui_begin(popup, &(Ca_DivDesc){
        .direction  = CA_VERTICAL,
        .padding    = { 16, 16, 16, 16 },
        .gap        = 10,
        .background = ca_color(0.16f, 0.14f, 0.20f, 1.0f),
    });
        ca_h3(&(Ca_TextDesc){
            .text  = "Popup Window",
            .color = ca_color(0.90f, 0.85f, 1.0f, 1.0f),
        });
        ca_hr(NULL);
        ca_text(&(Ca_TextDesc){
            .text  = "This is a separate window with its own UI tree.",
            .color = ca_color(0.80f, 0.80f, 0.85f, 1.0f),
        });
        ca_input(&(Ca_InputDesc){
            .placeholder = "Type in the popup...",
            .style       = "input-field",
        });
        ca_spacer(&(Ca_SpacerDesc){ .height = 4 });
        ca_btn(&(Ca_BtnDesc){
            .text       = "Close",
            .style      = "close-btn",
            .on_click   = on_close_click,
            .click_data = popup,
        });
    ca_ui_end();
}

static void on_popup_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    AppCtx *ctx = (AppCtx *)user_data;
    Ca_Window *popup = ca_window_create(ctx->instance,
        &(Ca_WindowDesc){ .title = "Popup", .width = 400, .height = 280 });
    if (!popup) return;
    popup_ui(popup);
    printf("[ui] popup opened\n");
}

/* ---- Entry point ---- */

int main(void)
{
    Ca_InstanceDesc inst_desc = {
        .app_name             = "Causality Sandbox",
        .prefer_dedicated_gpu = true,
        .font_size_px         = 14.0f,
    };
    Ca_Instance *instance = ca_instance_create(&inst_desc);
    if (!instance) {
        fprintf(stderr, "Failed to create causality instance\n");
        return 1;
    }

    /* Register event handlers */
    ca_event_set_handler(instance, CA_EVENT_KEY,           on_key,    NULL);
    ca_event_set_handler(instance, CA_EVENT_CHAR,          on_char,   NULL);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_RESIZE, on_resize, NULL);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_CLOSE,  on_close,  NULL);

    Ca_Window *window = ca_window_create(instance,
        &(Ca_WindowDesc){ .title = "Causality Sandbox", .width = 860, .height = 720 });
    if (!window) {
        ca_instance_destroy(instance);
        return 1;
    }

    static AppCtx app_ctx;
    app_ctx.instance = instance;

    /* Parse CSS (classes, IDs, transitions, flex-wrap) */
    Ca_Stylesheet *sheet = ca_css_parse(g_css);
    if (sheet) ca_instance_set_stylesheet(instance, sheet);

    /* Generate a procedural checkerboard image for the image demo */
    {
        enum { SZ = 64 };
        static uint8_t checker[SZ * SZ * 4];
        for (int y = 0; y < SZ; y++) {
            for (int x = 0; x < SZ; x++) {
                int idx = (y * SZ + x) * 4;
                int dark = ((x / 8) + (y / 8)) & 1;
                uint8_t c = dark ? 60 : 200;
                checker[idx + 0] = c;
                checker[idx + 1] = dark ? 80 : 180;
                checker[idx + 2] = dark ? 120 : 220;
                checker[idx + 3] = 255;
            }
        }
        g_checker_img = ca_image_create(instance, checker, SZ, SZ);
    }

    /* ================================================================
       BUILD THE UI
       ================================================================ */
    ca_window_set_scale(window, 1.0f);
    ca_ui_begin(window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "body",
    });

        /* ---- Hero heading (ID selector) ---- */
        ca_h1(&(Ca_TextDesc){
            .text = "Causality Sandbox",
            .id   = "hero-title",
        });
        ca_text(&(Ca_TextDesc){
            .text  = "A feature tour of the Causality UI library",
            .style = "muted",
        });

        ca_hr(NULL);

        /* ---- 1. Buttons & CSS Transitions ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Buttons & CSS Transitions", .style = "section" });

        g_status_label = ca_text(&(Ca_TextDesc){
            .text  = "Click a button to see transitions",
            .style = "body-text",
        });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "btn-row",
        });
            ca_btn(&(Ca_BtnDesc){
                .text     = "Turn ON",
                .style    = "btn-toggle",
                .on_click = on_toggle_click,
            });
            ca_btn(&(Ca_BtnDesc){
                .text       = "Open Popup",
                .style      = "btn-popup",
                .on_click   = on_popup_click,
                .click_data = &app_ctx,
            });
            ca_btn(&(Ca_BtnDesc){
                .text     = "Accent",
                .style    = "btn-accent",
                .on_click = on_accent_click,
            });
        ca_div_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 2 });

        /* ---- 2. Nested (compound) button ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Nested Button", .style = "section" });

        ca_btn_begin(&(Ca_BtnDesc){
            .style    = "btn-compound",
            .on_click = on_compound_click,
        });
            ca_text(&(Ca_TextDesc){
                .text  = "Compound Widget",
                .style = "btn-compound-label",
            });
            ca_text(&(Ca_TextDesc){
                .text  = "A button with multiple child elements",
                .style = "btn-compound-hint",
            });
        ca_btn_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 2 });

        /* ---- 3. Text Input ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Text Input", .style = "section" });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .gap       = 6,
        });
            ca_input(&(Ca_InputDesc){
                .placeholder = "Type your name here...",
                .style       = "input-field",
                .on_change   = on_name_change,
            });
            g_input_echo = ca_text(&(Ca_TextDesc){
                .text  = "You typed: ",
                .style = "body-text",
            });
        ca_div_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 2 });

        /* ---- 4. Flex-Wrap Tag Cloud ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Flex-Wrap Tags", .style = "section" });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "tag-row",
        });
        {
            const char *tags[] = {
                "Vulkan", "C11", "GLFW", "CSS", "Flexbox",
                "Transitions", "Focus", "Scroll", "HiDPI",
                "Reactive", "Text Input", "Multi-Window",
                "Splitter", "Drag", "Absolute Pos",
            };
            for (int i = 0; i < (int)(sizeof(tags)/sizeof(tags[0])); ++i)
                ca_text(&(Ca_TextDesc){ .text = tags[i], .style = "tag" });
        }
        ca_div_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 2 });

        /* ---- 5. Scrollable List ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Scrollable List", .style = "section" });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "scroll-box",
        });
            for (int i = 0; i < 20; ++i) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Row %d - scroll me!", i + 1);
                ca_text(&(Ca_TextDesc){ .text = buf, .style = "scroll-item" });
            }
        ca_div_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 2 });

        /* ---- 6. Card Component ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Card", .style = "section" });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "card",
        });
            ca_text(&(Ca_TextDesc){ .text = "Info Card", .style = "card-title" });
            ca_text(&(Ca_TextDesc){
                .text  = "Styled with CSS class selectors.",
                .style = "card-body",
            });
        ca_div_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 2 });

        /* ---- 7. Heading Hierarchy ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Heading Hierarchy", .style = "section" });

        ca_h1(&(Ca_TextDesc){ .text = "h1 Heading" });
        ca_h2(&(Ca_TextDesc){ .text = "h2 Heading" });
        ca_h3(&(Ca_TextDesc){ .text = "h3 Heading" });
        ca_h4(&(Ca_TextDesc){ .text = "h4 Heading" });
        ca_h5(&(Ca_TextDesc){ .text = "h5 Heading" });
        ca_h6(&(Ca_TextDesc){ .text = "h6 Heading" });

        /* ---- 8. Feature List ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Feature List", .style = "section" });

        ca_list_begin(&(Ca_DivDesc){ .gap = 2 });
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Declarative HTML-like API",   .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "CSS class, ID & type selectors", .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Flexbox layout with wrap",    .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "CSS transitions (ease-in-out)", .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Keyboard focus (Tab / Shift+Tab)", .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Text input with cursor & editing", .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Scroll containers",           .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Multi-window support",        .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Background threads",          .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Resizable split containers",  .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Drag interaction callbacks",   .style = "body-text" });
            ca_li_end();
            ca_li_begin(NULL);
                ca_text(&(Ca_TextDesc){ .text = "Absolute / fixed positioning", .style = "body-text" });
            ca_li_end();
        ca_list_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 4 });
        ca_text(&(Ca_TextDesc){
            .text  = "Press Tab to cycle focus between buttons and inputs.",
            .style = "muted",
        });

        ca_hr(NULL);

        /* ---- 9. Checkbox & Radio ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Checkbox & Radio", .style = "section" });

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .gap = 16 });
            ca_checkbox(&(Ca_CheckboxDesc){
                .text = "Enable feature",
                .checked = true,
                .on_change = on_check_change,
            });
            ca_checkbox(&(Ca_CheckboxDesc){
                .text = "Dark mode",
            });
        ca_div_end();
        g_check_label = ca_text(&(Ca_TextDesc){ .text = "Checked!", .style = "body-text" });

        ca_spacer(&(Ca_SpacerDesc){ .height = 4 });

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .gap = 12 });
            ca_radio(&(Ca_RadioDesc){ .text = "Small",  .group = 1, .value = 1 });
            ca_radio(&(Ca_RadioDesc){ .text = "Medium", .group = 1 });
            ca_radio(&(Ca_RadioDesc){ .text = "Large",  .group = 1 });
        ca_div_end();

        ca_hr(NULL);

        /* ---- 10. Slider ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Slider", .style = "section" });

        ca_slider(&(Ca_SliderDesc){
            .min = 0, .max = 100, .value = 42,
            .width = 240,
            .on_change = on_slider_change,
        });
        g_slider_label = ca_text(&(Ca_TextDesc){ .text = "Value: 42.0", .style = "body-text" });

        ca_hr(NULL);

        /* ---- 11. Toggle & Progress ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Toggle & Progress", .style = "section" });

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .gap = 12 });
            ca_toggle(&(Ca_ToggleDesc){
                .on = true,
                .on_change = on_toggle_sw_change,
            });
            g_toggle_label = ca_text(&(Ca_TextDesc){ .text = "ON", .style = "body-text" });
        ca_div_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 6 });
        ca_text(&(Ca_TextDesc){ .text = "Progress:", .style = "body-text" });
        ca_progress(&(Ca_ProgressDesc){
            .value = g_progress_val,
            .width = 240,
            .height = 8,
        });

        ca_hr(NULL);

        /* ---- 12. Select / Dropdown ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Select / Dropdown", .style = "section" });

        {
            static const char *opts[] = { "Apple", "Banana", "Cherry", "Date" };
            ca_select(&(Ca_SelectDesc){
                .options = opts,
                .option_count = 4,
                .selected = 0,
                .width = 160,
                .on_change = on_select_change,
            });
        }
        g_select_label = ca_text(&(Ca_TextDesc){ .text = "Selected: index 0", .style = "body-text" });

        ca_hr(NULL);

        /* ---- 13. Tab Bar ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Tab Bar", .style = "section" });

        {
            static const char *tabs[] = { "General", "Settings", "About" };
            ca_tabs(&(Ca_TabBarDesc){
                .labels = tabs,
                .count = 3,
                .active = 0,
                .on_change = on_tab_change,
            });
        }
        g_tab_label = ca_text(&(Ca_TextDesc){ .text = "Active tab: 0", .style = "body-text" });

        ca_hr(NULL);

        /* ---- 14. Tree View ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Tree View", .style = "section" });

        ca_tree_begin(&(Ca_DivDesc){ .gap = 1 });
            ca_tree_node_begin(&(Ca_TreeNodeDesc){ .text = "Root", .expanded = true });
                ca_tree_node_begin(&(Ca_TreeNodeDesc){ .text = "Child A", .expanded = false });
                    ca_tree_node_begin(&(Ca_TreeNodeDesc){ .text = "Leaf A.1", .expanded = false });
                    ca_tree_node_end();
                ca_tree_node_end();
                ca_tree_node_begin(&(Ca_TreeNodeDesc){ .text = "Child B", .expanded = true });
                    ca_tree_node_begin(&(Ca_TreeNodeDesc){ .text = "Leaf B.1", .expanded = false });
                    ca_tree_node_end();
                    ca_tree_node_begin(&(Ca_TreeNodeDesc){ .text = "Leaf B.2", .expanded = false });
                    ca_tree_node_end();
                ca_tree_node_end();
            ca_tree_node_end();
        ca_tree_end();

        ca_hr(NULL);

        /* ---- 15. Table ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Table", .style = "section" });

        {
            static const float col_w[] = { 80, 120, 60 };
            ca_table_begin(&(Ca_TableDesc){
                .column_count = 3,
                .column_widths = col_w,
            });
                /* Header */
                ca_table_row_begin(&(Ca_DivDesc){
                    .background = ca_color(0.2f, 0.2f, 0.28f, 1.0f),
                });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Name",  .color = ca_color(0.8f,0.85f,1,1) });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Email", .color = ca_color(0.8f,0.85f,1,1) });
                    ca_table_cell(&(Ca_TextDesc){ .text = "Age",   .color = ca_color(0.8f,0.85f,1,1) });
                ca_table_row_end();
                /* Rows */
                ca_table_row_begin(NULL);
                    ca_table_cell(&(Ca_TextDesc){ .text = "Alice", .style = "body-text" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "alice@ex.com", .style = "body-text" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "28",    .style = "body-text" });
                ca_table_row_end();
                ca_table_row_begin(NULL);
                    ca_table_cell(&(Ca_TextDesc){ .text = "Bob",   .style = "body-text" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "bob@ex.com", .style = "body-text" });
                    ca_table_cell(&(Ca_TextDesc){ .text = "34",    .style = "body-text" });
                ca_table_row_end();
            ca_table_end();
        }

        ca_hr(NULL);

        /* ---- 16. Tooltip ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Tooltip", .style = "section" });

        ca_btn(&(Ca_BtnDesc){
            .text = "Hover me",
            .style = "btn-accent",
        });
        ca_tooltip(&(Ca_TooltipDesc){
            .text = "This is a tooltip!",
        });

        ca_hr(NULL);

        /* ---- 17. Modal / Dialog ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Modal (hidden by default)", .style = "section" });
        ca_text(&(Ca_TextDesc){
            .text  = "Modals render an overlay - set visible=true to show.",
            .style = "muted",
        });

        ca_hr(NULL);

        /* ---- 18. Splitter (Resizable Split) ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Splitter", .style = "section" });
        ca_text(&(Ca_TextDesc){
            .text  = "Drag the divider bar to resize panes.",
            .style = "muted",
        });

        ca_split_begin(&(Ca_SplitDesc){
            .direction = CA_HORIZONTAL,
            .ratio     = 0.4f,
            .bar_size  = 2,
        });
            ca_div_begin(&(Ca_DivDesc){
                .direction = CA_VERTICAL,
                .style     = "split-pane split-left",
            });
                ca_text(&(Ca_TextDesc){
                    .text  = "Left Pane (40%)",
                    .color = ca_color(0.54f, 0.71f, 0.98f, 1),
                });
                ca_text(&(Ca_TextDesc){
                    .text  = "Resize me!",
                    .style = "body-text",
                });
            ca_div_end();
            ca_div_begin(&(Ca_DivDesc){
                .direction = CA_VERTICAL,
                .style     = "split-pane split-right",
            });
                ca_text(&(Ca_TextDesc){
                    .text  = "Right Pane (60%)",
                    .color = ca_color(0.80f, 0.65f, 0.97f, 1),
                });
                ca_text(&(Ca_TextDesc){
                    .text  = "Content fills remaining space.",
                    .style = "body-text",
                });
            ca_div_end();
        ca_split_end();

        ca_hr(NULL);

        /* ---- 19. Drag Interaction ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Drag Interaction", .style = "section" });
        ca_text(&(Ca_TextDesc){
            .text  = "Click and drag the blue box to see drag callbacks fire.",
            .style = "muted",
        });

        ca_div_begin(&(Ca_DivDesc){
            .direction    = CA_VERTICAL,
            .style        = "drag-box",
            .on_drag      = on_drag_move,
            .on_drag_end  = on_drag_end,
        });
            ca_text(&(Ca_TextDesc){
                .text  = "Drag me!",
                .color = ca_color(0.12f, 0.12f, 0.18f, 1),
            });
        ca_div_end();
        g_drag_label = ca_text(&(Ca_TextDesc){
            .text  = "Drag the box above...",
            .style = "body-text",
        });

        ca_hr(NULL);

        /* ---- 20. Absolute / Fixed Positioning ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Absolute Positioning", .style = "section" });
        ca_text(&(Ca_TextDesc){
            .text  = "The badge below is placed at pos_x=300, pos_y=20 inside its container.",
            .style = "muted",
        });

        ca_div_begin(&(Ca_DivDesc){
            .direction  = CA_VERTICAL,
            .style      = "abs-container",
        });
            ca_text(&(Ca_TextDesc){
                .text  = "Container (relative)",
                .color = ca_color(0.42f, 0.44f, 0.53f, 1),
            });
            /* This child is positioned absolutely inside the container */
            ca_div_begin(&(Ca_DivDesc){
                .position = CA_POSITION_ABSOLUTE,
                .pos_x    = 300,
                .pos_y    = 20,
                .width    = 140,
                .height   = 32,
                .style    = "abs-badge",
            });
                ca_text(&(Ca_TextDesc){
                    .text  = "Absolute badge",
                    .color = ca_color(0.12f, 0.12f, 0.18f, 1),
                });
            ca_div_end();
        ca_div_end();

        ca_hr(NULL);

        /* ---- 21. Borders & Box Shadows ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Borders & Box Shadows", .style = "section" });
        ca_text(&(Ca_TextDesc){
            .text  = "Cards with borders and drop shadows.",
            .style = "muted",
        });

        ca_div_begin(&(Ca_DivDesc){ .direction = CA_HORIZONTAL, .gap = 16 });

            ca_div_begin(&(Ca_DivDesc){
                .width         = 160, .height = 80,
                .background    = ca_color(0.19f, 0.20f, 0.27f, 1),
                .corner_radius = 8,
                .border_width  = 3,
                .border_color  = ca_color(0.54f, 0.71f, 0.98f, 1),
                .padding       = { 10, 10, 10, 10 },
                .direction     = CA_VERTICAL,
                .gap           = 4,
            });
                ca_text(&(Ca_TextDesc){
                    .text  = "Blue border",
                    .color = ca_color(0.54f, 0.71f, 0.98f, 1),
                });
            ca_div_end();

            ca_div_begin(&(Ca_DivDesc){
                .width            = 160, .height = 80,
                .background       = ca_color(0.19f, 0.20f, 0.27f, 1),
                .corner_radius    = 8,
                .shadow_offset_x  = 6,
                .shadow_offset_y  = 6,
                .shadow_blur      = 16,
                .shadow_color     = ca_color(0.0f, 0.0f, 0.0f, 0.7f),
                .padding          = { 10, 10, 10, 10 },
                .direction        = CA_VERTICAL,
                .gap              = 4,
            });
                ca_text(&(Ca_TextDesc){
                    .text  = "Drop shadow",
                    .color = ca_color(0.80f, 0.84f, 0.96f, 1),
                });
            ca_div_end();

            ca_div_begin(&(Ca_DivDesc){
                .width            = 160, .height = 80,
                .background       = ca_color(0.19f, 0.20f, 0.27f, 1),
                .corner_radius    = 8,
                .border_width     = 3,
                .border_color     = ca_color(0.80f, 0.65f, 0.97f, 1),
                .shadow_offset_x  = 5,
                .shadow_offset_y  = 5,
                .shadow_blur      = 12,
                .shadow_color     = ca_color(0.0f, 0.0f, 0.0f, 0.6f),
                .padding          = { 10, 10, 10, 10 },
                .direction        = CA_VERTICAL,
                .gap              = 4,
            });
                ca_text(&(Ca_TextDesc){
                    .text  = "Both!",
                    .color = ca_color(0.80f, 0.65f, 0.97f, 1),
                });
            ca_div_end();

        ca_div_end();

        ca_hr(NULL);

        /* ---- 22. Z-Index / Layering ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Z-Index Layering", .style = "section" });
        ca_text(&(Ca_TextDesc){
            .text  = "Overlapping boxes: green (z=2) on top, red (z=1) behind it.",
            .style = "muted",
        });

        ca_div_begin(&(Ca_DivDesc){
            .height     = 100,
            .background = ca_color(0.12f, 0.12f, 0.18f, 1),
            .corner_radius = 8,
        });
            ca_div_begin(&(Ca_DivDesc){
                .position   = CA_POSITION_ABSOLUTE,
                .pos_x      = 20, .pos_y = 10,
                .width      = 120, .height = 60,
                .background = ca_color(0.95f, 0.55f, 0.66f, 1),
                .corner_radius = 6,
                .z_index    = 1,
                .padding    = { 6, 8, 6, 8 },
            });
                ca_text(&(Ca_TextDesc){
                    .text  = "z=1 (red)",
                    .color = ca_color(0.12f, 0.12f, 0.18f, 1),
                });
            ca_div_end();

            ca_div_begin(&(Ca_DivDesc){
                .position   = CA_POSITION_ABSOLUTE,
                .pos_x      = 60, .pos_y = 30,
                .width      = 120, .height = 60,
                .background = ca_color(0.65f, 0.89f, 0.63f, 1),
                .corner_radius = 6,
                .z_index    = 2,
                .padding    = { 6, 8, 6, 8 },
            });
                ca_text(&(Ca_TextDesc){
                    .text  = "z=2 (green)",
                    .color = ca_color(0.12f, 0.12f, 0.18f, 1),
                });
            ca_div_end();
        ca_div_end();

        ca_hr(NULL);

        /* ---- 23. Multi-line Text Wrapping ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Multi-line Text Wrapping", .style = "section" });

        ca_div_begin(&(Ca_DivDesc){
            .width         = 300,
            .background    = ca_color(0.19f, 0.20f, 0.27f, 1),
            .corner_radius = 8,
            .padding       = { 12, 12, 12, 12 },
            .direction     = CA_VERTICAL,
            .gap           = 6,
        });
            ca_text(&(Ca_TextDesc){
                .text  = "This is a long paragraph that demonstrates multi-line "
                         "text wrapping. When the wrap flag is set to true, the "
                         "text will automatically break at word boundaries to fit "
                         "within the container width. Pretty neat!",
                .color = ca_color(0.80f, 0.84f, 0.96f, 1),
                .wrap  = true,
            });
        ca_div_end();

        ca_hr(NULL);

        /* ---- 24. Image Rendering ---- */
        ca_h3(&(Ca_TextDesc){ .text = "Image / Texture Rendering", .style = "section" });
        ca_text(&(Ca_TextDesc){
            .text  = "A procedurally generated 64x64 checkerboard texture.",
            .style = "muted",
        });

        ca_image(&(Ca_ImageDesc){
            .image  = g_checker_img,
            .width  = 128,
            .height = 128,
            .corner_radius = 8,
        });

    ca_ui_end();

    /* ---- Thread demo ---- */
    Ca_Thread *worker = ca_thread_create(worker_fn, NULL);
    if (worker) ca_thread_join(worker);

    return ca_instance_exec(instance);
}
