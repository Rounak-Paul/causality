// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "renderer.h"
#include "swapchain.h"
#include "pipeline.h"
#include "font.h"
#include "image.h"
#include "viewport.h"
#include "blur.h"

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

#ifdef CAUSALITY_VULKAN_VALIDATION
/* Prints every validation message to stderr — this is a temporary local
   diagnostic aid (gated behind CAUSALITY_VULKAN_VALIDATION, never built
   into release), not a permanent logging path. */
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_messenger_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user_data)
{
    (void)type; (void)user_data;
    const char *level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? "ERROR" :
                        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN"  :
                        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    ? "INFO"  : "VERBOSE";
    fprintf(stderr, "[vk-validation][%s] %s\n", level, data->pMessage);
    return VK_FALSE;
}
#endif

static bool create_vk_instance(Ca_Instance *inst, const char *app_name)
{
    uint32_t     glfw_ext_count = 0;
    const char **glfw_exts      = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    if (!glfw_exts || glfw_ext_count == 0) {
        fprintf(stderr, "[vk] GLFW returned no required instance extensions\n");
        return false;
    }

    Ca_DynArray extension_storage = CA_DYN_ARRAY_INIT(const char *);
    if (!ca_dyn_array_append(&extension_storage, glfw_exts, glfw_ext_count))
        return false;

    /* Check and opt-in to VK_KHR_portability_enumeration (required on macOS) */
    bool has_portability = false;
    uint32_t avail_ext_count = 0;
    VkResult vr = vkEnumerateInstanceExtensionProperties(NULL, &avail_ext_count, NULL);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[vk] extension count query failed: %d\n", vr);
        ca_dyn_array_destroy(&extension_storage);
        return false;
    }
    Ca_DynArray available_extension_storage = CA_DYN_ARRAY_INIT(VkExtensionProperties);
    if (!ca_dyn_array_resize(&available_extension_storage, avail_ext_count)) {
        ca_dyn_array_destroy(&extension_storage);
        return false;
    }
    VkExtensionProperties *avail_exts = available_extension_storage.data;
    vr = vkEnumerateInstanceExtensionProperties(NULL, &avail_ext_count, avail_exts);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        fprintf(stderr, "[vk] extension query failed: %d\n", vr);
        ca_dyn_array_destroy(&available_extension_storage);
        ca_dyn_array_destroy(&extension_storage);
        return false;
    }
    for (uint32_t i = 0; i < avail_ext_count; ++i) {
        if (strcmp(avail_exts[i].extensionName, "VK_KHR_portability_enumeration") == 0)
            has_portability = true;
    }
    ca_dyn_array_destroy(&available_extension_storage);

    if (has_portability) {
        const char *portability = "VK_KHR_portability_enumeration";
        if (!ca_dyn_array_push(&extension_storage, &portability)) {
            ca_dyn_array_destroy(&extension_storage);
            return false;
        }
    }

#ifdef CAUSALITY_VULKAN_VALIDATION
    const char *debug_utils = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (!ca_dyn_array_push(&extension_storage, &debug_utils)) {
        ca_dyn_array_destroy(&extension_storage);
        return false;
    }
#endif

    if (extension_storage.count > UINT32_MAX) {
        ca_dyn_array_destroy(&extension_storage);
        return false;
    }

    VkApplicationInfo app_info = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = app_name ? app_name : "causality",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        /* Vulkan 1.3 is required: the renderer uses dynamic rendering
           (VK_KHR_dynamic_rendering, core in 1.3) and synchronization2
           barriers (vkCmdPipelineBarrier2 / VkImageMemoryBarrier2). */
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
        .enabledExtensionCount   = (uint32_t)extension_storage.count,
        .ppEnabledExtensionNames = extension_storage.data,
    };

