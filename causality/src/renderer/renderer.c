#include "renderer.h"
#include "swapchain.h"
#include "pipeline.h"
#include "font.h"

/* ---- Helpers ---- */

#define VK_CHECK(expr)                                                    \
    do {                                                                  \
        VkResult _r = (expr);                                             \
        if (_r != VK_SUCCESS) {                                           \
            fprintf(stderr, "[vk] %s failed: %d (%s:%d)\n",              \
                    #expr, _r, __FILE__, __LINE__);                       \
            return false;                                                 \
        }                                                                 \
    } while (0)

/* ---- VkInstance creation ---- */

static bool create_vk_instance(Ca_Instance *inst, const char *app_name)
{
    uint32_t     glfw_ext_count = 0;
    const char **glfw_exts      = glfwGetRequiredInstanceExtensions(&glfw_ext_count);

    /* Copy GLFW extensions, add portability enumeration if present */
    const char *extensions[32];
    uint32_t    ext_count = 0;
    for (uint32_t i = 0; i < glfw_ext_count; ++i)
        extensions[ext_count++] = glfw_exts[i];

    /* Check and opt-in to VK_KHR_portability_enumeration (required on macOS) */
    bool has_portability = false;
    uint32_t avail_ext_count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &avail_ext_count, NULL);
    VkExtensionProperties *avail_exts =
        (VkExtensionProperties *)malloc(avail_ext_count * sizeof(VkExtensionProperties));
    vkEnumerateInstanceExtensionProperties(NULL, &avail_ext_count, avail_exts);
    for (uint32_t i = 0; i < avail_ext_count; ++i) {
        if (strcmp(avail_exts[i].extensionName, "VK_KHR_portability_enumeration") == 0)
            has_portability = true;
    }
    free(avail_exts);

    if (has_portability)
        extensions[ext_count++] = "VK_KHR_portability_enumeration";

    VkApplicationInfo app_info = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = app_name ? app_name : "causality",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName        = "causality",
        .engineVersion      = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion         = VK_API_VERSION_1_3,
    };

    VkInstanceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags                   = has_portability
                                       ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
                                       : 0,
        .pApplicationInfo        = &app_info,
        .enabledExtensionCount   = ext_count,
        .ppEnabledExtensionNames = extensions,
    };

#ifdef CAUSALITY_VULKAN_VALIDATION
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    ci.enabledLayerCount   = 1;
    ci.ppEnabledLayerNames = layers;
#endif

    if (vkCreateInstance(&ci, NULL, &inst->vk_instance) != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkCreateInstance failed\n");
        return false;
    }
    return true;
}

/* ---- Physical device selection ---- */

static int score_device(VkPhysicalDevice dev, bool prefer_dedicated)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev, &props);

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        score += prefer_dedicated ? 1000 : 100;
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        score += 50;
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        score += 1;

    return score;
}

static bool select_physical_device(Ca_Instance *inst, bool prefer_dedicated)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst->vk_instance, &count, NULL);
    if (count == 0) {
        fprintf(stderr, "[vk] no Vulkan physical devices found\n");
        return false;
    }

    VkPhysicalDevice *devs =
        (VkPhysicalDevice *)malloc(count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(inst->vk_instance, &count, devs);

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int best_score = -1;
    for (uint32_t i = 0; i < count; ++i) {
        int s = score_device(devs[i], prefer_dedicated);
        if (s > best_score) { best_score = s; best = devs[i]; }
    }
    free(devs);

    if (best == VK_NULL_HANDLE) {
        fprintf(stderr, "[vk] no suitable device found\n");
        return false;
    }

    inst->vk_gpu = best;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(best, &props);
    printf("[vk] selected GPU: %s\n", props.deviceName);
    return true;
}

/* ---- Queue families ---- */

