#include <causality.h>
#include <stdio.h>

static void *worker_fn(void *data)
{
    (void)data;
    printf("[thread] worker started\n");
    printf("[thread] worker done\n");
    return NULL;
}

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

int main(void)
{
    Ca_InstanceDesc inst_desc = { .app_name = "Sandbox" };
    Ca_Instance *instance = ca_instance_create(&inst_desc);
    if (!instance) {
        fprintf(stderr, "Failed to create causality instance\n");
        return 1;
    }

    ca_event_set_handler(instance, CA_EVENT_KEY,           on_key,    NULL);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_RESIZE, on_resize, NULL);
    ca_event_set_handler(instance, CA_EVENT_WINDOW_CLOSE,  on_close,  NULL);

    Ca_WindowDesc win_desc = { .title = "Sandbox", .width = 800, .height = 600 };
    if (!ca_window_create(instance, &win_desc)) {
        fprintf(stderr, "Failed to create window\n");
        ca_instance_destroy(instance);
        return 1;
    }

    Ca_Thread *worker = ca_thread_create(worker_fn, NULL);
    if (worker) ca_thread_join(worker);

    return ca_instance_exec(instance);
}

