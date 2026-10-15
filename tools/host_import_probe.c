/* Probe for importing guest memory into Vulkan (VK_EXT_external_memory_host).
 *
 * Guest memory is a memfd mapped twice: at guest addresses (page protection for write tracking)
 * and as an unprotected backing view. This checks, on the running GPU, whether the backing view
 * of a memfd can be imported, how fast the GPU reads it compared with today's path (CPU copy
 * into a staging buffer, then a GPU copy), whether CPU writes through the other mapping are
 * seen without extra steps, and what mprotect on the guest view costs the imported memory.
 *
 * Usage: bb-host-import-probe [MiB]   (default 512)
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
static PFN_vkGetMemoryHostPointerPropertiesEXT s_pfn_get_host_pointer_props;

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
    s_h_physical = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < c_devices; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            s_h_physical = devices[i];
            printf("GPU: %s\n", p.deviceName);
            break;
        }
    }
    if (!s_h_physical) {
        fprintf(stderr, "no GPU\n");
        exit(1);
    }
    vkGetPhysicalDeviceMemoryProperties(s_h_physical, &s_memory_props);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                          .pNext = &host_props};
    vkGetPhysicalDeviceProperties2(s_h_physical, &props2);
    printf("minImportedHostPointerAlignment: %llu\n",
           (unsigned long long)host_props.minImportedHostPointerAlignment);
    s_i_family = 0;
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueFamilyIndex = s_i_family,
                                         .queueCount = 1,
                                         .pQueuePriorities = &priority};
    const char* extensions[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .synchronization2 = 1};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .pNext = &f13,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &qci,
                                    .enabledExtensionCount = 3,
                                    .ppEnabledExtensionNames = extensions};
    CHECK(vkCreateDevice(s_h_physical, &dci, NULL, &s_h_device));
    vkGetDeviceQueue(s_h_device, s_i_family, 0, &s_h_queue);
    s_pfn_get_host_pointer_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
        s_h_device, "vkGetMemoryHostPointerPropertiesEXT");
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

typedef struct {
    VkBuffer h_buffer;
    VkDeviceMemory h_memory;
    void* p_mapped;
} Buffer;

static Buffer make_buffer(VkDeviceSize cb_buffer, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    Buffer b = {0};
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = cb_buffer,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(s_h_device, &bci, NULL, &b.h_buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s_h_device, b.h_buffer, &req);
    const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .allocationSize = req.size,
                                      .memoryTypeIndex = find_type(req.memoryTypeBits, want, avoid)};
    CHECK(vkAllocateMemory(s_h_device, &mai, NULL, &b.h_memory));
    CHECK(vkBindBufferMemory(s_h_device, b.h_buffer, b.h_memory, 0));
    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        CHECK(vkMapMemory(s_h_device, b.h_memory, 0, VK_WHOLE_SIZE, 0, &b.p_mapped));
    }
    return b;
}

/* Imports [pointer, pointer + size) as a transfer buffer; returns 0 on failure. */
static int import_buffer(void* p_host, VkDeviceSize cb_buffer, Buffer* p_out_buffer, double* p_out_ms) {
    VkMemoryHostPointerPropertiesEXT hpp = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = s_pfn_get_host_pointer_props(
        s_h_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p_host, &hpp);
    if (r != VK_SUCCESS || !hpp.memoryTypeBits) {
        printf("  host pointer properties: result %d, memory types %#x\n", r, hpp.memoryTypeBits);
        return 0;
    }
    const VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .pNext = &ext,
                                    .size = cb_buffer,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    CHECK(vkCreateBuffer(s_h_device, &bci, NULL, &p_out_buffer->h_buffer));
    const VkImportMemoryHostPointerInfoEXT import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = p_host};
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < s_memory_props.memoryTypeCount; ++i) {
        if (hpp.memoryTypeBits & (1u << i)) {
            type = i;
            break;
        }
    }
    const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .pNext = &import,
                                      .allocationSize = cb_buffer,
                                      .memoryTypeIndex = type};
    const double start = now_ms();
    r = vkAllocateMemory(s_h_device, &mai, NULL, &p_out_buffer->h_memory);
    *p_out_ms = now_ms() - start;
    if (r != VK_SUCCESS) {
        printf("  vkAllocateMemory(import) failed: %d\n", r);
        vkDestroyBuffer(s_h_device, p_out_buffer->h_buffer, NULL);
        return 0;
    }
    printf("  memory type %u (flags %#x)\n", type, s_memory_props.memoryTypes[type].propertyFlags);
    CHECK(vkBindBufferMemory(s_h_device, p_out_buffer->h_buffer, p_out_buffer->h_memory, 0));
    p_out_buffer->p_mapped = p_host;
    return 1;
}