static bool find_queue_families(Ca_Instance *inst)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(inst->vk_gpu, &count, NULL);
    VkQueueFamilyProperties *props =
        (VkQueueFamilyProperties *)malloc(count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(inst->vk_gpu, &count, props);

    inst->gfx_family     = UINT32_MAX;
    inst->present_family = UINT32_MAX;

    for (uint32_t i = 0; i < count; ++i) {
        if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            inst->gfx_family == UINT32_MAX)
            inst->gfx_family = i;

        /* Present support checked against a temporary surface.
           We use glfwGetPhysicalDevicePresentationSupport for simplicity since
           a real surface may not exist yet. */
        if (glfwGetPhysicalDevicePresentationSupport(inst->vk_instance, inst->vk_gpu, i) &&
            inst->present_family == UINT32_MAX)
            inst->present_family = i;
    }
    free(props);

    if (inst->gfx_family == UINT32_MAX || inst->present_family == UINT32_MAX) {
        fprintf(stderr, "[vk] could not find required queue families\n");
        return false;
    }
    return true;
}

/* ---- Logical device ---- */

static bool create_logical_device(Ca_Instance *inst)
{
    uint32_t unique_families[2];
    uint32_t family_count = 0;
    unique_families[family_count++] = inst->gfx_family;
    if (inst->present_family != inst->gfx_family)
        unique_families[family_count++] = inst->present_family;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_cis[2];
    for (uint32_t i = 0; i < family_count; ++i) {
        queue_cis[i] = (VkDeviceQueueCreateInfo){
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = unique_families[i],
            .queueCount       = 1,
            .pQueuePriorities = &priority,
        };
    }

    /* Required device extensions */
    const char *dev_exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        /* dynamic rendering is core in Vulkan 1.3 */
    };

    /* Check and add portability subset if present */
    uint32_t avail_count = 0;
    const char *final_exts[8];
    uint32_t final_count = 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(dev_exts)/sizeof(dev_exts[0])); ++i)
        final_exts[final_count++] = dev_exts[i];

    vkEnumerateDeviceExtensionProperties(inst->vk_gpu, NULL, &avail_count, NULL);
    VkExtensionProperties *avail =
        (VkExtensionProperties *)malloc(avail_count * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(inst->vk_gpu, NULL, &avail_count, avail);
    for (uint32_t i = 0; i < avail_count; ++i) {
        if (strcmp(avail[i].extensionName, "VK_KHR_portability_subset") == 0)
            final_exts[final_count++] = "VK_KHR_portability_subset";
    }
    free(avail);

    /* Enable dynamic rendering via Vulkan 1.3 features */
    VkPhysicalDeviceDynamicRenderingFeatures dyn_feat = {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &dyn_feat,
    };

    VkDeviceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &features2,
        .queueCreateInfoCount    = family_count,
        .pQueueCreateInfos       = queue_cis,
        .enabledExtensionCount   = final_count,
        .ppEnabledExtensionNames = final_exts,
    };

    if (vkCreateDevice(inst->vk_gpu, &ci, NULL, &inst->vk_device) != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkCreateDevice failed\n");
        return false;
    }

    vkGetDeviceQueue(inst->vk_device, inst->gfx_family,     0, &inst->gfx_queue);
    vkGetDeviceQueue(inst->vk_device, inst->present_family, 0, &inst->present_queue);
    return true;
}

/* ---- Command pool ---- */

static bool create_command_pool(Ca_Instance *inst)
{
    VkCommandPoolCreateInfo ci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = inst->gfx_family,
    };
    if (vkCreateCommandPool(inst->vk_device, &ci, NULL, &inst->cmd_pool) != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkCreateCommandPool failed\n");
        return false;
    }
    return true;
}

/* ---- Public ---- */

bool ca_renderer_init(Ca_Instance *inst, const Ca_InstanceDesc *desc)
{
    if (!create_vk_instance(inst, desc ? desc->app_name : NULL))   return false;
    if (!select_physical_device(inst, desc && desc->prefer_dedicated_gpu)) return false;
    if (!find_queue_families(inst))   return false;
    if (!create_logical_device(inst)) return false;
    if (!create_command_pool(inst))   return false;
    printf("[vk] renderer ready\n");
    return true;
}

void ca_renderer_shutdown(Ca_Instance *inst)
{
    if (inst->vk_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(inst->vk_device);

    ca_rect_pipeline_destroy(inst);
    ca_text_pipeline_destroy(inst);
    if (inst->font) {
        ca_font_destroy(inst, inst->font);
        free(inst->font);
        inst->font = NULL;
    }

    if (inst->cmd_pool   != VK_NULL_HANDLE)
        vkDestroyCommandPool(inst->vk_device, inst->cmd_pool, NULL);
    if (inst->vk_device  != VK_NULL_HANDLE)
        vkDestroyDevice(inst->vk_device, NULL);
    if (inst->vk_instance != VK_NULL_HANDLE)
        vkDestroyInstance(inst->vk_instance, NULL);
}

/* ---- Per-window surface ---- */

bool ca_renderer_window_init(Ca_Instance *inst, Ca_Window *win)
{
    /* Surface */
    if (glfwCreateWindowSurface(inst->vk_instance, win->glfw, NULL, &win->surface)
            != VK_SUCCESS) {
        fprintf(stderr, "[vk] glfwCreateWindowSurface failed\n");
        return false;
    }

    int w, h;
    glfwGetFramebufferSize(win->glfw, &w, &h);
    if (!ca_swapchain_create(inst, win, (uint32_t)w, (uint32_t)h))
        return false;

    /* Create the shared rect pipeline on the first window (format comes
       from the swapchain just built; all windows use the same format). */
    if (inst->rect_pipeline.pipeline == VK_NULL_HANDLE) {
        if (!ca_rect_pipeline_create(inst, win->sc.format))
            return false;
    }

    /* Load font atlas and text pipeline on the first window init */
    if (inst->text_pipeline.pipeline == VK_NULL_HANDLE) {
        inst->font = (Ca_Font *)calloc(1, sizeof(Ca_Font));
        bool font_ok = false;
        if (inst->font_path[0] != '\0') {
            /* User-specified font file */
            font_ok = ca_font_create(inst, win->glfw,
                                     inst->font, inst->font_path,
                                     inst->font_size_px);
        }
        if (!font_ok) {
            /* Fall back to embedded Roboto Mono Nerd Font */
            extern const unsigned char ca_embedded_font_data[];
            extern const unsigned int  ca_embedded_font_size;
            font_ok = ca_font_create_from_memory(
                        inst, win->glfw, inst->font,
                        ca_embedded_font_data, ca_embedded_font_size,
                        inst->font_size_px);
        }
        if (!font_ok) {
            free(inst->font);
            inst->font = NULL;
        }
        if (!ca_text_pipeline_create(inst, win->sc.format))
            return false;
        if (inst->font)
            ca_text_pipeline_update_font(inst);
    }

    return true;
}

void ca_renderer_window_shutdown(Ca_Instance *inst, Ca_Window *win)
{
    ca_swapchain_destroy(inst, win);
    if (win->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(inst->vk_instance, win->surface, NULL);
        win->surface = VK_NULL_HANDLE;
    }
}

bool ca_renderer_window_resize(Ca_Instance *inst, Ca_Window *win, int w, int h)
{
    if (w == 0 || h == 0) return true; /* minimised */
    vkDeviceWaitIdle(inst->vk_device);
    ca_swapchain_destroy(inst, win);
    return ca_swapchain_create(inst, win, (uint32_t)w, (uint32_t)h);
}

/* ---- Frame ---- */

void ca_renderer_frame(Ca_Instance *inst)
{
    for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
        Ca_Window *win = &inst->windows[i];
        if (!win->in_use || win->sc.swapchain == VK_NULL_HANDLE) continue;
        if (!win->needs_render) continue;
        win->needs_render = false;
        ca_swapchain_frame(inst, win);
    }
}
