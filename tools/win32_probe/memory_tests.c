#include "probe.h"

#define GPU_TIMEOUT_NS 5000000000ull

const char *const g_sz_test_ids[TEST_COUNT] = {"a", "b", "b2", "c", "d", "e1", "e2"};
const char *const g_sz_test_names[TEST_COUNT] = {
    "host import of private memory (VirtualAlloc)",
    "host import of a view of a pagefile section (CreateFileMapping SEC_COMMIT, MapViewOfFile)",
    "host import of a view of a SEC_RESERVE section (pages committed by VirtualAlloc)",
    "host import of a view placed into a placeholder (VirtualAlloc2, MapViewOfFile3: psport's guest mapping)",
    "host import of two views of one section at different addresses, both imported (alias)",
    "OPAQUE_WIN32 export and import round trip (VK_KHR_external_memory_win32)",
    "OPAQUE_WIN32_KMT export and import round trip (VK_KHR_external_memory_win32)",
};

typedef struct {
    Device *p_device;
    VkDevice h_device;
    VkQueue h_queue;
    VkCommandPool h_pool;
    VkCommandBuffer h_cmd;
    VkFence h_fence;
    PFN_vkGetMemoryHostPointerPropertiesEXT pfn_get_host_pointer_properties;
    PFN_vkGetMemoryWin32HandleKHR pfn_get_win32_handle;
    /* After a timeout or a lost device nothing more is submitted or destroyed, since the GPU may
     * still use it. */
    int is_unusable;
} Gpu;

typedef struct {
    VkBuffer h_buffer;
    VkDeviceMemory h_memory;
} Import;

/* ---- Active tests ---- */

static int open_gpu(Device *p_device, Gpu *p_gpu) {
    g_sz_step = "creating a Vulkan device for the tests";
    memset(p_gpu, 0, sizeof(*p_gpu));
    p_gpu->p_device = p_device;
    VkQueueFamilyProperties families[16];
    uint32_t c_families = 16;
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(p_device->h_physical, &c_families, families);
    uint32_t i_family = UINT32_MAX;
    const VkQueueFlags transfer = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    for (uint32_t i = 0; i < c_families && i_family == UINT32_MAX; ++i)
        if (families[i].queueFlags & transfer && families[i].queueCount) i_family = i;
    if (i_family == UINT32_MAX) {
        snprintf(p_device->sz_tests_skipped, sizeof(p_device->sz_tests_skipped), "no queue family takes transfers");
        return 0;
    }
    const char *sz_extensions[2];
    uint32_t c_extensions = 0;
    if (has_device_extension(p_device, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
        sz_extensions[c_extensions++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (has_device_extension(p_device, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME))
        sz_extensions[c_extensions++] = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueFamilyIndex = i_family,
                                           .queueCount = 1,
                                           .pQueuePriorities = &priority};
    const VkDeviceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                       .queueCreateInfoCount = 1,
                                       .pQueueCreateInfos = &queue,
                                       .enabledExtensionCount = c_extensions,
                                       .ppEnabledExtensionNames = sz_extensions};
    VkResult result = g_vk.vkCreateDevice(p_device->h_physical, &create, NULL, &p_gpu->h_device);
    if (result != VK_SUCCESS) {
        snprintf(p_device->sz_tests_skipped, sizeof(p_device->sz_tests_skipped), "vkCreateDevice -> %s (%d)",
                 vk_result_name(result), result);
        return 0;
    }
    g_vk.vkGetDeviceQueue(p_gpu->h_device, i_family, 0, &p_gpu->h_queue);
    p_gpu->pfn_get_host_pointer_properties = (PFN_vkGetMemoryHostPointerPropertiesEXT)g_vk.vkGetDeviceProcAddr(
        p_gpu->h_device, "vkGetMemoryHostPointerPropertiesEXT");
    p_gpu->pfn_get_win32_handle =
        (PFN_vkGetMemoryWin32HandleKHR)g_vk.vkGetDeviceProcAddr(p_gpu->h_device, "vkGetMemoryWin32HandleKHR");
    const VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = i_family};
    const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = g_vk.vkCreateCommandPool(p_gpu->h_device, &pool, NULL, &p_gpu->h_pool);
    if (result == VK_SUCCESS) {
        const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                      .commandPool = p_gpu->h_pool,
                                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                      .commandBufferCount = 1};
        result = g_vk.vkAllocateCommandBuffers(p_gpu->h_device, &allocate, &p_gpu->h_cmd);
    }
    if (result == VK_SUCCESS) result = g_vk.vkCreateFence(p_gpu->h_device, &fence, NULL, &p_gpu->h_fence);
    if (result != VK_SUCCESS) {
        snprintf(p_device->sz_tests_skipped, sizeof(p_device->sz_tests_skipped),
                 "command pool, command buffer or fence -> %s (%d)", vk_result_name(result), result);
        if (p_gpu->h_pool) g_vk.vkDestroyCommandPool(p_gpu->h_device, p_gpu->h_pool, NULL);
        g_vk.vkDestroyDevice(p_gpu->h_device, NULL);
        return 0;
    }
    return 1;
}