#ifdef CAUSALITY_VULKAN_VALIDATION
    /* VK_LAYER_KHRONOS_validation ships with the LunarG Vulkan SDK, not
       with the base loader/ICDs — only request it if it's actually
       installed, so Debug builds still start on machines without the SDK. */
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    bool has_validation_layer = false;
    uint32_t avail_layer_count = 0;
    vr = vkEnumerateInstanceLayerProperties(&avail_layer_count, NULL);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[vk] validation layer count query failed: %d\n", vr);
        ca_dyn_array_destroy(&extension_storage);
        return false;
    }
    Ca_DynArray available_layer_storage = CA_DYN_ARRAY_INIT(VkLayerProperties);
    if (!ca_dyn_array_resize(&available_layer_storage, avail_layer_count)) {
        ca_dyn_array_destroy(&extension_storage);
        return false;
    }
    VkLayerProperties *avail_layers = available_layer_storage.data;
    vr = vkEnumerateInstanceLayerProperties(&avail_layer_count, avail_layers);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        fprintf(stderr, "[vk] validation layer query failed: %d\n", vr);
        ca_dyn_array_destroy(&available_layer_storage);
        ca_dyn_array_destroy(&extension_storage);
        return false;
    }
    for (uint32_t i = 0; i < avail_layer_count; ++i) {
        if (strcmp(avail_layers[i].layerName, layers[0]) == 0)
            has_validation_layer = true;
    }
    ca_dyn_array_destroy(&available_layer_storage);

    if (has_validation_layer) {
        ci.enabledLayerCount   = 1;
        ci.ppEnabledLayerNames = layers;
    } else {
        fprintf(stderr, "[vk] VK_LAYER_KHRONOS_validation not found — "
                        "install the LunarG Vulkan SDK to enable validation; "
                        "continuing without it\n");
    }
#endif

    vr = vkCreateInstance(&ci, NULL, &inst->vk_instance);
    ca_dyn_array_destroy(&extension_storage);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkCreateInstance failed: %d\n", vr);
        return false;
    }

#ifdef CAUSALITY_VULKAN_VALIDATION
    PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            inst->vk_instance, "vkCreateDebugUtilsMessengerEXT");
    if (create_messenger) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_ci = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_messenger_callback,
        };
        create_messenger(inst->vk_instance, &messenger_ci, NULL, &inst->vk_debug_messenger);
    } else {
        fprintf(stderr, "[vk] vkCreateDebugUtilsMessengerEXT not available — "
                        "validation messages will only appear via the loader's default handler\n");
    }
