/* psport: Wine side of the memory bridge test (tools/bridge_helper). Built with MinGW, run under
 * Proton's Wine. Imports a Vulkan buffer the native process exported as an opaque fd (inherited
 * fd number on the command line) through VK_KHR_external_memory_win32, checks the native
 * pattern and writes its own.
 *
 * usage: bridge_helper.exe <fd> <size> <device uuid hex> */
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

typedef LONG(WINAPI *FdToHandle)(int h_fd, unsigned int access, unsigned int attributes,
                                 HANDLE *p_out_handle);

static PFN_vkGetInstanceProcAddr s_pfn_get_instance_proc;
#define LOAD(instance, name) PFN_##name name = (PFN_##name)s_pfn_get_instance_proc(instance, #name)

static int fail(const char *sz_what, int code) {
    printf("bridge_helper: %s (%d)\n", sz_what, code);
    fflush(stdout);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        return fail("usage: bridge_helper <fd> <size> <uuid>", 0);
    }
    const int h_fd = atoi(argv[1]);
    const VkDeviceSize cb_buffer = strtoull(argv[2], NULL, 10);
    uint8_t uuid[VK_UUID_SIZE];
    for (int i = 0; i < VK_UUID_SIZE; ++i) {
        unsigned value = 0;
        sscanf(argv[3] + 2 * i, "%2x", &value);
        uuid[i] = (uint8_t)value;
    }

    HMODULE h_ntdll = GetModuleHandleA("ntdll.dll");
    FdToHandle pfn_fd_to_handle = (FdToHandle)GetProcAddress(h_ntdll, "wine_server_fd_to_handle");
    if (!pfn_fd_to_handle) {
        return fail("ntdll has no wine_server_fd_to_handle", 0);
    }
    HANDLE h_memory = NULL;
    LONG status = pfn_fd_to_handle(h_fd, GENERIC_ALL, 0, &h_memory);
    if (status != 0 || !h_memory) {
        return fail("wine_server_fd_to_handle failed", (int)status);
    }

    HMODULE h_vulkan = LoadLibraryA("vulkan-1.dll");
    if (!h_vulkan) {
        return fail("cannot load vulkan-1.dll", (int)GetLastError());
    }
    s_pfn_get_instance_proc = (PFN_vkGetInstanceProcAddr)GetProcAddress(h_vulkan, "vkGetInstanceProcAddr");
    LOAD(NULL, vkCreateInstance);
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                              .pApplicationInfo = &app};
    VkInstance h_instance;
    VkResult r = vkCreateInstance(&instance_ci, NULL, &h_instance);
    if (r) return fail("vkCreateInstance", r);
    LOAD(h_instance, vkEnumeratePhysicalDevices);
    LOAD(h_instance, vkGetPhysicalDeviceProperties2);
    LOAD(h_instance, vkGetPhysicalDeviceMemoryProperties);
    LOAD(h_instance, vkCreateDevice);
    LOAD(h_instance, vkGetDeviceProcAddr);

    VkPhysicalDevice devices[8];
    uint32_t c_devices = 8;
    vkEnumeratePhysicalDevices(h_instance, &c_devices, devices);
    VkPhysicalDevice h_physical = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < c_devices; ++i) {
        VkPhysicalDeviceIDProperties ids = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                             .pNext = &ids};
        vkGetPhysicalDeviceProperties2(devices[i], &props);
        if (!memcmp(ids.deviceUUID, uuid, VK_UUID_SIZE)) {
            h_physical = devices[i];
            printf("bridge_helper: device %s\n", props.properties.deviceName);
        }
    }
    if (!h_physical) return fail("no device with the native UUID", (int)c_devices);

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = 0,
                                              .queueCount = 1,
                                              .pQueuePriorities = &priority};
    const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME};
    const VkDeviceCreateInfo device_ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                          .queueCreateInfoCount = 1,
                                          .pQueueCreateInfos = &queue_ci,
                                          .enabledExtensionCount = 1,
                                          .ppEnabledExtensionNames = extensions};
    VkDevice h_device;
    r = vkCreateDevice(h_physical, &device_ci, NULL, &h_device);
    if (r) return fail("vkCreateDevice (external_memory_win32)", r);
#define DLOAD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(h_device, #name)
    DLOAD(vkCreateBuffer);
    DLOAD(vkGetBufferMemoryRequirements);
    DLOAD(vkAllocateMemory);
    DLOAD(vkBindBufferMemory);
    DLOAD(vkMapMemory);
    DLOAD(vkGetDeviceQueue);
    DLOAD(vkCreateCommandPool);
    DLOAD(vkAllocateCommandBuffers);
    DLOAD(vkBeginCommandBuffer);
    DLOAD(vkCmdCopyBuffer);
    DLOAD(vkCmdFillBuffer);
    DLOAD(vkCmdPipelineBarrier);
    DLOAD(vkEndCommandBuffer);
    DLOAD(vkQueueSubmit);
    DLOAD(vkQueueWaitIdle);

    const VkExternalMemoryBufferCreateInfo external_buffer = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    const VkBufferCreateInfo shared_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                          .pNext = &external_buffer,
                                          .size = cb_buffer,
                                          .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer h_shared;
    vkCreateBuffer(h_device, &shared_ci, NULL, &h_shared);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(h_device, h_shared, &requirements);
    VkPhysicalDeviceMemoryProperties memory_props;
    vkGetPhysicalDeviceMemoryProperties(h_physical, &memory_props);
    uint32_t type = 0;
    while (!(requirements.memoryTypeBits & (1u << type))) ++type;
    const VkImportMemoryWin32HandleInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
        .handle = h_memory};
    const VkMemoryAllocateInfo shared_alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                               .pNext = &import,
                                               .allocationSize = requirements.size,
                                               .memoryTypeIndex = type};
    VkDeviceMemory h_shared_memory;
    r = vkAllocateMemory(h_device, &shared_alloc, NULL, &h_shared_memory);
    if (r) return fail("vkAllocateMemory import", r);
    vkBindBufferMemory(h_device, h_shared, h_shared_memory, 0);

    /* Readback buffer (host visible). */
    const VkBufferCreateInfo read_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                        .size = cb_buffer,
                                        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer h_readback;
    vkCreateBuffer(h_device, &read_ci, NULL, &h_readback);
    vkGetBufferMemoryRequirements(h_device, h_readback, &requirements);
    type = 0;
    while (!((requirements.memoryTypeBits & (1u << type)) &&
             (memory_props.memoryTypes[type].propertyFlags &
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
        ++type;
    const VkMemoryAllocateInfo read_alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .allocationSize = requirements.size,
                                             .memoryTypeIndex = type};
    VkDeviceMemory h_read_memory;
    vkAllocateMemory(h_device, &read_alloc, NULL, &h_read_memory);
    vkBindBufferMemory(h_device, h_readback, h_read_memory, 0);

    VkQueue h_queue;
    vkGetDeviceQueue(h_device, 0, 0, &h_queue);
    const VkCommandPoolCreateInfo pool_ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool h_pool;
    vkCreateCommandPool(h_device, &pool_ci, NULL, &h_pool);
    const VkCommandBufferAllocateInfo cb_ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .commandPool = h_pool,
                                               .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1};
    VkCommandBuffer h_cmd;
    vkAllocateCommandBuffers(h_device, &cb_ai, &h_cmd);
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(h_cmd, &begin);
    const VkBufferCopy region = {.size = cb_buffer};
    vkCmdCopyBuffer(h_cmd, h_shared, h_readback, 1, &region);
    const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(h_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                         &barrier, 0, NULL, 0, NULL);
    vkCmdFillBuffer(h_cmd, h_shared, 0, cb_buffer, 0x5EED1234u);
    vkEndCommandBuffer(h_cmd);
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .commandBufferCount = 1,
                                 .pCommandBuffers = &h_cmd};
    r = vkQueueSubmit(h_queue, 1, &submit, VK_NULL_HANDLE);
    if (r) return fail("vkQueueSubmit", r);
    vkQueueWaitIdle(h_queue);

    uint32_t *p_words;
    vkMapMemory(h_device, h_read_memory, 0, cb_buffer, 0, (void **)&p_words);
    size_t bad = 0;
    for (size_t i = 0; i < cb_buffer / 4; ++i) {
        bad += p_words[i] != 0xB12D6E00u + (uint32_t)(i & 0xFF);
    }
    printf("bridge_helper: native pattern %s (%zu bad words of %zu); wrote 0x5EED1234\n",
           bad ? "WRONG" : "OK", bad, (size_t)(cb_buffer / 4));
    fflush(stdout);
    return bad ? 2 : 0;
}
