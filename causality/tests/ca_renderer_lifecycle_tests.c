typedef struct GLFWwindow GLFWwindow;
#include "ca_internal.h"
#include <stdio.h>

/** Terminal device failure must end ticks before accessing window or graphics state. */
int main(void)
{
    Ca_Instance instance = {0};
    instance.renderer_failed = true;
    for (unsigned i = 0; i < 3; i++) {
        if (ca_instance_tick(&instance)) {
            fputs("failed renderer continued ticking\n", stderr);
            return 1;
        }
    }
    if (ca_instance_tick(NULL) || !ca_instance_renderer_failed(&instance) || ca_instance_renderer_failed(NULL)) return 1;
    puts("renderer lifecycle tests passed");
    return 0;
}
