#include <causality.h>
#include <stdio.h>

/* Forward declarations */
static void on_toggle_click(Ca_Button *btn, void *user_data);
static void on_popup_click(Ca_Button *btn, void *user_data);

/* ---- CSS Stylesheet ---- */
static const char *g_css =
    ".body {"
    "  background: #1e1e2e;"
    "  padding: 16px;"
    "  gap: 10px;"
    "}"
    ""
    "h1 {"
    "  color: #cdd6f4;"
    "}"
    ""
    "hr {"
    "  background: #45475a;"
    "  height: 2px;"
    "}"
    ""
    ".status-text {"
    "  color: #a6adc8;"
    "  font-size: 14px;"
    "}"
    ""
    ".btn-row {"
    "  gap: 8px;"
    "}"
    ""
    ".btn-toggle {"
    "  width: 90px;"
    "  height: 28px;"
    "  background: #89b4fa;"
    "  border-radius: 6px;"
    "  color: #1e1e2e;"
    "}"
    ""
    ".btn-popup {"
    "  width: 100px;"
    "  height: 28px;"
    "  background: #cba6f7;"
    "  border-radius: 6px;"
    "  color: #1e1e2e;"
    "}"
    ""
    ".section-title {"
    "  color: #f5c2e7;"
    "  font-size: 15px;"
    "}"
    ""
    ".scroll-box {"
    "  height: 120px;"
    "  background: #313244;"
    "  border-radius: 8px;"
    "  padding: 8px;"
    "  gap: 4px;"
    "  overflow: scroll;"
    "}"
    ""
    ".scroll-item {"
    "  background: #45475a;"
    "  padding: 6px 10px;"
    "  border-radius: 4px;"
    "  color: #cdd6f4;"
    "}"
    ""
    ".card {"
    "  background: #313244;"
    "  border-radius: 8px;"
    "  padding: 12px;"
    "  gap: 6px;"
    "}"
    ""
    ".card-title {"
    "  color: #89dceb;"
    "  font-size: 14px;"
    "}"
    ""
    ".card-body {"
    "  color: #a6adc8;"
    "}"
    ""
    ".close-btn {"
    "  width: 60px;"
    "  height: 24px;"
    "  background: #f38ba8;"
    "  border-radius: 4px;"
    "  color: #1e1e2e;"
    "}"
;

/* ---- Context ---- */

typedef struct {
    Ca_Instance *instance;
} AppCtx;

/* Close-button callback */
static void on_close_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    ca_window_close((Ca_Window *)user_data);
}

/* ---- Event handlers ---- */

static void on_key(const Ca_Event *ev, void *user_data)
{
    (void)user_data;
    if (ev->key.action == CA_PRESS)
        printf("[event] key pressed: %d\n", ev->key.key);
}

static void on_resize(const Ca_Event *ev, void *user_data)
{
    (void)user_data;
    printf("[event] resize: %dx%d\n", ev->resize.width, ev->resize.height);
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
    printf("[thread] worker started\n");
    printf("[thread] worker done\n");
    return NULL;
}

/* ---- Button callbacks ---- */

static int g_toggle = 0;
static Ca_Label *g_status_label = NULL;

static void on_toggle_click(Ca_Button *btn, void *user_data)
{
    (void)user_data;
    g_toggle = !g_toggle;

    ca_label_set_text(g_status_label, g_toggle ? "Toggled ON!" : "Press the button!");
    ca_button_set_text(btn, g_toggle ? "Turn OFF" : "Turn ON");
    ca_button_set_background(btn, g_toggle
        ? ca_color(0.95f, 0.40f, 0.10f, 1.0f)
        : ca_color(0.20f, 0.50f, 0.90f, 1.0f));

    printf("[ui] toggle = %s\n", g_toggle ? "ON" : "OFF");
}

/* ---- Popup component ---- */

