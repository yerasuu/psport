/* Probe for the PC memory model: guest memory the GPU uses in place, through the sparse arena.
 *
 * Guest direct memory is allocated as host-visible Vulkan memory, exported as a dma-buf and
 * mapped by the runtime (gpu/memory/psport_guest_memory.cpp). This checks, on the running GPU,
 * whether such memory can back a sparse buffer (the arena) at an arbitrary block, whether CPU
 * writes through the dma-buf mapping are seen by the GPU and GPU writes by the CPU without extra
 * steps, and how fast the GPU reads it compared with device-local memory.
 *
 * Usage: bb-sparse-host-probe [MiB]   (default 256)
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, r_);                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static VkInstance s_h_instance;
static VkPhysicalDevice s_h_physical;
static VkDevice s_h_device;
static VkQueue s_h_queue;
static uint32_t s_i_family;
static VkCommandPool s_h_pool;
static VkCommandBuffer s_h_cmd;
static VkFence s_h_fence;
static VkPhysicalDeviceMemoryProperties s_memory_props;

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < s_memory_props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = s_memory_props.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) return i;
    }
    return UINT32_MAX;
}

static void init(void) {
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &s_h_instance));
    uint32_t c_devices = 8;
    VkPhysicalDevice devices[8];
    CHECK(vkEnumeratePhysicalDevices(s_h_instance, &c_devices, devices));
    for (uint32_t i = 0; i < c_devices && !s_h_physical; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            s_h_physical = devices[i];
            printf("GPU: %s (%s)\n", p.deviceName,
                   p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                         : "discrete");
        }
    }
    if (!s_h_physical) {
        fprintf(stderr, "no GPU\n");
        exit(1);
    }
    vkGetPhysicalDeviceMemoryProperties(s_h_physical, &s_memory_props);
    uint32_t families = 16;
    VkQueueFamilyProperties fp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(s_h_physical, &families, fp);
    s_i_family = UINT32_MAX;
    for (uint32_t i = 0; i < families; ++i) {
        if ((fp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (fp[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) {
            s_i_family = i;
            break;
        }
    }
    if (s_i_family == UINT32_MAX) {
        fprintf(stderr, "no graphics queue with sparse binding\n");
        exit(1);
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueFamilyIndex = s_i_family,
                                         .queueCount = 1,
                                         .pQueuePriorities = &priority};
    const char* extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    const VkPhysicalDeviceFeatures features = {.sparseBinding = VK_TRUE,
                                               .sparseResidencyBuffer = VK_TRUE};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &qci,
                                    .enabledExtensionCount = 2,
                                    .ppEnabledExtensionNames = extensions,
                                    .pEnabledFeatures = &features};
    CHECK(vkCreateDevice(s_h_physical, &dci, NULL, &s_h_device));
    vkGetDeviceQueue(s_h_device, s_i_family, 0, &s_h_queue);
    const VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                         .queueFamilyIndex = s_i_family};
    CHECK(vkCreateCommandPool(s_h_device, &pci, NULL, &s_h_pool));
    const VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = s_h_pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(s_h_device, &cai, &s_h_cmd));
    const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(s_h_device, &fci, NULL, &s_h_fence));
}

static void begin(void) {
    const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkResetCommandBuffer(s_h_cmd, 0));
    CHECK(vkBeginCommandBuffer(s_h_cmd, &bi));
}

static double submit_wait(void) {
    CHECK(vkEndCommandBuffer(s_h_cmd));
    const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &s_h_cmd};
    const double start = now_ms();
    CHECK(vkQueueSubmit(s_h_queue, 1, &si, s_h_fence));
    CHECK(vkWaitForFences(s_h_device, 1, &s_h_fence, VK_TRUE, UINT64_MAX));
    const double ms = now_ms() - start;
    CHECK(vkResetFences(s_h_device, 1, &s_h_fence));
    return ms;
}

static void barrier(void) {
    const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT |
                                                 VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(s_h_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
}

int main(int argc, char** argv) {
    const VkDeviceSize mib = argc > 1 ? strtoull(argv[1], NULL, 10) : 256;
    const VkDeviceSize cb_guest = mib << 20, cb_arena = 4ull << 30;
    init();

    /* The arena: a 4 GiB sparse buffer, as BufferCache creates it. */
    const VkBufferCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                                             VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                                    .size = cb_arena,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer h_arena;
    CHECK(vkCreateBuffer(s_h_device, &sci, NULL, &h_arena));
    VkMemoryRequirements areq;
    vkGetBufferMemoryRequirements(s_h_device, h_arena, &areq);
    printf("arena: block %llu KiB, memory types 0x%x\n", (unsigned long long)areq.alignment >> 10,
           areq.memoryTypeBits);

    /* Guest memory: host-visible, host-cached, not device-local, exported as a dma-buf. */
    const uint32_t host_type =
        find_type(areq.memoryTypeBits,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (host_type == UINT32_MAX) {
        printf("RESULT: no host-cached memory type can back the arena\n");
        return 2;
    }
    printf("guest memory type %u: flags 0x%x\n", host_type,
           s_memory_props.memoryTypes[host_type].propertyFlags);
    const VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkMemoryAllocateInfo gai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .pNext = &export_info,
                                      .allocationSize = cb_guest,
                                      .memoryTypeIndex = host_type};
    VkDeviceMemory h_guest_memory;
    CHECK(vkAllocateMemory(s_h_device, &gai, NULL, &h_guest_memory));
    const PFN_vkGetMemoryFdKHR pfn_get_fd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(s_h_device, "vkGetMemoryFdKHR");
    const VkMemoryGetFdInfoKHR fdi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                      .memory = h_guest_memory,
                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int h_fd = -1;
    CHECK(pfn_get_fd(s_h_device, &fdi, &h_fd));
    uint8_t* p_guest = mmap(NULL, cb_guest, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, 0);
    if (p_guest == MAP_FAILED) {
        perror("mmap dma-buf");
        return 1;
    }

    /* Bind the second half of the guest memory at an arena offset 1 GiB + 3 blocks in. */
    const VkDeviceSize half = cb_guest / 2, off_arena = (1ull << 30) + 3 * areq.alignment;
    const VkSparseMemoryBind bind = {.resourceOffset = off_arena,
                                     .size = half,
                                     .memory = h_guest_memory,
                                     .memoryOffset = half};
    const VkSparseBufferMemoryBindInfo bbi = {.buffer = h_arena, .bindCount = 1, .pBinds = &bind};
    const VkBindSparseInfo bsi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
                                  .bufferBindCount = 1,
                                  .pBufferBinds = &bbi};
    CHECK(vkQueueBindSparse(s_h_queue, 1, &bsi, s_h_fence));
    CHECK(vkWaitForFences(s_h_device, 1, &s_h_fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(s_h_device, 1, &s_h_fence));
    printf("bound %llu MiB of guest memory into the arena\n", (unsigned long long)(half >> 20));

    /* A device-local buffer to copy into. */
    const VkBufferCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = half,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer h_vram;
    CHECK(vkCreateBuffer(s_h_device, &vci, NULL, &h_vram));
    VkMemoryRequirements vreq;
    vkGetBufferMemoryRequirements(s_h_device, h_vram, &vreq);
    const VkMemoryAllocateInfo vai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = vreq.size,
        .memoryTypeIndex = find_type(vreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    VkDeviceMemory h_vram_memory;
    CHECK(vkAllocateMemory(s_h_device, &vai, NULL, &h_vram_memory));
    CHECK(vkBindBufferMemory(s_h_device, h_vram, h_vram_memory, 0));

    /* 1. CPU writes through the dma-buf mapping, the GPU copies through the arena. */
    uint32_t* p_words = (uint32_t*)(p_guest + half);
    for (VkDeviceSize i = 0; i < half / 4; ++i) p_words[i] = (uint32_t)(i * 2654435761u);
    begin();
    const VkBufferCopy to_vram = {.srcOffset = off_arena, .dstOffset = 0, .size = half};
    vkCmdCopyBuffer(s_h_cmd, h_arena, h_vram, 1, &to_vram);
    barrier();
    /* 2. The GPU writes through the arena, the CPU reads through the mapping. */
    vkCmdFillBuffer(s_h_cmd, h_arena, off_arena + 4096, 4096, 0xabcd1234u);
    barrier();
    const VkBufferCopy back = {.srcOffset = 0, .dstOffset = off_arena + 8192, .size = 4096};
    vkCmdCopyBuffer(s_h_cmd, h_vram, h_arena, 1, &back);
    barrier();
    submit_wait();
    int is_ok = 1;
    for (int i = 0; i < 1024; ++i) is_ok &= p_words[1024 + i] == 0xabcd1234u;
    for (int i = 0; i < 1024; ++i) is_ok &= p_words[2048 + i] == (uint32_t)(i * 2654435761u);
    printf("CPU write -> GPU read -> GPU write -> CPU read: %s\n", is_ok ? "ok" : "MISMATCH");

    /* 3. GPU read bandwidth: guest memory through the arena vs device-local memory. */
    double host_ms = 1e9, vram_ms = 1e9;
    for (int run = 0; run < 5; ++run) {
        begin();
        vkCmdCopyBuffer(s_h_cmd, h_arena, h_vram, 1, &to_vram);
        const double h = submit_wait();
        host_ms = h < host_ms ? h : host_ms;
        begin();
        const VkBufferCopy within = {.srcOffset = 0, .dstOffset = half / 2, .size = half / 2};
        vkCmdCopyBuffer(s_h_cmd, h_vram, h_vram, 1, &within);
        const double v = submit_wait() * 2; /* the same bytes as the copy above */
        vram_ms = v < vram_ms ? v : vram_ms;
    }
    printf("GPU reads %llu MiB: guest memory %.2f ms (%.1f GB/s), device-local %.2f ms (%.1f GB/s)\n",
           (unsigned long long)(half >> 20), host_ms, half / host_ms / 1e6, vram_ms,
           half / vram_ms / 1e6);
    printf("RESULT: %s\n", is_ok ? "guest memory can back the arena" : "data mismatch");
    return is_ok ? 0 : 1;
}