static void close_gpu(Gpu *p_gpu) {
    if (p_gpu->is_unusable) {
        say("Device %s left open: the GPU did not finish its work", p_gpu->p_device->properties.deviceName);
        return;
    }
    g_vk.vkDeviceWaitIdle(p_gpu->h_device);
    g_vk.vkDestroyFence(p_gpu->h_device, p_gpu->h_fence, NULL);
    g_vk.vkDestroyCommandPool(p_gpu->h_device, p_gpu->h_pool, NULL);
    g_vk.vkDestroyDevice(p_gpu->h_device, NULL);
}

static void release_import(Gpu *p_gpu, Import *p_import) {
    if (p_gpu->is_unusable) return;
    if (p_import->h_buffer) g_vk.vkDestroyBuffer(p_gpu->h_device, p_import->h_buffer, NULL);
    if (p_import->h_memory) g_vk.vkFreeMemory(p_gpu->h_device, p_import->h_memory, NULL);
    memset(p_import, 0, sizeof(*p_import));
}

static VkBuffer create_buffer(Gpu *p_gpu, VkDeviceSize cb, VkExternalMemoryHandleTypeFlags handle_types,
                              VkResult *p_out_result) {
    const VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                       .handleTypes = handle_types};
    const VkBufferCreateInfo create = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .pNext = &external,
                                       .size = cb,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer h_buffer = VK_NULL_HANDLE;
    *p_out_result = g_vk.vkCreateBuffer(p_gpu->h_device, &create, NULL, &h_buffer);
    return *p_out_result == VK_SUCCESS ? h_buffer : VK_NULL_HANDLE;
}

/* Imports [p_host, p_host + cb) as a buffer the way psport does (gpu/memory/psport_guest_memory.cpp). */
static int import_host(Gpu *p_gpu, void *p_host, VkDeviceSize cb, Import *p_out, Outcome *p_outcome,
                       const char *sz_what) {
    memset(p_out, 0, sizeof(*p_out));
    if (!p_gpu->pfn_get_host_pointer_properties) {
        set_outcome(p_outcome, OUTCOME_FAIL, VK_ERROR_EXTENSION_NOT_PRESENT, "%s: no VK_EXT_external_memory_host",
                    sz_what);
        return 0;
    }
    VkMemoryHostPointerPropertiesEXT host = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult result = p_gpu->pfn_get_host_pointer_properties(
        p_gpu->h_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p_host, &host);
    if (result != VK_SUCCESS) {
        set_outcome(p_outcome, OUTCOME_FAIL, result, "%s: vkGetMemoryHostPointerPropertiesEXT at %p -> %s (%d)",
                    sz_what, p_host, vk_result_name(result), result);
        return 0;
    }
    p_out->h_buffer = create_buffer(p_gpu, cb, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, &result);
    if (!p_out->h_buffer) {
        set_outcome(p_outcome, OUTCOME_FAIL, result, "%s: vkCreateBuffer -> %s (%d)", sz_what,
                    vk_result_name(result), result);
        return 0;
    }
    VkMemoryRequirements requirements;
    g_vk.vkGetBufferMemoryRequirements(p_gpu->h_device, p_out->h_buffer, &requirements);
    const uint32_t type_bits = requirements.memoryTypeBits & host.memoryTypeBits;
    if (!type_bits) {
        release_import(p_gpu, p_out);
        set_outcome(p_outcome, OUTCOME_FAIL, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                    "%s: no memory type for the pointer (host pointer types %#x, buffer types %#x)", sz_what,
                    host.memoryTypeBits, requirements.memoryTypeBits);
        return 0;
    }
    uint32_t i_type = 0;
    while (!(type_bits & (1u << i_type))) ++i_type;
    const VkImportMemoryHostPointerInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                                     .handleType =
                                                         VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                     .pHostPointer = p_host};
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                           .pNext = &import,
                                           .allocationSize = cb,
                                           .memoryTypeIndex = i_type};
    result = g_vk.vkAllocateMemory(p_gpu->h_device, &allocate, NULL, &p_out->h_memory);
    if (result != VK_SUCCESS) {
        p_out->h_memory = VK_NULL_HANDLE;
        release_import(p_gpu, p_out);
        set_outcome(p_outcome, OUTCOME_FAIL, result, "%s: vkAllocateMemory (import, type %u) -> %s (%d)", sz_what,
                    i_type, vk_result_name(result), result);
        return 0;
    }
    result = g_vk.vkBindBufferMemory(p_gpu->h_device, p_out->h_buffer, p_out->h_memory, 0);
    if (result != VK_SUCCESS) {
        release_import(p_gpu, p_out);
        set_outcome(p_outcome, OUTCOME_FAIL, result, "%s: vkBindBufferMemory -> %s (%d)", sz_what,
                    vk_result_name(result), result);
        return 0;
    }
    return 1;
}