/* Records `copies` copies of `size` bytes src -> dst, submits, waits; returns wall ms. */
static double gpu_copy(VkBuffer h_src, VkDeviceSize off_src, VkBuffer h_dst, VkDeviceSize cb_copy) {
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkResetCommandBuffer(s_h_cmd, 0));
    CHECK(vkBeginCommandBuffer(s_h_cmd, &begin));
    const VkBufferCopy region = {.srcOffset = off_src, .dstOffset = 0, .size = cb_copy};
    vkCmdCopyBuffer(s_h_cmd, h_src, h_dst, 1, &region);
    const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(s_h_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &barrier, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(s_h_cmd));
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .commandBufferCount = 1,
                                 .pCommandBuffers = &s_h_cmd};
    const double start = now_ms();
    CHECK(vkQueueSubmit(s_h_queue, 1, &submit, s_h_fence));
    CHECK(vkWaitForFences(s_h_device, 1, &s_h_fence, VK_TRUE, UINT64_MAX));
    const double ms = now_ms() - start;
    CHECK(vkResetFences(s_h_device, 1, &s_h_fence));
    return ms;
}

#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>

static volatile sig_atomic_t s_c_write_faults;
static uint8_t* s_p_fault_view;
static uint64_t s_cb_fault;
static void on_write_fault(int sig, siginfo_t* p_info, void* p_context) {
    (void)sig;
    (void)p_context;
    uint8_t* p_address = p_info->si_addr;
    if (p_address >= s_p_fault_view && p_address < s_p_fault_view + s_cb_fault) {
        ++s_c_write_faults;
        uint8_t* p_page = (uint8_t*)((uintptr_t)p_address & ~(uintptr_t)4095);
        mprotect(p_page, 4096, PROT_READ | PROT_WRITE);
        return;
    }
    signal(SIGSEGV, SIG_DFL);
}

/* CPU speed on a mapping: sequential write, sequential read, random 8-byte reads. */
static void cpu_speed(const char* sz_name, uint8_t* p_data, uint64_t cb_data) {
    double start = now_ms();
    memset(p_data, 0x5a, cb_data);
    const double write_ms = now_ms() - start;
    start = now_ms();
    uint64_t sum = 0;
    for (uint64_t i = 0; i < cb_data; i += 64) sum += p_data[i];
    const double read_ms = now_ms() - start;
    start = now_ms();
    uint64_t x = 88172645463325252ull;
    for (int i = 0; i < 4000000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        sum += p_data[(x % (cb_data / 8)) * 8];
    }
    const double random_ms = now_ms() - start;
    printf("  %-28s write %6.1f GB/s, read %6.1f GB/s, random reads %6.1f ns (%llu)\n", sz_name,
           cb_data / write_ms / 1e6, cb_data / read_ms / 1e6, random_ms * 1e6 / 4000000,
           (unsigned long long)(sum & 1));
}

/* Guest memory allocated by Vulkan (system memory the GPU reaches), exported as a dma-buf and
 * mapped by the CPU: no import, several views of the same memory, protection on the CPU views
 * only. */
