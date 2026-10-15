#include "psport_write_log.h"
// psport: glue between the C loader and the vendored shadPS4 video core.
#include "psport_overlay.h"
#include "psport_settings.h"
#include "game_profile.h"
#include "psport_copy.h"
#include <sys/resource.h>
#include "psport_free_check.h"
#include "psport_toggles.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../psport_gpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
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
// runtime_calls.c
void runtime_calls_report(void);
const char* runtime_file_user_dir(void);
const char* runtime_savedata_title(void);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::Ngs2 { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::Fiber { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::Font { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::FontFt { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::Audiodec { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::Videodec2 { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::PngEnc { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::PngDec { void RegisterLib(Core::Loader::SymbolsResolver* p_sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* p_sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 va_guest;
};
std::vector<Symbol> s_symbols;
u32 s_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* sz_nid, const char* sz_library, const char* sz_module, SymbolType,
                                u64 va_guest) {
    s_symbols.push_back({sz_nid, sz_library, sz_module, va_guest});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const PsGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.sz_serial ? config.sz_serial : "UNKNOWN";
        info.title = config.sz_title ? config.sz_title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

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
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* p_ver) {
    if (!p_ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *p_ver = s32(s_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread s_window_thread;
std::mutex s_window_mutex;
std::condition_variable s_window_cv;
bool s_is_window_ready;
} // namespace

#ifdef PS_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (PS_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int psport_gpu_init(const PsGpuConfig* p_config) {
    // What the translator knows about this game (games/), before anything asks for it: the
    // settings among them, which keep its effect switches.
    Game::Select(p_config->sz_serial);
    PsSettings::Load();
    if (const Game::Profile* p_game = Game::Active(); p_game && p_game->prepare_saves) {
        const std::string saves = std::string(runtime_file_user_dir()) + "/savedata";
        p_game->prepare_saves(saves.c_str(), runtime_savedata_title());
    }
#ifdef PS_PGO_GENERATE
    StartProfileWriter();
#endif
    s_sdk_version = p_config->sdk_version;
    if (p_config->sz_user_dir) setenv("PS_GPU_USER_DIR", p_config->sz_user_dir, 0);
    Core::Emulator::FillElfInfo(*p_config);
    const std::string title = p_config->sz_title && *p_config->sz_title ? p_config->sz_title
                                  : p_config->sz_serial ? p_config->sz_serial : "UNKNOWN";
    const std::string icon = std::string(p_config->sz_app0 ? p_config->sz_app0 : ".") + "/sce_sys/icon0.png";
    const s32 width = p_config->width, height = p_config->height;
    s_window_thread = std::thread([title, icon, width, height] {
        Common::SetCurrentThreadName("bb:window");
        auto* p_window = new Frontend::WindowSDL(width, height, title.c_str(), icon.c_str());
        {
            std::scoped_lock lock{s_window_mutex};
            g_window = p_window;
            s_is_window_ready = true;
        }
        s_window_cv.notify_all();
        while (p_window->PollEvents()) {
            SDL_Delay(2);
        }
        std::fflush(stdout);
        runtime_calls_report();
        std::_Exit(0);
    });
    s_window_thread.detach();
    {
        std::unique_lock lock{s_window_mutex};
        s_window_cv.wait(lock, [] { return s_is_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void psport_gpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
    Libraries::Ngs2::RegisterLib(&resolver);
    Libraries::Fiber::RegisterLib(&resolver);
    Libraries::Font::RegisterLib(&resolver);
    Libraries::FontFt::RegisterLib(&resolver);
    Libraries::Audiodec::RegisterLib(&resolver);
    Libraries::Videodec2::RegisterLib(&resolver);
    Libraries::PngEnc::RegisterLib(&resolver);
    Libraries::PngDec::RegisterLib(&resolver);
}

extern "C" uintptr_t psport_gpu_resolve(const char* sz_scoped_nid) {
    const char* sz_hash = std::strchr(sz_scoped_nid, '#');
    const size_t length = sz_hash ? size_t(sz_hash - sz_scoped_nid) : std::strlen(sz_scoped_nid);
    for (const auto& symbol : s_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), sz_scoped_nid, length)) {
            return uintptr_t(symbol.va_guest);
        }
    }
    return 0;
}

extern "C" int psport_gpu_handle_fault(void* p_ucontext, void* p_address) {
    // PS_LABEL_TRAP (diagnostic): its read-only label pages first. Not passed on to GPU page
    // tracking (a write fault drains the draw pipe: thousands a second would change the timing
    // under test); label pages are not expected to be GPU-tracked.
    if (PsFreeCheck::OnTrapFault(p_ucontext, reinterpret_cast<std::uint64_t>(p_address))) {
        return 1;
    }
    if (Core::Signals::Instance()->DispatchAccessViolation(p_ucontext, p_address)) {
        return 1;
    }
    return PsFreeCheck::OnStaleTrapFault(reinterpret_cast<std::uint64_t>(p_address)) ? 1 : 0;
}

extern "C" void psport_gpu_patch_image(unsigned char* p_image, uint64_t cb_image) {
    if (const Game::Profile* p_game = Game::Active(); p_game && p_game->patch_image) {
        p_game->patch_image(p_image, cb_image);
    }
}

extern "C" const char* psport_gpu_game_serial(const char* sz_serial, size_t i_serial) {
    const Game::Profile* p_game = Game::Find(sz_serial);
    return p_game && i_serial < p_game->serials.size() ? p_game->serials[i_serial] : nullptr;
}

extern "C" const char* psport_gpu_game_mod_folder(const char* sz_serial, size_t i_folder) {
    const Game::Profile* p_game = Game::Find(sz_serial);
    return p_game && i_folder < p_game->mod_folders.size() ? p_game->mod_folders[i_folder] : nullptr;
}

extern "C" const char* psport_gpu_game_patch_file(const char* sz_serial) {
    const Game::Profile* p_game = Game::Find(sz_serial);
    return p_game ? p_game->patch_file : nullptr;
}

extern "C" void psport_gpu_prepare_game(const PsGameFiles* p_files) {
    if (const Game::Profile* p_game = Game::Find(p_files->sz_serial); p_game && p_game->prepare_environment) {
        p_game->prepare_environment();
    }
}

extern "C" int psport_gpu_check_game(const PsGameFiles* p_files, char* sz_out_kind, size_t cb_kind, char* sz_out_advice,
                                size_t cb_advice) {
    const Game::Profile* p_game = Game::Find(p_files->sz_serial);
    std::string kind, advice;
    if (!p_game || !p_game->check_files || p_game->check_files(*p_files, kind, advice)) {
        return 0;
    }
    std::snprintf(sz_out_kind, cb_kind, "%s", kind.c_str());
    std::snprintf(sz_out_advice, cb_advice, "%s", advice.c_str());
    return 1;
}

extern "C" int psport_gpu_game_has_presets(const char* sz_serial) {
    const Game::Profile* p_game = Game::Find(sz_serial);
    return p_game && p_game->add_patches ? 1 : 0;
}

extern "C" int psport_gpu_game_patches(const PsGameFiles* p_files, const PsPatchServices* p_services) {
    const Game::Profile* p_game = Game::Find(p_files->sz_serial);
    if (!p_game || !p_game->add_patches) {
        return -1;
    }
    return p_game->add_patches(*p_files, *p_services) ? 1 : 0;
}

extern "C" unsigned psport_gpu_symbol_count(void) {
    return unsigned(s_symbols.size());
}

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

extern "C" int psport_gpu_overlay_captures_input(void) {
    return PsOverlay::CapturesInput() ? 1 : 0;
}

extern "C" int psport_gpu_overlay_pad_file_press(int guide, int cross, int circle) {
    return PsOverlay::PadFilePress(guide != 0, cross != 0, circle != 0) ? 1 : 0;
}

extern "C" void psport_gpu_mouse_take(double* p_dx, double* p_dy, int* p_wheel_up, int* p_wheel_down) {
    double x = 0, y = 0;
    int is_up = 0, is_down = 0;
    if (g_window) {
        g_window->TakeMouse(x, y, is_up, is_down);
    }
    *p_dx = x;
    *p_dy = y;
    *p_wheel_up = is_up;
    *p_wheel_down = is_down;
}

extern "C" int psport_gpu_mouse_captured(void) {
    return g_window && g_window->MouseCaptured() ? 1 : 0;
}

extern "C" int psport_gpu_game_mouse_take(int32_t* p_dx, int32_t* p_dy, int32_t* p_wheel, int32_t* p_tilt, uint32_t* p_buttons) {
    *p_dx = *p_dy = *p_wheel = *p_tilt = 0;
    *p_buttons = 0;
    return g_window && g_window->TakeGameMouse(*p_dx, *p_dy, *p_wheel, *p_tilt, *p_buttons) ? 1 : 0;
}

extern "C" void psport_gpu_game_keyboard_open(int is_open) {
    Frontend::WindowSDL::OpenGameKeyboard(is_open != 0);
}

extern "C" int psport_gpu_game_keyboard_take(PsKeyEvent* p_events, int max, int* p_keyboard, uint32_t* p_dropped) {
    bool is_host = false;
    u32 lost = 0;
    const int c_keys = g_window ? g_window->TakeGameKeys(p_events, max, is_host, lost) : 0;
    *p_keyboard = is_host ? 1 : 0;
    *p_dropped = lost;
    return c_keys;
}

extern "C" void psport_gpu_mouse_look_enable(int is_enabled) {
    Frontend::WindowSDL::EnableMouseLook(is_enabled != 0);
}

extern "C" int psport_gpu_text_input_begin(const char* sz_initial, const char* sz_prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(sz_initial ? sz_initial : "", sz_prompt ? sz_prompt : "Text");
    return 1;
}

extern "C" void psport_gpu_text_input_end(void) {
    if (g_window) g_window->EndTextInput();
}

extern "C" int psport_gpu_text_input_poll(char* sz_out, uint64_t cb_out) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (cb_out) {
        const size_t cb_copy = std::min<size_t>(text.size(), cb_out - 1);
        std::memcpy(sz_out, text.data(), cb_copy);
        sz_out[cb_copy] = 0;
    }
    return state;
}