#define C_PATTERN 64u
#define OFF_COPIED 4096u
#define OFF_FILLED 8192u
#define FILL_VALUE 0x5a5a5a5au

/* The CPU writes through p_cpu_write, the GPU copies it through h_gpu_read and writes through h_gpu_write,
 * and the CPU reads the results through p_cpu_read: with two views of one section, each step crosses
 * from one view to the other. */
static void check_round_trip(Gpu *p_gpu, uint8_t *p_cpu_write, VkBuffer h_gpu_read, VkBuffer h_gpu_write,
                             uint8_t *p_cpu_read, Outcome *p_outcome) {
    g_sz_step = "GPU round trip on imported memory";
    uint32_t *p_source = (uint32_t *)p_cpu_write;
    for (uint32_t i = 0; i < C_PATTERN; ++i) p_source[i] = 0xc0de0000u + i;
    memset(p_cpu_read + OFF_COPIED, 0, C_PATTERN * sizeof(uint32_t));
    memset(p_cpu_read + OFF_FILLED, 0, C_PATTERN * sizeof(uint32_t));
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkResult result = g_vk.vkResetCommandBuffer(p_gpu->h_cmd, 0);
    if (result == VK_SUCCESS) result = g_vk.vkBeginCommandBuffer(p_gpu->h_cmd, &begin);
    if (result != VK_SUCCESS) {
        set_outcome(p_outcome, OUTCOME_FAIL, result, "recording the GPU check -> %s (%d)", vk_result_name(result),
                    result);
        return;
    }
    const VkMemoryBarrier host_to_transfer = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                              .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
                                              .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    g_vk.vkCmdPipelineBarrier(p_gpu->h_cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                              &host_to_transfer, 0, NULL, 0, NULL);
    const VkBufferCopy copy = {.srcOffset = 0, .dstOffset = OFF_COPIED, .size = C_PATTERN * sizeof(uint32_t)};
    g_vk.vkCmdCopyBuffer(p_gpu->h_cmd, h_gpu_read, h_gpu_write, 1, &copy);
    g_vk.vkCmdFillBuffer(p_gpu->h_cmd, h_gpu_write, OFF_FILLED, C_PATTERN * sizeof(uint32_t), FILL_VALUE);
    const VkMemoryBarrier transfer_to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                              .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    g_vk.vkCmdPipelineBarrier(p_gpu->h_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                              &transfer_to_host, 0, NULL, 0, NULL);
    result = g_vk.vkEndCommandBuffer(p_gpu->h_cmd);
    if (result == VK_SUCCESS) result = g_vk.vkResetFences(p_gpu->h_device, 1, &p_gpu->h_fence);
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &p_gpu->h_cmd};
    if (result == VK_SUCCESS) result = g_vk.vkQueueSubmit(p_gpu->h_queue, 1, &submit, p_gpu->h_fence);
    if (result == VK_SUCCESS) result = g_vk.vkWaitForFences(p_gpu->h_device, 1, &p_gpu->h_fence, VK_TRUE, GPU_TIMEOUT_NS);
    if (result != VK_SUCCESS) {
        if (result == VK_TIMEOUT || result == VK_ERROR_DEVICE_LOST) p_gpu->is_unusable = 1;
        set_outcome(p_outcome, OUTCOME_FAIL, result, "the GPU copy and fill -> %s (%d)%s", vk_result_name(result),
                    result, result == VK_TIMEOUT ? ": not finished after 5 s" : "");
        return;
    }
    const uint32_t *p_copied = (const uint32_t *)(p_cpu_read + OFF_COPIED);
    const uint32_t *p_filled = (const uint32_t *)(p_cpu_read + OFF_FILLED);
    for (uint32_t i = 0; i < C_PATTERN; ++i) {
        if (p_copied[i] != 0xc0de0000u + i) {
            set_outcome(p_outcome, OUTCOME_FAIL, VK_SUCCESS,
                        "imported, but the GPU did not read what the CPU wrote (word %u: %#x, expected %#x)", i,
                        p_copied[i], 0xc0de0000u + i);
            return;
        }
        if (p_filled[i] != FILL_VALUE) {
            set_outcome(p_outcome, OUTCOME_FAIL, VK_SUCCESS,
                        "imported, but the CPU does not see what the GPU wrote (word %u: %#x, expected %#x)", i,
                        p_filled[i], FILL_VALUE);
            return;
        }
    }
    set_outcome(p_outcome, OUTCOME_PASS, VK_SUCCESS, "imported; the GPU read the CPU's writes and the CPU saw the GPU's");
}

static void test_single(Gpu *p_gpu, void *p_host, VkDeviceSize cb, Outcome *p_outcome, const char *sz_what) {
    Import import;
    if (!import_host(p_gpu, p_host, cb, &import, p_outcome, sz_what)) return;
    check_round_trip(p_gpu, p_host, import.h_buffer, import.h_buffer, p_host, p_outcome);
    release_import(p_gpu, &import);
}

static void test_private(Gpu *p_gpu, VkDeviceSize cb, Outcome *p_outcome) {
    g_sz_step = "test a: private memory";
    void *p_host = VirtualAlloc(NULL, cb, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p_host) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "VirtualAlloc failed (error %lu)", GetLastError());
        return;
    }
    test_single(p_gpu, p_host, cb, p_outcome, "private memory");
    if (!p_gpu->is_unusable) VirtualFree(p_host, 0, MEM_RELEASE);
}

static void test_section_view(Gpu *p_gpu, VkDeviceSize cb, int is_reserve, Outcome *p_outcome) {
    g_sz_step = is_reserve ? "test b2: view of a SEC_RESERVE section" : "test b: view of a section";
    HANDLE h_section = create_section(cb, is_reserve ? SEC_RESERVE : SEC_COMMIT);
    if (!h_section) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "CreateFileMappingW failed (error %lu)", GetLastError());
        return;
    }
    void *p_view = MapViewOfFile(h_section, FILE_MAP_ALL_ACCESS, 0, 0, cb);
    if (!p_view) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "MapViewOfFile failed (error %lu)", GetLastError());
    } else if (is_reserve && !VirtualAlloc(p_view, cb, MEM_COMMIT, PAGE_READWRITE)) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "committing the view's pages failed (error %lu)",
                    GetLastError());
    } else {
        test_single(p_gpu, p_view, cb, p_outcome, is_reserve ? "SEC_RESERVE section view" : "section view");
    }
    if (p_gpu->is_unusable) return;
    if (p_view) UnmapViewOfFile(p_view);
    CloseHandle(h_section);
}

static void test_placed_view(Gpu *p_gpu, VkDeviceSize cb, Outcome *p_outcome) {
    g_sz_step = "test c: view placed into a placeholder";
    if (!has_placeholders()) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "VirtualAlloc2 or MapViewOfFile3 missing");
        return;
    }
    HANDLE h_section = create_section(cb, SEC_COMMIT);
    if (!h_section) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "CreateFileMappingW failed (error %lu)", GetLastError());
        return;
    }
    DWORD error = 0;
    void *p_view = map_placed_view(h_section, cb, &error);
    if (!p_view) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "placing the view failed (error %lu)", error);
    } else {
        test_single(p_gpu, p_view, cb, p_outcome, "placed view");
    }
    if (p_gpu->is_unusable) return;
    if (p_view) UnmapViewOfFile(p_view);
    CloseHandle(h_section);
}

static void test_alias_views(Gpu *p_gpu, VkDeviceSize cb, Outcome *p_outcome) {
    g_sz_step = "test d: two views of one section, both imported";
    if (!has_placeholders()) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "VirtualAlloc2 or MapViewOfFile3 missing");
        return;
    }
    HANDLE h_section = create_section(cb, SEC_COMMIT);
    if (!h_section) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "CreateFileMappingW failed (error %lu)", GetLastError());
        return;
    }
    DWORD error = 0;
    uint8_t *p_guest = map_placed_view(h_section, cb, &error);
    uint8_t *p_backing = p_guest ? MapViewOfFile(h_section, FILE_MAP_ALL_ACCESS, 0, 0, cb) : NULL;
    if (!p_guest || !p_backing) {
        if (p_backing == NULL && p_guest) error = GetLastError();
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "mapping the two views failed (error %lu)", error);
    } else {
        Import guest, backing;
        if (import_host(p_gpu, p_guest, cb, &guest, p_outcome, "first view (placed)")) {
            if (import_host(p_gpu, p_backing, cb, &backing, p_outcome,
                            "second view (MapViewOfFile) while the first is imported")) {
                check_round_trip(p_gpu, p_guest, backing.h_buffer, guest.h_buffer, p_backing, p_outcome);
                if (p_outcome->state == OUTCOME_PASS)
                    snprintf(p_outcome->sz_detail, sizeof(p_outcome->sz_detail),
                             "both views imported (%p, %p); writes cross between them both ways", (void *)p_guest,
                             (void *)p_backing);
                release_import(p_gpu, &backing);
            }
            release_import(p_gpu, &guest);
        }
    }
    if (p_gpu->is_unusable) return;
    if (p_backing) UnmapViewOfFile(p_backing);
    if (p_guest) UnmapViewOfFile(p_guest);
    CloseHandle(h_section);
}

static uint32_t pick_memory_type(const Device *p_device, uint32_t type_bits, VkMemoryPropertyFlags wanted) {
    for (uint32_t i = 0; i < p_device->memory.memoryTypeCount; ++i)
        if (type_bits & (1u << i) && (p_device->memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return UINT32_MAX;
}

/* Memory Vulkan allocates and exports, imported again through its handle; then whether the handle maps on
 * the CPU, which psport would need to place that memory at guest addresses. */
static void test_opaque(Gpu *p_gpu, VkExternalMemoryHandleTypeFlagBits handle_type, Outcome *p_outcome) {
    const int is_nt = handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    g_sz_step = is_nt ? "test e1: OPAQUE_WIN32 round trip" : "test e2: OPAQUE_WIN32_KMT round trip";
    if (!has_device_extension(p_gpu->p_device, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) ||
        !p_gpu->pfn_get_win32_handle) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "no VK_KHR_external_memory_win32");
        return;
    }
    const VkExternalMemoryFeatureFlags features = external_buffer_features(p_gpu->p_device, handle_type);
    const VkExternalMemoryFeatureFlags both =
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    if ((features & both) != both) {
        set_outcome(p_outcome, OUTCOME_NOT_RUN, VK_SUCCESS, "not both exportable and importable for buffers");
        return;
    }
    const int is_dedicated = (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    VkResult result;
    Import source = {0}, imported = {0};
    HANDLE h_memory = NULL;
    source.h_buffer = create_buffer(p_gpu, CB_TEST, handle_type, &result);
    if (!source.h_buffer) {
        set_outcome(p_outcome, OUTCOME_FAIL, result, "vkCreateBuffer -> %s (%d)", vk_result_name(result), result);
        return;
    }
    VkMemoryRequirements requirements;
    g_vk.vkGetBufferMemoryRequirements(p_gpu->h_device, source.h_buffer, &requirements);
    const VkMemoryPropertyFlags mappable = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t i_type = pick_memory_type(p_gpu->p_device, requirements.memoryTypeBits, mappable);
    const int is_mappable = i_type != UINT32_MAX;
    if (!is_mappable) i_type = pick_memory_type(p_gpu->p_device, requirements.memoryTypeBits, 0);
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                               .buffer = source.h_buffer};
    const VkExportMemoryAllocateInfo export_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                                    .pNext = is_dedicated ? &dedicated : NULL,
                                                    .handleTypes = handle_type};
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .pNext = &export_info,
                                     .allocationSize = requirements.size,
                                     .memoryTypeIndex = i_type};
    result = g_vk.vkAllocateMemory(p_gpu->h_device, &allocate, NULL, &source.h_memory);
    if (result != VK_SUCCESS) {
        source.h_memory = VK_NULL_HANDLE;
        set_outcome(p_outcome, OUTCOME_FAIL, result, "vkAllocateMemory (export, type %u) -> %s (%d)", i_type,
                    vk_result_name(result), result);
        release_import(p_gpu, &source);
        return;
    }
    result = g_vk.vkBindBufferMemory(p_gpu->h_device, source.h_buffer, source.h_memory, 0);
    const VkMemoryGetWin32HandleInfoKHR get = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
                                               .memory = source.h_memory,
                                               .handleType = handle_type};
    if (result == VK_SUCCESS) result = p_gpu->pfn_get_win32_handle(p_gpu->h_device, &get, &h_memory);
    if (result != VK_SUCCESS || !h_memory) {
        set_outcome(p_outcome, OUTCOME_FAIL, result, "binding or vkGetMemoryWin32HandleKHR -> %s (%d)",
                    vk_result_name(result), result);
        release_import(p_gpu, &source);
        return;
    }
    char sz_cpu_map[96] = "";
    if (is_nt) {
        void *p_mapped = MapViewOfFile(h_memory, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (p_mapped) {
            snprintf(sz_cpu_map, sizeof(sz_cpu_map), "; the exported handle maps on the CPU (MapViewOfFile)");
            UnmapViewOfFile(p_mapped);
        } else {
            snprintf(sz_cpu_map, sizeof(sz_cpu_map), "; the exported handle does not map on the CPU (MapViewOfFile: error %lu)",
                     GetLastError());
        }
    }
    imported.h_buffer = create_buffer(p_gpu, CB_TEST, handle_type, &result);
    if (imported.h_buffer) {
        dedicated.buffer = imported.h_buffer;
        const VkImportMemoryWin32HandleInfoKHR import_info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
            .pNext = is_dedicated ? &dedicated : NULL,
            .handleType = handle_type,
            .handle = h_memory};
        allocate.pNext = &import_info;
        result = g_vk.vkAllocateMemory(p_gpu->h_device, &allocate, NULL, &imported.h_memory);
        if (result != VK_SUCCESS) imported.h_memory = VK_NULL_HANDLE;
        if (result == VK_SUCCESS)
            result = g_vk.vkBindBufferMemory(p_gpu->h_device, imported.h_buffer, imported.h_memory, 0);
    }
    if (result != VK_SUCCESS) {
        set_outcome(p_outcome, OUTCOME_FAIL, result, "the import of the exported handle -> %s (%d)%s",
                    vk_result_name(result), result, sz_cpu_map);
    } else if (!is_mappable) {
        set_outcome(p_outcome, OUTCOME_PASS, VK_SUCCESS,
                    "exported and imported (contents not compared: no host-visible coherent type)%s", sz_cpu_map);
    } else {
        uint32_t *p_source = NULL, *p_imported = NULL;
        result = g_vk.vkMapMemory(p_gpu->h_device, source.h_memory, 0, VK_WHOLE_SIZE, 0, (void **)&p_source);
        if (result == VK_SUCCESS)
            result = g_vk.vkMapMemory(p_gpu->h_device, imported.h_memory, 0, VK_WHOLE_SIZE, 0, (void **)&p_imported);
        if (result != VK_SUCCESS) {
            set_outcome(p_outcome, OUTCOME_FAIL, result, "vkMapMemory -> %s (%d)%s", vk_result_name(result), result,
                        sz_cpu_map);
        } else {
            for (uint32_t i = 0; i < C_PATTERN; ++i) p_source[i] = 0xbeef0000u + i;
            int is_same = 1;
            for (uint32_t i = 0; i < C_PATTERN && is_same; ++i) is_same = p_imported[i] == 0xbeef0000u + i;
            set_outcome(p_outcome, is_same ? OUTCOME_PASS : OUTCOME_FAIL, VK_SUCCESS, "%s%s",
                        is_same ? "exported and imported; both objects show the same contents"
                                : "exported and imported, but the imported object does not show the writes",
                        sz_cpu_map);
        }
        if (p_imported) g_vk.vkUnmapMemory(p_gpu->h_device, imported.h_memory);
        if (p_source) g_vk.vkUnmapMemory(p_gpu->h_device, source.h_memory);
    }
    release_import(p_gpu, &imported);
    release_import(p_gpu, &source);
    // An NT handle stays the application's after an import; a KMT handle is not one to close.
    if (is_nt) CloseHandle(h_memory);
}