#endif

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
        (VkPhysicalDevice *)CA_MALLOC(count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(inst->vk_instance, &count, devs);

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int best_score = -1;
    for (uint32_t i = 0; i < count; ++i) {
        int s = score_device(devs[i], prefer_dedicated);
        if (s > best_score) { best_score = s; best = devs[i]; }
    }
    CA_FREE(devs);

    if (best == VK_NULL_HANDLE) {
        fprintf(stderr, "[vk] no suitable device found\n");
        return false;
    }

    inst->vk_gpu = best;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(best, &props);
    printf("[vk] selected GPU: %s\n", props.deviceName);

    /* Store GPU info for debug overlay */
    strncpy(inst->gpu_name, props.deviceName, sizeof(inst->gpu_name) - 1);
    inst->gpu_name[sizeof(inst->gpu_name) - 1] = '\0';
    inst->gpu_type       = (uint32_t)props.deviceType;
    inst->vk_api_version = props.apiVersion;
    inst->driver_version = props.driverVersion;
    inst->vendor_id      = props.vendorID;

    /* Sum device-local heap sizes */
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(best, &mem_props);
    inst->gpu_heap_count = mem_props.memoryHeapCount;
    inst->gpu_heap_total = 0;
    for (uint32_t i = 0; i < mem_props.memoryHeapCount; ++i) {
        if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            inst->gpu_heap_total += mem_props.memoryHeaps[i].size;
    }

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

    VkPhysicalDeviceVulkan11Features available11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features available12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &available11,
    };
    VkPhysicalDeviceVulkan13Features available13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &available12,
    };
    VkPhysicalDeviceFeatures2 available = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &available13,
    };
    vkGetPhysicalDeviceFeatures2(inst->vk_gpu, &available);
    if (!available13.dynamicRendering ||
        !available13.synchronization2 ||
        !available13.shaderDemoteToHelperInvocation ||
        !available.features.samplerAnisotropy ||
        !available.features.fillModeNonSolid ||
        !available.features.multiDrawIndirect ||
        !available.features.fragmentStoresAndAtomics) {
        fprintf(stderr,
                "[vk] required Vulkan 1.3 features unavailable "
                "(dynamicRendering=%u, synchronization2=%u, shaderDemoteToHelperInvocation=%u, "
                "samplerAnisotropy=%u, fillModeNonSolid=%u, multiDrawIndirect=%u, "
                "fragmentStoresAndAtomics=%u)\n",
                available13.dynamicRendering,
                available13.synchronization2,
                available13.shaderDemoteToHelperInvocation,
                available.features.samplerAnisotropy,
                available.features.fillModeNonSolid,
                available.features.multiDrawIndirect,
                available.features.fragmentStoresAndAtomics);
        return false;
    }
    /* shaderDrawParameters (Vulkan 1.1, VK_KHR_shader_draw_parameters
       promoted to core) gives GLSL gl_BaseInstance/gl_BaseVertex/gl_DrawID
       via #extension GL_ARB_shader_draw_parameters — the foliage GPU
       cluster-cull pass (rg_foliage_cull_node.c) uses gl_BaseInstance to
       encode a per-draw metadata index in a compacted indirect draw.
       Required, not optional: confirmed supported on every Vulkan 1.1+
       driver including MoltenVK (no feature-support fallback path exists
       for the foliage draw, unlike bindless above). */
    if (!available11.shaderDrawParameters) {
        fprintf(stderr, "[vk] required Vulkan 1.1 feature unavailable (shaderDrawParameters=0)\n");
        return false;
    }

    /* Optional Vulkan 1.2 features: drawIndirectCount lets GPU-driven culling
       submit only the compacted visible draw count (unsupported on MoltenVK —
       callers must query ca_gpu_draw_indirect_count_supported and fall back). */
    inst->draw_indirect_count = available12.drawIndirectCount == VK_TRUE;

    /* Optional Vulkan 1.2 descriptor-indexing features: together these let a
       shader hold ONE large "bindless" sampler array binding (update-after-
       bind, partially-bound, indexed dynamically at runtime via a UBO/push-
       constant-supplied index) instead of one fixed binding per texture —
       needed once a material wants more textures bound than the device's
       maxPerStageDescriptorSamplers limit allows through fixed bindings
       (e.g. terrain's 8 layers x 5 PBR maps). All four bits must be present
       together; soft-fail (log + continue with bindless disabled) rather
       than refusing device creation, since only terrain currently needs
       this and it has a non-bindless fallback path. */
    bool bindless_bits =
        available12.descriptorIndexing == VK_TRUE &&
        available12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
        available12.descriptorBindingPartiallyBound == VK_TRUE &&
        available12.descriptorBindingVariableDescriptorCount == VK_TRUE &&
        available12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
        available12.runtimeDescriptorArray == VK_TRUE;
    inst->descriptor_indexing_supported = bindless_bits;
    printf("[vk] descriptor indexing (bindless) support: descriptorIndexing=%u "
           "shaderSampledImageArrayNonUniformIndexing=%u descriptorBindingPartiallyBound=%u "
           "descriptorBindingVariableDescriptorCount=%u descriptorBindingSampledImageUpdateAfterBind=%u "
           "runtimeDescriptorArray=%u -> %s\n",
           available12.descriptorIndexing,
           available12.shaderSampledImageArrayNonUniformIndexing,
           available12.descriptorBindingPartiallyBound,
           available12.descriptorBindingVariableDescriptorCount,
           available12.descriptorBindingSampledImageUpdateAfterBind,
           available12.runtimeDescriptorArray,
           bindless_bits ? "ENABLED" : "DISABLED");

    VkPhysicalDeviceVulkan11Features enabled11 = {
        .sType                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .shaderDrawParameters    = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features enabled12 = {
        .sType              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext              = &enabled11,
        .drawIndirectCount  = inst->draw_indirect_count ? VK_TRUE : VK_FALSE,
        .descriptorIndexing                              = bindless_bits ? VK_TRUE : VK_FALSE,
        .shaderSampledImageArrayNonUniformIndexing        = bindless_bits ? VK_TRUE : VK_FALSE,
        .descriptorBindingPartiallyBound                  = bindless_bits ? VK_TRUE : VK_FALSE,
        .descriptorBindingVariableDescriptorCount         = bindless_bits ? VK_TRUE : VK_FALSE,
        .descriptorBindingSampledImageUpdateAfterBind     = bindless_bits ? VK_TRUE : VK_FALSE,
        .runtimeDescriptorArray                           = bindless_bits ? VK_TRUE : VK_FALSE,
    };
    VkPhysicalDeviceVulkan13Features enabled13 = {
        .sType                           = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext                           = &enabled12,
        .dynamicRendering                = VK_TRUE,
        .synchronization2                = VK_TRUE,
        .shaderDemoteToHelperInvocation = VK_TRUE,
    };
    /* dualSrcBlend is no longer required: the text pipeline uses grayscale
       antialiasing with standard premultiplied-alpha blending.          */
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &enabled13,
        .features = {
            .samplerAnisotropy = VK_TRUE,
            .fillModeNonSolid  = VK_TRUE,
            .multiDrawIndirect = VK_TRUE,
            /* Required for imageAtomicOr on a storage image bound in the
               fragment stage — the standard fragment-shader voxelization
               technique (rasterize triangles, atomic-write occupancy). */
            .fragmentStoresAndAtomics = VK_TRUE,
        },
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

/* ---- VMA allocator ---- */

static bool create_vma_allocator(Ca_Instance *inst)
{
    VmaAllocatorCreateInfo ci = {
        .vulkanApiVersion = VK_API_VERSION_1_3,
        .physicalDevice   = inst->vk_gpu,
        .device           = inst->vk_device,
        .instance         = inst->vk_instance,
    };
    if (vmaCreateAllocator(&ci, &inst->vma) != VK_SUCCESS) {
        fprintf(stderr, "[vk] vmaCreateAllocator failed\n");
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
    if (!create_vma_allocator(inst))  return false;
    printf("[vk] renderer ready\n");
    return true;
}

void ca_renderer_shutdown(Ca_Instance *inst)
{
    if (inst->vk_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(inst->vk_device);

    /* Give the external renderer sharing this device (if any) a chance to
       flush its own deferred/frame-delayed resource teardown now that the
       device is confirmed idle — otherwise those resources are still
       destroyed by the time vkDestroyDevice below runs, tripping
       validation's "child objects not destroyed" check. */
    if (inst->gpu_predestroy_fn) inst->gpu_predestroy_fn(inst->gpu_predestroy_data);

    if (inst->vma != VK_NULL_HANDLE) {
        vmaDestroyAllocator(inst->vma);
        inst->vma = VK_NULL_HANDLE;
    }

    ca_image_pool_shutdown(inst);
    ca_blur_pipeline_destroy(inst);
    ca_image_pipeline_destroy(inst);
    ca_rect_pipeline_destroy(inst);
    ca_text_pipeline_destroy(inst);
    ca_ssbo_layout_destroy(inst);
    if (inst->font) {
        ca_font_destroy(inst, inst->font);
        free(inst->font);
        inst->font = NULL;
    }

    if (inst->cmd_pool   != VK_NULL_HANDLE)
        vkDestroyCommandPool(inst->vk_device, inst->cmd_pool, NULL);
    inst->cmd_pool = VK_NULL_HANDLE;

    if (inst->vk_device  != VK_NULL_HANDLE)
        vkDestroyDevice(inst->vk_device, NULL);
    inst->vk_device = VK_NULL_HANDLE;

#ifdef CAUSALITY_VULKAN_VALIDATION
    if (inst->vk_debug_messenger != VK_NULL_HANDLE && inst->vk_instance != VK_NULL_HANDLE) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                inst->vk_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_messenger)
            destroy_messenger(inst->vk_instance, inst->vk_debug_messenger, NULL);
    }
    inst->vk_debug_messenger = VK_NULL_HANDLE;
#endif

    if (inst->vk_instance != VK_NULL_HANDLE)
        vkDestroyInstance(inst->vk_instance, NULL);
    inst->vk_instance = VK_NULL_HANDLE;
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

    /* Create the shared SSBO layout + rect pipeline on the first window */
    if (inst->rect_pipeline.pipeline == VK_NULL_HANDLE) {
        if (inst->ssbo_desc_layout == VK_NULL_HANDLE) {
            if (!ca_ssbo_layout_create(inst))
                return false;
        }
        if (!ca_rect_pipeline_create(inst, win->sc.format))
            return false;
    }

    /* Load font atlas and text pipeline on the first window init */
    if (inst->text_pipeline.pipeline == VK_NULL_HANDLE) {
        inst->font = (Ca_Font *)calloc(1, sizeof(Ca_Font));
        bool font_ok = false;

        extern const unsigned char ca_embedded_font_data[];
        extern const unsigned int  ca_embedded_font_size;
        extern const unsigned char ca_embedded_font_bold_data[];
        extern const unsigned int  ca_embedded_font_bold_size;

        /* Resolve regular font data — file override or embedded */
        const unsigned char *regular_data = ca_embedded_font_data;
        unsigned int         regular_size = ca_embedded_font_size;
        unsigned char       *regular_buf  = NULL;
        const char          *regular_path = inst->font_path;

        if (regular_path[0] != '\0') {
            FILE *fp = fopen(regular_path, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long sz = ftell(fp);
                rewind(fp);
                regular_buf = (unsigned char *)malloc((size_t)sz);
                if (regular_buf) {
                    fread(regular_buf, 1, (size_t)sz, fp);
                    regular_data = regular_buf;
                    regular_size = (unsigned int)sz;
                }
                fclose(fp);
            }
        }

        /* Resolve bold font data — file override or embedded */
        const unsigned char *bold_data = ca_embedded_font_bold_data;
        unsigned int         bold_size = ca_embedded_font_bold_size;
        unsigned char       *bold_buf  = NULL;

        if (inst->bold_font_path[0] != '\0') {
            FILE *fp = fopen(inst->bold_font_path, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long sz = ftell(fp);
                rewind(fp);
                bold_buf = (unsigned char *)malloc((size_t)sz);
                if (bold_buf) {
                    fread(bold_buf, 1, (size_t)sz, fp);
                    bold_data = bold_buf;
                    bold_size = (unsigned int)sz;
                }
                fclose(fp);
            }
        }

        font_ok = ca_font_create_from_memory(
                    inst, win->glfw, inst->font,
                    regular_data, regular_size,
                    bold_data,    bold_size);

        free(regular_buf);
        free(bold_buf);
        if (!font_ok) {
            free(inst->font);
            inst->font = NULL;
        }
        if (!ca_text_pipeline_create(inst, win->sc.format))
            return false;
        if (inst->font)
            ca_text_pipeline_update_font(inst);

        /* Image descriptor pool — shares text pipeline's descriptor set layout */
        if (!ca_image_pool_init(inst))
            return false;

        /* Image pipeline — RGBA textured quads (shares text pipeline layout) */
        if (!ca_image_pipeline_create(inst, win->sc.format))
            return false;

        /* Backdrop blur pipeline */
        if (!ca_blur_pipeline_create(inst, win->sc.format))
            return false;
    }

    /* Create per-frame instance buffers if they don't exist yet.
       On the first window, the SSBO layout is created after the swapchain,
       so the swapchain_create path can't create them.  Fix up here. */
    for (uint32_t i = 0; i < CA_FRAMES_IN_FLIGHT; ++i) {
        Ca_Frame *frame = &win->sc.frames[i];
        if (frame->instance_buf == VK_NULL_HANDLE &&
            inst->ssbo_desc_layout != VK_NULL_HANDLE) {
            if (!ca_instance_buf_create(inst, frame))
                return false;
        }
    }

    /* Create per-window blur images if the blur pipeline is ready */
    if (inst->blur_h_pipeline != VK_NULL_HANDLE && win->blur_image == VK_NULL_HANDLE)
        ca_blur_window_create(inst, win, win->sc.extent.width, win->sc.extent.height, win->sc.format);

    return true;
}

void ca_renderer_window_shutdown(Ca_Instance *inst, Ca_Window *win)
{
    /* Destroy backdrop blur images */
    ca_blur_window_destroy(inst, win);

    /* Destroy viewport GPU resources before tearing down the swapchain */
    if (ca_pool_slot_count(&win->viewport_pool) > 0) {
        for (int i = 0; i < ca_pool_slot_count(&win->viewport_pool); ++i) {
            if (CA_POOL_AT(win->viewport_pool, Ca_Viewport, i)->in_use)
                ca_viewport_gpu_destroy(inst, CA_POOL_AT(win->viewport_pool, Ca_Viewport, i));
        }
    }

    ca_swapchain_destroy(inst, win);
    if (win->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(inst->vk_instance, win->surface, NULL);
        win->surface = VK_NULL_HANDLE;
    }
}

bool ca_renderer_window_resize(Ca_Instance *inst, Ca_Window *win, int w, int h)
{
    if (w == 0 || h == 0) return true; /* minimised */
    if (!inst || !win) return false;
    if (win->sc.swapchain != VK_NULL_HANDLE &&
        win->sc.extent.width  == (uint32_t)w &&
        win->sc.extent.height == (uint32_t)h) {
        return true;
    }
    vkDeviceWaitIdle(inst->vk_device);
    ca_swapchain_destroy(inst, win);
    if (!ca_swapchain_create(inst, win, (uint32_t)w, (uint32_t)h))
        return false;
    /* Resize backdrop blur images to match new swapchain extent */
    if (inst->blur_h_pipeline != VK_NULL_HANDLE)
        ca_blur_window_resize(inst, win, win->sc.extent.width, win->sc.extent.height, win->sc.format);
    return true;
}

/* ---- Frame ---- */

void ca_renderer_frame(Ca_Instance *inst)
{
    /* content_scale (display DPI) can change without a resize event on this
       codepath — the window moving to a different monitor, or the OS
       reporting a live scale-factor change. Everything else that reads
       content scale (viewport.c, widget.c) already re-queries GLFW per
       frame; the font atlas must too, or glyphs stay baked for whichever
       display was active at startup. Checked against the first live
       window since inst->font is a single instance-wide atlas shared by
       all windows. */
    if (inst && inst->font) {
        for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
            Ca_Window *scale_win = CA_POOL_AT(inst->windows, Ca_Window, i);
            if (!scale_win->in_use || !scale_win->glfw) continue;
            ca_font_refresh_content_scale(inst->font, scale_win->glfw);
            break;
        }
    }

    ca_profile_begin(inst, "Platform Font Upload");
    if (inst && inst->font)
        ca_font_flush_uploads(inst, inst->font);
    ca_profile_end(inst, "Platform Font Upload");

    for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
        Ca_Window *win = CA_POOL_AT(inst->windows, Ca_Window, i);
        if (!win->in_use) continue;

        /* Apply any deferred swapchain resize now that we are outside GLFW
           callbacks — safe to call vkDeviceWaitIdle here. */
        if (win->pending_swapchain_resize) {
            ca_profile_begin(inst, "Platform Swapchain Resize");
            win->pending_swapchain_resize = false;
            ca_renderer_window_resize(inst, win, win->pending_sc_w, win->pending_sc_h);
            ca_profile_end(inst, "Platform Swapchain Resize");
        }

        if (win->sc.swapchain == VK_NULL_HANDLE) continue;
        if (win->bg_render_fn || inst->default_bg_render_fn) win->needs_render = true;
        if (!win->needs_render) continue;
        win->needs_render = false;
        ca_profile_begin(inst, "Platform Swapchain");
        ca_swapchain_frame(inst, win);
        ca_profile_end(inst, "Platform Swapchain");
    }
}
