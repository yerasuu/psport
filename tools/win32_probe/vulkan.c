#include "probe.h"

const char *vk_result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    default: return "VkResult";
    }
}

ProbeVulkan g_vk;

void set_outcome(Outcome *p_outcome, OutcomeState state, VkResult result, const char *sz_format, ...) {
    p_outcome->state = state;
    p_outcome->result = result;
    va_list args;
    va_start(args, sz_format);
    vsnprintf(p_outcome->sz_detail, sizeof(p_outcome->sz_detail), sz_format, args);
    va_end(args);
}

int has_device_extension(const Device *p_device, const char *sz_name) {
    for (uint32_t i = 0; i < p_device->c_extensions; ++i)
        if (!strcmp(p_device->p_extensions[i].extensionName, sz_name)) return 1;
    return 0;
}

HMODULE load_vulkan(void) {
    g_sz_step = "loading vulkan-1.dll";
    heading("Vulkan");
    HMODULE h_vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!h_vulkan) {
        say("Loader: vulkan-1.dll does not load (error %lu): no Vulkan driver is installed, or it is broken",
            GetLastError());
        return NULL;
    }
    wchar_t wsz_path[MAX_PATH];
    char sz_path[MAX_PATH * 3];
    if (!GetModuleFileNameW(h_vulkan, wsz_path, MAX_PATH)) wcscpy(wsz_path, L"?");
    to_utf8(wsz_path, sz_path, sizeof(sz_path));
    say("Loader: %s", sz_path);
    g_vk.vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(h_vulkan, "vkGetInstanceProcAddr");
    if (!g_vk.vkGetInstanceProcAddr) {
        say("Loader: vulkan-1.dll exports no vkGetInstanceProcAddr");
        return NULL;
    }
    g_vk.vkCreateInstance = (PFN_vkCreateInstance)g_vk.vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    g_vk.vkEnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)g_vk.vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    g_vk.vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)g_vk.vkGetInstanceProcAddr(
        NULL, "vkEnumerateInstanceExtensionProperties");
    g_vk.vkEnumerateInstanceLayerProperties =
        (PFN_vkEnumerateInstanceLayerProperties)g_vk.vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceLayerProperties");
    return g_vk.vkCreateInstance ? h_vulkan : NULL;
}

VkInstance create_instance(void) {
    g_sz_step = "creating the Vulkan instance";
    uint32_t version = VK_API_VERSION_1_0;
    if (g_vk.vkEnumerateInstanceVersion && g_vk.vkEnumerateInstanceVersion(&version) != VK_SUCCESS)
        version = VK_API_VERSION_1_0;
    say("Instance version: %u.%u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
        VK_API_VERSION_PATCH(version));
    if (g_vk.vkEnumerateInstanceLayerProperties) {
        uint32_t c_layers = 0;
        VkLayerProperties layers[64];
        c_layers = 64;
        const VkResult result = g_vk.vkEnumerateInstanceLayerProperties(&c_layers, layers);
        if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
            say("Layers available (implicit ones load by themselves: overlays, capture tools): %u", c_layers);
            for (uint32_t i = 0; i < c_layers; ++i)
                say("  %s %u.%u.%u: %s", layers[i].layerName, VK_API_VERSION_MAJOR(layers[i].specVersion),
                    VK_API_VERSION_MINOR(layers[i].specVersion), VK_API_VERSION_PATCH(layers[i].specVersion),
                    layers[i].description);
        } else {
            say("Layers: vkEnumerateInstanceLayerProperties -> %s (%d)", vk_result_name(result), result);
        }
    }
    if (version < VK_API_VERSION_1_1) {
        say("Instance: Vulkan 1.1 is needed for the probe (and 1.3 for psport); this loader offers 1.0 only");
        return VK_NULL_HANDLE;
    }
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "psport-windows-probe",
        .apiVersion = version < VK_API_VERSION_1_3 ? version : VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance h_instance = VK_NULL_HANDLE;
    const VkResult result = g_vk.vkCreateInstance(&create, NULL, &h_instance);
    if (result != VK_SUCCESS) {
        say("Instance: vkCreateInstance -> %s (%d)", vk_result_name(result), result);
        return VK_NULL_HANDLE;
    }
    int is_complete = 1;
#define PROBE_LOAD(name)                                                                                               \
    g_vk.name = (PFN_##name)g_vk.vkGetInstanceProcAddr(h_instance, #name);                                            \
    if (!g_vk.name) {                                                                                                  \
        say("Instance: no %s", #name);                                                                                 \
        is_complete = 0;                                                                                               \
    }
    PROBE_VULKAN_FUNCTIONS(PROBE_LOAD)
#undef PROBE_LOAD
    if (!is_complete) {
        if (g_vk.vkDestroyInstance) g_vk.vkDestroyInstance(h_instance, NULL);
        return VK_NULL_HANDLE;
    }
    return h_instance;
}

static void memory_flags_text(VkMemoryPropertyFlags flags, char *sz_out, size_t cb_out) {
    static const struct {
        VkMemoryPropertyFlags flag;
        const char *sz_name;
    } s_names[] = {
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "device-local"},
        {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "host-visible"},
        {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "host-coherent"},
        {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "host-cached"},
        {VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "lazily-allocated"},
        {VK_MEMORY_PROPERTY_PROTECTED_BIT, "protected"},
        {VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD, "device-coherent"},
        {VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD, "device-uncached"},
    };
    sz_out[0] = 0;
    size_t off_text = 0;
    for (size_t i = 0; i < sizeof(s_names) / sizeof(s_names[0]); ++i) {
        if (!(flags & s_names[i].flag)) continue;
        const int cb_written = snprintf(sz_out + off_text, cb_out - off_text, "%s%s", off_text ? " " : "",
                                        s_names[i].sz_name);
        if (cb_written < 0 || (size_t)cb_written >= cb_out - off_text) return;
        off_text += (size_t)cb_written;
    }
    if (!off_text) snprintf(sz_out, cb_out, "none");
}

static void external_features_text(VkExternalMemoryFeatureFlags features, char *sz_out, size_t cb_out) {
    snprintf(sz_out, cb_out, "%s%s%s%s", features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ? "exportable " : "",
             features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ? "importable " : "",
             features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ? "dedicated-only " : "",
             features ? "" : "unsupported");
}

VkExternalMemoryFeatureFlags external_buffer_features(const Device *p_device,
                                                             VkExternalMemoryHandleTypeFlagBits handle_type) {
    const VkPhysicalDeviceExternalBufferInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .handleType = handle_type,
    };
    VkExternalBufferProperties properties = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    g_vk.vkGetPhysicalDeviceExternalBufferProperties(p_device->h_physical, &info, &properties);
    return properties.externalMemoryProperties.externalMemoryFeatures;
}

static const char *const s_sz_relevant_extensions[] = {
    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME,
    VK_KHR_MAP_MEMORY_2_EXTENSION_NAME,
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME,
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
    VK_EXT_PCI_BUS_INFO_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
};

void describe_device(uint32_t i_device, Device *p_device) {
    g_sz_step = "reading a device's properties";
    const VkPhysicalDeviceProperties *p_properties = &p_device->properties;
    static const char *const s_sz_types[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    const unsigned i_type = p_properties->deviceType <= VK_PHYSICAL_DEVICE_TYPE_CPU ? p_properties->deviceType : 0;
    heading("Vulkan device");
    say("Device %u: %s", i_device, p_properties->deviceName);
    say("Vendor id: %#06x, device id: %#06x, type: %s", p_properties->vendorID, p_properties->deviceID,
        s_sz_types[i_type]);
    say("Device Vulkan version: %u.%u.%u, raw driver version: %#010x", VK_API_VERSION_MAJOR(p_properties->apiVersion),
        VK_API_VERSION_MINOR(p_properties->apiVersion), VK_API_VERSION_PATCH(p_properties->apiVersion),
        p_properties->driverVersion);
    const int is_12 = p_properties->apiVersion >= VK_API_VERSION_1_2;
    const int is_13 = p_properties->apiVersion >= VK_API_VERSION_1_3;
    const int has_host_import = has_device_extension(p_device, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    const int has_placed = has_device_extension(p_device, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);

    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT placed = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
    VkPhysicalDeviceMaintenance3Properties maintenance3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &maintenance3};
    void **pp_next = &maintenance3.pNext;
    if (is_12 || has_device_extension(p_device, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
        *pp_next = &driver;
        pp_next = &driver.pNext;
    }
    if (has_host_import) {
        *pp_next = &host;
        pp_next = &host.pNext;
    }
    if (has_placed) *pp_next = &placed;
    g_vk.vkGetPhysicalDeviceProperties2(p_device->h_physical, &properties2);
    if (driver.driverID) {
        say("Driver: %s (%s), id %d, conformance %u.%u.%u.%u", driver.driverName, driver.driverInfo, driver.driverID,
            driver.conformanceVersion.major, driver.conformanceVersion.minor, driver.conformanceVersion.subminor,
            driver.conformanceVersion.patch);
    } else {
        say("Driver: no VkPhysicalDeviceDriverProperties (Vulkan 1.2 or VK_KHR_driver_properties)");
    }
    p_device->cb_import_alignment = has_host_import ? host.minImportedHostPointerAlignment : 0;
    say("Max memory allocation: %llu MiB, max allocations: %u, max storage buffer range: %u MiB",
        (unsigned long long)(maintenance3.maxMemoryAllocationSize >> 20), p_properties->limits.maxMemoryAllocationCount,
        p_properties->limits.maxStorageBufferRange >> 20);

    say("Extensions psport looks at:");
    for (size_t i = 0; i < sizeof(s_sz_relevant_extensions) / sizeof(s_sz_relevant_extensions[0]); ++i)
        say("  %-44s %s", s_sz_relevant_extensions[i],
            has_device_extension(p_device, s_sz_relevant_extensions[i]) ? "yes" : "no");
    if (has_host_import) {
        say("  minImportedHostPointerAlignment: %llu (allocation granularity %lu%s)",
            (unsigned long long)host.minImportedHostPointerAlignment, g_cb_granularity,
            host.minImportedHostPointerAlignment > g_cb_granularity ? ": larger, views may not be aligned enough"
                                                                    : "");
    }
    if (has_placed)
        say("  minPlacedMemoryMapAlignment: %llu", (unsigned long long)placed.minPlacedMemoryMapAlignment);

    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features features13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    pp_next = &features.pNext;
    if (is_12) {
        *pp_next = &features12;
        pp_next = &features12.pNext;
    }
    if (is_13) {
        *pp_next = &features13;
        pp_next = &features13.pNext;
    }
    if (has_placed) *pp_next = &placed_features;
    g_vk.vkGetPhysicalDeviceFeatures2(p_device->h_physical, &features);
    say("Features:");
    say("  sparseBinding %u, sparseResidencyBuffer %u, sparseResidencyAliased %u, shaderInt64 %u, "
        "geometryShader %u",
        features.features.sparseBinding, features.features.sparseResidencyBuffer,
        features.features.sparseResidencyAliased, features.features.shaderInt64, features.features.geometryShader);
    if (is_12) {
        say("  bufferDeviceAddress %u, descriptorIndexing %u, runtimeDescriptorArray %u, "
            "descriptorBindingPartiallyBound %u, descriptorBindingVariableDescriptorCount %u",
            features12.bufferDeviceAddress, features12.descriptorIndexing, features12.runtimeDescriptorArray,
            features12.descriptorBindingPartiallyBound, features12.descriptorBindingVariableDescriptorCount);
        say("  shaderSampledImageArrayNonUniformIndexing %u, descriptorBindingSampledImageUpdateAfterBind %u, "
            "descriptorBindingStorageBufferUpdateAfterBind %u, timelineSemaphore %u, scalarBlockLayout %u",
            features12.shaderSampledImageArrayNonUniformIndexing,
            features12.descriptorBindingSampledImageUpdateAfterBind,
            features12.descriptorBindingStorageBufferUpdateAfterBind, features12.timelineSemaphore,
            features12.scalarBlockLayout);
    } else {
        say("  Vulkan 1.2 features: not queried (device below Vulkan 1.2)");
    }
    if (is_13) {
        say("  synchronization2 %u, dynamicRendering %u, maintenance4 %u", features13.synchronization2,
            features13.dynamicRendering, features13.maintenance4);
    } else {
        say("  Vulkan 1.3 features: not queried (device below Vulkan 1.3, which psport needs)");
    }
    if (has_placed) {
        say("  memoryMapPlaced %u, memoryMapRangePlaced %u, memoryUnmapReserve %u", placed_features.memoryMapPlaced,
            placed_features.memoryMapRangePlaced, placed_features.memoryUnmapReserve);
    }

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    const int has_budget = has_device_extension(p_device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    VkPhysicalDeviceMemoryProperties2 memory2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                                 .pNext = has_budget ? &budget : NULL};
    g_vk.vkGetPhysicalDeviceMemoryProperties2(p_device->h_physical, &memory2);
    p_device->memory = memory2.memoryProperties;
    const VkPhysicalDeviceMemoryProperties *p_memory = &p_device->memory;
    say("Memory heaps:");
    for (uint32_t i = 0; i < p_memory->memoryHeapCount; ++i) {
        const VkMemoryHeap *p_heap = &p_memory->memoryHeaps[i];
        if (p_heap->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && p_heap->size > p_device->cb_local_heap)
            p_device->cb_local_heap = p_heap->size;
        if (has_budget) {
            say("  heap %u: %llu MiB%s%s, budget %llu MiB, used %llu MiB", i, (unsigned long long)(p_heap->size >> 20),
                p_heap->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? ", device-local" : "",
                p_heap->flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT ? ", multi-instance" : "",
                (unsigned long long)(budget.heapBudget[i] >> 20), (unsigned long long)(budget.heapUsage[i] >> 20));
        } else {
            say("  heap %u: %llu MiB%s%s", i, (unsigned long long)(p_heap->size >> 20),
                p_heap->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? ", device-local" : "",
                p_heap->flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT ? ", multi-instance" : "");
        }
    }
    say("Memory types:");
    for (uint32_t i = 0; i < p_memory->memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = p_memory->memoryTypes[i].propertyFlags;
        const uint32_t i_heap = p_memory->memoryTypes[i].heapIndex;
        char sz_flags[160];
        memory_flags_text(flags, sz_flags, sizeof(sz_flags));
        say("  type %u: heap %u, %s", i, i_heap, sz_flags);
        const VkMemoryPropertyFlags rebar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        if ((flags & rebar) == rebar && p_memory->memoryHeaps[i_heap].size > p_device->cb_rebar)
            p_device->cb_rebar = p_memory->memoryHeaps[i_heap].size;
    }
    say("ReBAR (host-visible device-local heap, as psport measures it): %llu MiB",
        (unsigned long long)(p_device->cb_rebar >> 20));

    say("External memory for buffers (transfer usage):");
    static const struct {
        VkExternalMemoryHandleTypeFlagBits type;
        const char *sz_name;
    } s_types[] = {
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, "HOST_ALLOCATION"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT, "HOST_MAPPED_FOREIGN_MEMORY"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, "OPAQUE_WIN32"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, "OPAQUE_WIN32_KMT"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, "D3D12_RESOURCE"},
    };
    for (size_t i = 0; i < sizeof(s_types) / sizeof(s_types[0]); ++i) {
        char sz_features[64];
        external_features_text(external_buffer_features(p_device, s_types[i].type), sz_features, sizeof(sz_features));
        say("  %-28s %s", s_types[i].sz_name, sz_features);
    }

    say("All device extensions (%u):", p_device->c_extensions);
    char sz_line[120];
    size_t off_line = 0;
    for (uint32_t i = 0; i < p_device->c_extensions; ++i) {
        const char *sz_name = p_device->p_extensions[i].extensionName;
        if (off_line && off_line + strlen(sz_name) + 1 >= 100) {
            say("  %s", sz_line);
            off_line = 0;
        }
        const int cb_written =
            snprintf(sz_line + off_line, sizeof(sz_line) - off_line, "%s%s", off_line ? " " : "", sz_name);
        if (cb_written > 0) off_line += (size_t)cb_written < sizeof(sz_line) - off_line ? (size_t)cb_written : 0;
    }
    if (off_line) say("  %s", sz_line);
}