void run_device_tests(Device *p_device) {
    VkDeviceSize cb = CB_TEST;
    const VkDeviceSize cb_align = p_device->cb_import_alignment > g_cb_granularity ? p_device->cb_import_alignment
                                                                                   : g_cb_granularity;
    cb = (cb + cb_align - 1) / cb_align * cb_align;
    if (p_device->properties.apiVersion < VK_API_VERSION_1_1) {
        snprintf(p_device->sz_tests_skipped, sizeof(p_device->sz_tests_skipped), "device below Vulkan 1.1");
    }
    Gpu gpu;
    if (p_device->sz_tests_skipped[0] || !open_gpu(p_device, &gpu)) {
        for (int i = 0; i < TEST_COUNT; ++i)
            set_outcome(&p_device->outcomes[i], OUTCOME_NOT_RUN, VK_SUCCESS, "%s", p_device->sz_tests_skipped);
    } else {
        const int has_host_import = p_device->cb_import_alignment != 0;
        for (int i = TEST_PRIVATE; i <= TEST_ALIAS_VIEWS && !has_host_import; ++i)
            set_outcome(&p_device->outcomes[i], OUTCOME_FAIL, VK_ERROR_EXTENSION_NOT_PRESENT,
                        "no VK_EXT_external_memory_host on this device");
        if (has_host_import) {
            test_private(&gpu, cb, &p_device->outcomes[TEST_PRIVATE]);
            if (!gpu.is_unusable) test_section_view(&gpu, cb, 0, &p_device->outcomes[TEST_SECTION_VIEW]);
            if (!gpu.is_unusable) test_section_view(&gpu, cb, 1, &p_device->outcomes[TEST_SECTION_RESERVE_VIEW]);
            if (!gpu.is_unusable) test_placed_view(&gpu, cb, &p_device->outcomes[TEST_PLACED_VIEW]);
            if (!gpu.is_unusable) test_alias_views(&gpu, cb, &p_device->outcomes[TEST_ALIAS_VIEWS]);
        }
        if (!gpu.is_unusable)
            test_opaque(&gpu, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, &p_device->outcomes[TEST_OPAQUE_WIN32]);
        if (!gpu.is_unusable)
            test_opaque(&gpu, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
                        &p_device->outcomes[TEST_OPAQUE_WIN32_KMT]);
        for (int i = 0; i < TEST_COUNT; ++i)
            if (gpu.is_unusable && p_device->outcomes[i].state == OUTCOME_NOT_RUN && !p_device->outcomes[i].sz_detail[0])
                set_outcome(&p_device->outcomes[i], OUTCOME_NOT_RUN, VK_SUCCESS,
                            "skipped: an earlier test left the GPU busy or lost");
        close_gpu(&gpu);
    }
    say("Memory sharing tests (%llu KiB each):", (unsigned long long)(cb >> 10));
    static const char *const s_sz_states[] = {"NOT RUN", "PASS", "FAIL"};
    for (int i = 0; i < TEST_COUNT; ++i) {
        const Outcome *p_outcome = &p_device->outcomes[i];
        say("  [%s] %s: %s (%s, %d)", g_sz_test_ids[i], g_sz_test_names[i], s_sz_states[p_outcome->state],
            vk_result_name(p_outcome->result), p_outcome->result);
        if (p_outcome->sz_detail[0]) say("       %s", p_outcome->sz_detail);
    }
}
