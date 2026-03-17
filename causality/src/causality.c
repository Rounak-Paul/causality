#include "ca_internal.h"
#include "window.h"
#include "event.h"

#include <stdlib.h>

Ca_Instance *ca_instance_create(const Ca_InstanceDesc *desc)
{
    if (!ca_window_system_init())
        return NULL;

    Ca_Instance *inst = (Ca_Instance *)calloc(1, sizeof(Ca_Instance));
    if (!inst) {
        glfwTerminate();
        return NULL;
    }

    ca_event_init(inst);

    printf("[causality] instance created (%s)\n",
           desc && desc->app_name ? desc->app_name : "unnamed");
    return inst;
}

void ca_instance_destroy(Ca_Instance *instance)
{
    if (!instance) return;
    ca_window_system_shutdown(instance);
    ca_event_shutdown(instance);
    free(instance);
    printf("[causality] instance destroyed\n");
}

int ca_instance_exec(Ca_Instance *instance)
{
    while (ca_window_system_tick(instance)) { /* spin */ }
    ca_instance_destroy(instance);
    return 0;
}

