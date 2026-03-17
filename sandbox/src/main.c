#include <causality.h>
#include <stdio.h>

/* Forward declarations */
static void on_toggle_click(Ca_Button *btn, void *user_data);
static void on_popup_click(Ca_Button *btn, void *user_data);

/* ---- Context passed to the popup button callback ---- */

typedef struct {
    Ca_Instance *instance;
} PopupCtx;

/* Close-button callback: receives the Ca_Window* of the popup as user_data */
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

/* ---- Button callback ---- */

static int g_toggle = 0;

static void on_toggle_click(Ca_Button *btn, void *user_data)
{
    Ca_Label *lbl = (Ca_Label *)user_data;
    g_toggle = !g_toggle;

    ca_label_set_text(lbl, g_toggle ? "Toggled ON!" : "Press the button!");
    ca_button_set_text(btn, g_toggle ? "Turn OFF" : "Turn ON");
    ca_button_set_background(btn, g_toggle
        ? ca_color(0.95f, 0.40f, 0.10f, 1.0f)   /* orange */
        : ca_color(0.20f, 0.50f, 0.90f, 1.0f)); /* blue   */

    printf("[ui] toggle = %s\n", g_toggle ? "ON" : "OFF");
}

/* ---- Popup callback ---- */

static void on_popup_click(Ca_Button *btn, void *user_data)
{
    (void)btn;
    PopupCtx *ctx = (PopupCtx *)user_data;

    Ca_Window *popup = ca_window_create(ctx->instance,
        &(Ca_WindowDesc){ .title = "Popup", .width = 360, .height = 200 });
    if (!popup) { fprintf(stderr, "Failed to create popup window\n"); return; }

    Ca_Panel *root = ca_panel_create(popup, &(Ca_PanelDesc){
        .orientation    = CA_VERTICAL,
        .padding_top    = 24.0f, .padding_right  = 24.0f,
        .padding_bottom = 24.0f, .padding_left   = 24.0f,
        .gap            = 12.0f,
        .background     = ca_color(0.16f, 0.14f, 0.20f, 1.0f),
    });

    ca_label_add(root, &(Ca_LabelDesc){
        .text   = "Hello from the popup!",
        .height = 28.0f,
        .color  = ca_color(0.90f, 0.85f, 1.0f, 1.0f),
    });

    Ca_Button *close_btn = ca_button_add(root, &(Ca_ButtonDesc){
        .text       = "Close",
        .width      = 100.0f,
        .height     = 36.0f,
        .background = ca_color(0.55f, 0.25f, 0.75f, 1.0f),
        .text_color = ca_color(1.0f, 1.0f, 1.0f, 1.0f),
    });
    ca_button_on_click(close_btn, on_close_click, popup);

    printf("[ui] popup opened\n");
}

/* ---- Entry point ---- */

int main(void)
{
    Ca_InstanceDesc inst_desc = {
        .app_name             = "Sandbox",
        .prefer_dedicated_gpu = true,
        .font_path            = CA_FONT_PATH,
        .font_size_px         = 18.0f,
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

    /* ---- Build the UI ---- */

    /* Root panel: dark background, vertical layout, 24px padding, 16px gap */
    Ca_Panel *root = ca_panel_create(window, &(Ca_PanelDesc){
        .orientation    = CA_VERTICAL,
        .padding_top    = 24.0f, .padding_right  = 24.0f,
        .padding_bottom = 24.0f, .padding_left   = 24.0f,
        .gap            = 16.0f,
        .background     = ca_color(0.12f, 0.12f, 0.14f, 1.0f),
    });

    /* Status label */
    Ca_Label *lbl = ca_label_add(root, &(Ca_LabelDesc){
        .text   = "Press the button!",
        .height = 28.0f,
        .color  = ca_color(1.0f, 1.0f, 1.0f, 1.0f),
    });

    /* Toggle button */
    Ca_Button *btn = ca_button_add(root, &(Ca_ButtonDesc){
        .text          = "Turn ON",
        .width         = 160.0f,
        .height        = 40.0f,
        .background    = ca_color(0.20f, 0.50f, 0.90f, 1.0f),
        .text_color    = ca_color(1.0f, 1.0f, 1.0f, 1.0f),
        .corner_radius = 6.0f,
    });
    ca_button_on_click(btn, on_toggle_click, lbl);

    /* "Open Popup" button */
    static PopupCtx popup_ctx;
    popup_ctx.instance = instance;

    Ca_Button *popup_btn = ca_button_add(root, &(Ca_ButtonDesc){
        .text       = "Open Popup",
        .width      = 160.0f,
        .height     = 40.0f,
        .background = ca_color(0.45f, 0.20f, 0.65f, 1.0f),
        .text_color = ca_color(1.0f, 1.0f, 1.0f, 1.0f),
    });
    ca_button_on_click(popup_btn, on_popup_click, &popup_ctx);

    /* ---- Thread demo ---- */
    Ca_Thread *worker = ca_thread_create(worker_fn, NULL);
    if (worker) ca_thread_join(worker);

    return ca_instance_exec(instance);
}