static void dmabuf_test(uint64_t cb_buffer, Buffer local, Buffer readback) {
    printf("\n5. Vulkan-allocated guest memory exported as dma-buf (%llu MiB)\n",
           (unsigned long long)(cb_buffer >> 20));
    PFN_vkGetMemoryFdKHR pfn_get_fd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(s_h_device, "vkGetMemoryFdKHR");
    const VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .pNext = &ext,
                                    .size = cb_buffer,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer h_buffer;
    CHECK(vkCreateBuffer(s_h_device, &bci, NULL, &h_buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s_h_device, h_buffer, &req);
    const uint32_t type =
        find_type(req.memoryTypeBits,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        printf("  no cached system memory type for this buffer\n");
        return;
    }
    const VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .pNext = &export_info,
                                      .allocationSize = req.size,
                                      .memoryTypeIndex = type};
    VkDeviceMemory h_memory;
    double start = now_ms();
    VkResult r = vkAllocateMemory(s_h_device, &mai, NULL, &h_memory);
    printf("  allocate (type %u, flags %#x): %d, %.1f ms\n", type,
           s_memory_props.memoryTypes[type].propertyFlags, r, now_ms() - start);
    if (r != VK_SUCCESS) return;
    CHECK(vkBindBufferMemory(s_h_device, h_buffer, h_memory, 0));
    const VkMemoryGetFdInfoKHR gfi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                      .memory = h_memory,
                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int h_fd = -1;
    r = pfn_get_fd(s_h_device, &gfi, &h_fd);
    printf("  export dma-buf fd: %d (fd %d)\n", r, h_fd);
    if (r != VK_SUCCESS) return;
    uint8_t* p_guest = mmap(NULL, cb_buffer, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, 0);
    uint8_t* p_backing = mmap(NULL, cb_buffer, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, 0);
    if (p_guest == MAP_FAILED || p_backing == MAP_FAILED) {
        perror("  mmap of the dma-buf");
        return;
    }
    printf("  two CPU views mapped: OK\n");

    uint8_t* p_anon = mmap(NULL, cb_buffer, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    cpu_speed("ordinary memory", p_anon, cb_buffer);
    const int shm = memfd_create("bb-probe-speed", MFD_CLOEXEC);
    if (shm >= 0 && !ftruncate(shm, (off_t)cb_buffer)) {
        uint8_t* p_view = mmap(NULL, cb_buffer, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
        if (p_view != MAP_FAILED) cpu_speed("memfd (the guest memory now)", p_view, cb_buffer);
    }
    cpu_speed("dma-buf guest view", p_guest, cb_buffer);

    /* A view of part of it at a chosen address (guest mappings: MAP_FIXED at an offset). */
    uint8_t* p_hole = mmap(NULL, 8 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t* p_fixed = mmap(p_hole + (1 << 20), 4 << 20, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                          h_fd, (off_t)(256 << 20));
    if (p_fixed == MAP_FAILED) {
        perror("  MAP_FIXED view at an offset");
    } else {
        p_guest[(256 << 20) + 64] = 0x42;
        printf("  MAP_FIXED view at offset 256 MiB: %s\n", p_fixed[64] == 0x42 ? "OK" : "WRONG DATA");
    }
    /* Aliasing: a write through one view is visible through the other. */
    p_guest[12345] = 0x77;
    printf("  aliasing: %s\n", p_backing[12345] == 0x77 ? "OK" : "BROKEN");

    const VkDeviceSize chunk = 64ull << 20 < cb_buffer ? 64ull << 20 : cb_buffer;
    double best = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(h_buffer, 0, local.h_buffer, chunk);
        if (ms < best) best = ms;
    }
    printf("  GPU reads %llu MiB of it: %.2f ms (%.1f GB/s)\n", (unsigned long long)(chunk >> 20),
           best, chunk / best / 1e6);

    int bad = 0;
    for (int round = 0; round < 64; ++round) {
        const uint64_t off_page = ((uint64_t)rand() % (cb_buffer / 4096)) * 4096;
        const uint32_t value = 0x51ac0000u + (uint32_t)round;
        memcpy(p_guest + off_page, &value, 4);
        gpu_copy(h_buffer, off_page, readback.h_buffer, 4);
        uint32_t seen;
        memcpy(&seen, readback.p_mapped, 4);
        if (seen != value) ++bad;
    }
    printf("  coherence (CPU write, GPU read): %s (%d of 64 stale)\n", bad ? "STALE" : "OK", bad);

    double base = 1e9, after = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(h_buffer, 0, local.h_buffer, 4 << 20);
        if (ms < base) base = ms;
    }
    start = now_ms();
    for (int i = 0; i < 1000; ++i) {
        uint8_t* p_page = p_guest + ((uint64_t)i * 65536) % cb_buffer;
        mprotect(p_page, 65536, PROT_READ);
        mprotect(p_page, 65536, PROT_READ | PROT_WRITE);
    }
    const double protect_ms = now_ms() - start;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(h_buffer, 0, local.h_buffer, 4 << 20);
        if (ms < after) after = ms;
    }
    printf("  2000 mprotect calls on the guest view: %.1f ms; 4 MiB GPU read before %.3f ms, "
           "after %.3f ms\n", protect_ms, base, after);

    /* Write tracking as the GPU caches do it: protect, write, catch the fault, unprotect. */
    s_p_fault_view = p_guest;
    s_cb_fault = cb_buffer;
    struct sigaction sa = {0};
    sa.sa_sigaction = on_write_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    mprotect(p_guest, 1 << 20, PROT_READ);
    for (int i = 0; i < 256; ++i) p_guest[i * 4096] = 1;
    printf("  write faults caught after mprotect: %d of 256\n", (int)s_c_write_faults);

    /* File reads into the guest view (the game loads its data with read()). */
    char sz_path[] = "/tmp/bb-probe-file-XXXXXX";
    const int file = mkstemp(sz_path);
    static char s_data[1 << 20];
    memset(s_data, 0x3c, sizeof(s_data));
    if (file >= 0 && write(file, s_data, sizeof(s_data)) == (ssize_t)sizeof(s_data)) {
        const ssize_t got = pread(file, p_guest + (2 << 20), sizeof(s_data), 0);
        printf("  read() into the guest view: %zd bytes, %s\n", got,
               got == (ssize_t)sizeof(s_data) && p_guest[(2 << 20) + 100] == 0x3c ? "OK" : "FAILED");
        close(file);
        unlink(sz_path);
    }
}