static void popup_ui(Ca_Window *popup)
{
    ca_ui_begin(popup, &(Ca_DivDesc){
        .direction  = CA_VERTICAL,
        .padding    = { 12, 12, 12, 12 },
        .gap        = 8,
        .background = ca_color(0.16f, 0.14f, 0.20f, 1.0f),
    });
        ca_h3(&(Ca_TextDesc){
            .text  = "Popup Window",
            .color = ca_color(0.90f, 0.85f, 1.0f, 1.0f),
        });

        ca_hr(NULL);

        ca_text(&(Ca_TextDesc){
            .text  = "Hello from the popup!",
            .color = ca_color(0.80f, 0.80f, 0.85f, 1.0f),
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
        &(Ca_WindowDesc){ .title = "Popup", .width = 360, .height = 240 });
    if (!popup) { fprintf(stderr, "Failed to create popup window\n"); return; }

    popup_ui(popup);
    printf("[ui] popup opened\n");
}

/* ---- Reusable components ---- */

static void feature_list(void)
{
    ca_list_begin(&(Ca_DivDesc){ .gap = 2 });
        ca_li_begin(NULL);
            ca_text(&(Ca_TextDesc){
                .text  = "- HTML-like nesting",
                .color = ca_color(0.75f, 0.75f, 0.80f, 1.0f),
            });
        ca_li_end();
        ca_li_begin(NULL);
            ca_text(&(Ca_TextDesc){
                .text  = "- Nestable buttons",
                .color = ca_color(0.75f, 0.75f, 0.80f, 1.0f),
            });
        ca_li_end();
        ca_li_begin(NULL);
            ca_text(&(Ca_TextDesc){
                .text  = "- Lists, headings, hr, spacer",
                .color = ca_color(0.75f, 0.75f, 0.80f, 1.0f),
            });
        ca_li_end();
    ca_list_end();
}

/* ---- Entry point ---- */

int main(void)
{
    Ca_InstanceDesc inst_desc = {
        .app_name             = "Sandbox",
        .prefer_dedicated_gpu = true,
        .font_path            = CA_FONT_PATH,
        .font_size_px         = 13.0f,
    };
    Ca_Instance *instance = ca_instance_create(&inst_desc);
    if (!instance) {
        fprintf(stderr, "Failed to create causality instance\n");
        return 1;
    }

    ca_event_set_handler(instance, CA_EVENT_KEY,           on_key,    NULL);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_RESIZE, on_resize, NULL);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_CLOSE,  on_close,  NULL);

    Ca_WindowDesc win_desc = { .title = "Sandbox", .width = 800, .height = 600 };
    Ca_Window *window = ca_window_create(instance, &win_desc);
    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        ca_instance_destroy(instance);
        return 1;
    }

    static AppCtx app_ctx;
    app_ctx.instance = instance;

    /* ---- Parse & apply CSS ---- */
    Ca_Stylesheet *sheet = ca_css_parse(g_css);
    if (sheet) ca_instance_set_stylesheet(instance, sheet);

    /* ---- Build the UI ---- */
    ca_window_set_scale(window, 1.0f);
    ca_ui_begin(window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style     = "body",
    });

        /* Heading */
        ca_h1(&(Ca_TextDesc){
            .text = "Causality Sandbox",
        });

        ca_hr(NULL);

        /* Status label */
        g_status_label = ca_text(&(Ca_TextDesc){
            .text  = "Press the button!",
            .style = "status-text",
        });

        /* Buttons in a horizontal row */
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "btn-row",
        });

            /* Toggle button */
            ca_btn(&(Ca_BtnDesc){
                .text     = "Turn ON",
                .style    = "btn-toggle",
                .on_click = on_toggle_click,
            });

            /* Open Popup button */
            ca_btn(&(Ca_BtnDesc){
                .text       = "Open Popup",
                .style      = "btn-popup",
                .on_click   = on_popup_click,
                .click_data = &app_ctx,
            });

        ca_div_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 4 });

        /* Section: Scrollable list */
        ca_text(&(Ca_TextDesc){
            .text  = "Scrollable List",
            .style = "section-title",
        });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "scroll-box",
        });
            for (int i = 0; i < 20; ++i) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Item %d — scroll me!", i + 1);
                ca_text(&(Ca_TextDesc){
                    .text  = buf,
                    .style = "scroll-item",
                });
            }
        ca_div_end();

        ca_spacer(&(Ca_SpacerDesc){ .height = 4 });

        /* Card component */
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style     = "card",
        });
            ca_text(&(Ca_TextDesc){
                .text  = "Info Card",
                .style = "card-title",
            });
            ca_text(&(Ca_TextDesc){
                .text  = "This card is styled entirely via CSS classes.",
                .style = "card-body",
            });
        ca_div_end();

        /* Feature list component */
        ca_h4(&(Ca_TextDesc){
            .text  = "Features",
            .style = "section-title",
        });
        feature_list();

    ca_ui_end();

    /* ---- Thread demo ---- */
    Ca_Thread *worker = ca_thread_create(worker_fn, NULL);
    if (worker) ca_thread_join(worker);

    return ca_instance_exec(instance);
}
