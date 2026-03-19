#include <causality.h>
#include <stdio.h>
#include <string.h>

/* Forward declarations */
static void on_toggle_click(Ca_Button *btn, void *user_data);
static void on_popup_click(Ca_Button *btn, void *user_data);

/* ---- CSS Stylesheet ---- */
static const char *g_css =

    /* ---- Global / layout ---- */
    ".body {"
    "  background: #010117;"
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
        .font_path            = CA_FONT_PATH,
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
        ca_list_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 4 });
        ca_text(&(Ca_TextDesc){
            .text  = "Press Tab to cycle focus between buttons and inputs.",
            .style = "muted",
        });

    ca_ui_end();

    /* ---- Thread demo ---- */
    Ca_Thread *worker = ca_thread_create(worker_fn, NULL);
    if (worker) ca_thread_join(worker);

    return ca_instance_exec(instance);
}