int main(int argc, char** argv) {
    const uint64_t mib = argc > 1 ? strtoull(argv[1], NULL, 10) : 512;
    const uint64_t cb_guest = mib << 20;
    init();

    /* Guest memory as the runtime makes it: a memfd, a backing view, a guest view. */
    const int h_fd = memfd_create("bb-probe-guest", MFD_CLOEXEC);
    if (h_fd < 0 || ftruncate(h_fd, (off_t)cb_guest)) {
        perror("memfd");
        return 1;
    }
    uint8_t* p_backing = mmap(NULL, cb_guest, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, 0);
    uint8_t* p_guest = mmap(NULL, cb_guest, PROT_READ | PROT_WRITE, MAP_SHARED, h_fd, 0);
    if (p_backing == MAP_FAILED || p_guest == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    for (uint64_t i = 0; i < cb_guest; i += 4096) p_guest[i] = (uint8_t)(i >> 12);

    printf("\n1. Import of the memfd backing view (%llu MiB)\n", (unsigned long long)mib);
    Buffer imported;
    double import_ms = 0;
    const int is_memfd_ok = import_buffer(p_backing, cb_guest, &imported, &import_ms);
    printf("  %s (%.1f ms)\n", is_memfd_ok ? "OK" : "FAILED", import_ms);

    printf("\n1b. Import of anonymous memory (%llu MiB), for comparison\n", (unsigned long long)mib);
    uint8_t* p_anon = mmap(NULL, cb_guest, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(p_anon, 1, cb_guest);
    Buffer anon_buffer;
    double anon_ms = 0;
    const int is_anon_ok = import_buffer(p_anon, cb_guest, &anon_buffer, &anon_ms);
    printf("  %s (%.1f ms)\n", is_anon_ok ? "OK" : "FAILED", anon_ms);

    if (!is_memfd_ok) {
        if (!is_anon_ok) {
            printf("\nNeither memfd nor anonymous memory can be imported: stopping.\n");
            return 2;
        }
        // The rest on anonymous memory: there is no second view, so the "guest view" is the
        // imported range itself (protection changes hit the imported pages).
        printf("\nmemfd import failed: the tests below use the anonymous import.\n");
        imported = anon_buffer;
        p_backing = p_anon;
        p_guest = p_anon;
    }

    const VkDeviceSize chunk = 64ull << 20 < cb_guest ? 64ull << 20 : cb_guest;
    Buffer local = make_buffer(chunk, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    Buffer staging = make_buffer(chunk, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Buffer readback = make_buffer(4096, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);

    printf("\n2. Upload of %llu MiB into device-local memory\n", (unsigned long long)(chunk >> 20));
    gpu_copy(imported.h_buffer, 0, local.h_buffer, chunk); /* warm up */
    double direct = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(imported.h_buffer, (uint64_t)i * 4096 % (cb_guest - chunk + 1),
                                   local.h_buffer, chunk);
        if (ms < direct) direct = ms;
    }
    double cpu = 1e9, gpu = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double start = now_ms();
        memcpy(staging.p_mapped, p_guest, chunk);
        const double c = now_ms() - start;
        const double g = gpu_copy(staging.h_buffer, 0, local.h_buffer, chunk);
        if (c < cpu) cpu = c;
        if (g < gpu) gpu = g;
    }
    printf("  GPU reads the imported guest memory: %.2f ms (%.1f GB/s)\n", direct,
           chunk / direct / 1e6);
    printf("  today: CPU copy to staging %.2f ms + GPU copy %.2f ms = %.2f ms (CPU busy %.2f ms)\n",
           cpu, gpu, cpu + gpu, cpu);

    printf("\n3. Coherence: CPU writes through the guest view, GPU reads the imported view\n");
    int bad = 0;
    for (int round = 0; round < 64; ++round) {
        const uint64_t off_page = ((uint64_t)rand() % (cb_guest / 4096)) * 4096;
        const uint32_t value = 0x51ac0000u + (uint32_t)round;
        memcpy(p_guest + off_page, &value, 4);
        gpu_copy(imported.h_buffer, off_page, readback.h_buffer, 4);
        uint32_t seen;
        memcpy(&seen, readback.p_mapped, 4);
        if (seen != value) ++bad;
    }
    printf("  %s (%d of 64 rounds stale)\n", bad ? "STALE DATA" : "OK", bad);

    printf("\n4. mprotect on the guest view vs the imported backing view\n");
    double base = 1e9, after = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(imported.h_buffer, 0, local.h_buffer, 4 << 20);
        if (ms < base) base = ms;
    }
    const double protect_start = now_ms();
    for (int i = 0; i < 1000; ++i) {
        uint8_t* p_page = p_guest + ((uint64_t)i * 65536) % cb_guest;
        mprotect(p_page, 65536, PROT_READ);
        mprotect(p_page, 65536, PROT_READ | PROT_WRITE);
    }
    const double protect_ms = now_ms() - protect_start;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(imported.h_buffer, 0, local.h_buffer, 4 << 20);
        if (ms < after) after = ms;
    }
    printf("  2000 mprotect calls on the guest view: %.1f ms; 4 MiB GPU read before %.3f ms, "
           "after %.3f ms\n", protect_ms, base, after);
    /* The same on the imported view itself (what to avoid). */
    double own = 0;
    for (int i = 0; i < 3; ++i) {
        mprotect(p_backing, 65536, PROT_READ);
        mprotect(p_backing, 65536, PROT_READ | PROT_WRITE);
        own += gpu_copy(imported.h_buffer, 0, local.h_buffer, 4 << 20);
    }
    printf("  mprotect on the imported view itself, then a 4 MiB GPU read: %.3f ms average\n",
           own / 3);

    dmabuf_test(cb_guest, local, readback);
    printf("\nDone.\n");
    return 0;
}
