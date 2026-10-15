// psport: stand-ins for the shadPS4 parts the port replaces (memory manager, address space,
// kernel services and RenderDoc hooks), over the C runtime.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "common/rdtsc.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "psport_copy.h"
#include "psport_toggles.h"
#include "psport_write_log.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t va_range, uint64_t cb_range);
int runtime_memory_write_backing(uintptr_t va_range, const void* p_data, uint64_t cb_range);
void runtime_memory_read_backing(uintptr_t va_range, void* p_data, uint64_t cb_range);
uint64_t runtime_memory_clamp(uintptr_t va_range, uint64_t cb_range);
int runtime_memory_region(uintptr_t va_guest, uintptr_t* p_start, uintptr_t* p_end, int* p_mapped);
const uint64_t* runtime_memory_generation(void);
void runtime_memory_gpu_protect(uintptr_t va_range, uint64_t cb_range, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t va_range, uint64_t cb_range);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange pfn_map, RuntimeGpuRange pfn_unmap, RuntimeGpuRange pfn_invalidate);
void runtime_memory_set_note_write_hook(RuntimeGpuRange pfn_note);
void runtime_memory_set_cpu_write_hook(RuntimeGpuRange pfn_hook);
}

namespace Core {
void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t va_range, uint64_t cb_range) {
            auto* p_rasterizer = Memory::Instance()->GetRasterizer();
            p_rasterizer->RegisterMemory(va_range, cb_range);
            p_rasterizer->MapMemory(va_range, cb_range);
        },
        [](uintptr_t va_range, uint64_t cb_range) { Memory::Instance()->GetRasterizer()->UnmapMemory(va_range, cb_range); },
        [](uintptr_t va_range, uint64_t cb_range) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(va_range, cb_range);
        });
    // psport: data written into GPU memory by a path the GPU side hears of (file reads, the game's
    // resource loaders): invalidated, and an asset (it may keep a VRAM copy).
    runtime_memory_set_note_write_hook([](uintptr_t va_range, uint64_t cb_range) {
        Memory::Instance()->GetRasterizer()->NoteAssetWrite(va_range, cb_range);
    });
    // psport: the game's libc copies (memcpy, memset, memmove) over pages the caches watch.
    runtime_memory_set_cpu_write_hook([](uintptr_t va_range, uint64_t cb_range) {
        Memory::Instance()->GetRasterizer()->OnCpuWrite(va_range, cb_range);
    });
}
void MemoryManager::InvalidateMemory(VAddr va_range, u64 cb_range) {
    if (rasterizer) rasterizer->InvalidateMemory(va_range, cb_range);
}
namespace {
// psport: the mapped region of the last lookup, per thread, valid for the mapping table generation
// it was read at. The GPU command thread clamps and copies thousands of constant ranges a frame,
// nearly all within the region of the one before: two calls into the runtime each (~5% of it).
struct RegionCache {
    u64 generation = 1; // odd: invalid
    uintptr_t start = 0, end = 0;
};
thread_local RegionCache s_region_cache;

bool CachedMapped(VAddr va_range, u64 cb_range) {
    static const uint64_t* const s_p_generation_ptr = runtime_memory_generation();
    const u64 generation = __atomic_load_n(s_p_generation_ptr, __ATOMIC_ACQUIRE);
    if (generation & 1) {
        return false; // being changed
    }
    auto& cache = s_region_cache;
    if (cache.generation == generation && va_range >= cache.start && va_range + cb_range <= cache.end) {
        return true;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(va_range, &start, &end, &mapped) || !mapped) {
        return false;
    }
    cache = {generation, start, end};
    return va_range + cb_range <= end;
}
} // namespace

u64 MemoryManager::ClampRangeSize(VAddr va_virtual, u64 cb_range) {
    if (cb_range && CachedMapped(va_virtual, cb_range)) {
        return cb_range;
    }
    return runtime_memory_clamp(va_virtual, cb_range);
}
static void CopySparseSerial(VAddr source, u8* p_dest, u64 cb_range) {
    // PS_READBACKS=2 protects GPU-written pages against reads: the copy threads and recorders
    // read through the backing view, as a read fault on them would wait for the GPU thread,
    // which waits for them.
    static const bool s_is_precise =
        EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise;
    if (s_is_precise) {
        runtime_memory_read_backing(source, p_dest, cb_range);
        return;
    }
    if (cb_range && CachedMapped(source, cb_range)) {
        std::memcpy(p_dest, reinterpret_cast<const void*>(source), cb_range);
        return;
    }
    while (cb_range) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + cb_range;
        }
        const u64 cb_chunk = std::min<u64>(cb_range, end - source);
        if (mapped) std::memcpy(p_dest, reinterpret_cast<const void*>(source), cb_chunk);
        else std::memset(p_dest, 0, cb_chunk);
        source += cb_chunk; p_dest += cb_chunk; cb_range -= cb_chunk;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* p_dest, u64 cb_range) {
    // psport: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (cb_range < 4 * Chunk || !PsCopy::Enabled()) {
        return CopySparseSerial(source, p_dest, cb_range);
    }
    PsCopy::ParallelFor((cb_range + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 off_chunk = i * Chunk;
        CopySparseSerial(source + off_chunk, p_dest + off_chunk, std::min(Chunk, cb_range - off_chunk));
    });
}
void MemoryManager::ReadBacking(VAddr va_range, void* p_data, u64 cb_range) {
    runtime_memory_read_backing(va_range, p_data, cb_range);
}
bool MemoryManager::TryWriteBacking(void* p_address, const void* p_data, u64 cb_range) {
    PsWriteLog::Note(reinterpret_cast<uintptr_t>(p_address), p_data, cb_range, PsWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(p_address), p_data, cb_range) != 0;
}
void AddressSpace::Protect(VAddr va_virtual, u64 cb_range, MemoryPermission perms) {
    PsStats::Timer timer{PsStats::g_t_protect};
    const u64 pages = (cb_range + 4095) / 4096;
    PsStats::g_c_protect_calls.fetch_add(1, std::memory_order_relaxed);
    PsStats::g_c_protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        PsStats::g_c_protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        PsStats::g_c_protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(va_virtual, cb_range, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core
// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return runtime_tsc_frequency(); }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel
// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex s_asio_req;
static std::condition_variable_any s_cv_asio_req;
static std::atomic<u32> s_c_asio_requests;
static std::jthread s_service_thread;
void KernelSignalRequest() {
    std::unique_lock lock{s_asio_req};
    ++s_c_asio_requests;
    s_cv_asio_req.notify_one();
}
static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{s_asio_req};
            s_cv_asio_req.wait(lock, stoken, [] { return s_c_asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        s_c_asio_requests = 0;
    }
}
void StartKernelService() {
    s_service_thread = std::jthread{KernelServiceThread};
}
int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore
