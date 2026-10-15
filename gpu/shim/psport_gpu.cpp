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
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
const uint64_t* runtime_memory_generation(void);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
void runtime_memory_set_note_write_hook(RuntimeGpuRange note);
void runtime_memory_set_cpu_write_hook(RuntimeGpuRange hook);
// runtime_calls.c
void runtime_calls_report(void);
const char* runtime_file_user_dir(void);
const char* runtime_savedata_title(void);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Ngs2 { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Fiber { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Font { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::FontFt { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Audiodec { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Videodec2 { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::PngEnc { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::PngDec { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const PsGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
    // psport: data written into GPU memory by a path the GPU side hears of (file reads, the game's
    // resource loaders): invalidated, and an asset (it may keep a VRAM copy).
    runtime_memory_set_note_write_hook([](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->NoteAssetWrite(address, size);
    });
    // psport: the game's libc copies (memcpy, memset, memmove) over pages the caches watch.
    runtime_memory_set_cpu_write_hook([](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->OnCpuWrite(address, size);
    });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
namespace {
// psport: the mapped region of the last lookup, per thread, valid for the mapping table generation
// it was read at. The GPU command thread clamps and copies thousands of constant ranges a frame,
// nearly all within the region of the one before: two calls into the runtime each (~5% of it).
struct RegionCache {
    u64 generation = 1; // odd: invalid
    uintptr_t start = 0, end = 0;
};
thread_local RegionCache region_cache;

bool CachedMapped(VAddr address, u64 size) {
    static const uint64_t* const generation_ptr = runtime_memory_generation();
    const u64 generation = __atomic_load_n(generation_ptr, __ATOMIC_ACQUIRE);
    if (generation & 1) {
        return false; // being changed
    }
    auto& cache = region_cache;
    if (cache.generation == generation && address >= cache.start && address + size <= cache.end) {
        return true;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    cache = {generation, start, end};
    return address + size <= end;
}
} // namespace

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    if (size && CachedMapped(virtual_addr, size)) {
        return size;
    }
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    // PS_READBACKS=2 protects GPU-written pages against reads: the copy threads and recorders
    // read through the backing view, as a read fault on them would wait for the GPU thread,
    // which waits for them.
    static const bool precise =
        EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise;
    if (precise) {
        runtime_memory_read_backing(source, dest, size);
        return;
    }
    if (size && CachedMapped(source, size)) {
        std::memcpy(dest, reinterpret_cast<const void*>(source), size);
        return;
    }
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // psport: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !PsCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    PsCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
void MemoryManager::ReadBacking(VAddr address, void* data, u64 size) {
    runtime_memory_read_backing(address, data, size);
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    PsWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, PsWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    PsStats::Timer timer{PsStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    PsStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    PsStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        PsStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        PsStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
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
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
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

extern "C" int psport_gpu_init(const PsGpuConfig* config) {
    // What the translator knows about this game (games/), before anything asks for it: the
    // settings among them, which keep its effect switches.
    Game::Select(config->serial);
    PsSettings::Load();
    if (const Game::Profile* game = Game::Active(); game && game->prepare_saves) {
        const std::string saves = std::string(runtime_file_user_dir()) + "/savedata";
        game->prepare_saves(saves.c_str(), runtime_savedata_title());
    }
#ifdef PS_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
    if (config->user_dir) setenv("PS_GPU_USER_DIR", config->user_dir, 0);
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title && *config->title ? config->title
                                  : config->serial ? config->serial : "UNKNOWN";
    const std::string icon = std::string(config->app0 ? config->app0 : ".") + "/sce_sys/icon0.png";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, icon, width, height] {
        Common::SetCurrentThreadName("bb:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str(), icon.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        std::fflush(stdout);
        runtime_calls_report();
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
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

extern "C" uintptr_t psport_gpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int psport_gpu_handle_fault(void* ucontext, void* address) {
    // PS_LABEL_TRAP (diagnostic): its read-only label pages first. Not passed on to GPU page
    // tracking (a write fault drains the draw pipe: thousands a second would change the timing
    // under test); label pages are not expected to be GPU-tracked.
    if (PsFreeCheck::OnTrapFault(ucontext, reinterpret_cast<std::uint64_t>(address))) {
        return 1;
    }
    if (Core::Signals::Instance()->DispatchAccessViolation(ucontext, address)) {
        return 1;
    }
    return PsFreeCheck::OnStaleTrapFault(reinterpret_cast<std::uint64_t>(address)) ? 1 : 0;
}

extern "C" void psport_gpu_patch_image(unsigned char* image, uint64_t size) {
    if (const Game::Profile* game = Game::Active(); game && game->patch_image) {
        game->patch_image(image, size);
    }
}

extern "C" const char* psport_gpu_game_serial(const char* serial, size_t index) {
    const Game::Profile* game = Game::Find(serial);
    return game && index < game->serials.size() ? game->serials[index] : nullptr;
}

extern "C" const char* psport_gpu_game_mod_folder(const char* serial, size_t index) {
    const Game::Profile* game = Game::Find(serial);
    return game && index < game->mod_folders.size() ? game->mod_folders[index] : nullptr;
}

extern "C" const char* psport_gpu_game_patch_file(const char* serial) {
    const Game::Profile* game = Game::Find(serial);
    return game ? game->patch_file : nullptr;
}

extern "C" void psport_gpu_prepare_game(const PsGameFiles* files) {
    if (const Game::Profile* game = Game::Find(files->sz_serial); game && game->prepare_environment) {
        game->prepare_environment();
    }
}

extern "C" int psport_gpu_check_game(const PsGameFiles* files, char* sz_out_kind, size_t cb_kind, char* sz_out_advice,
                                size_t cb_advice) {
    const Game::Profile* game = Game::Find(files->sz_serial);
    std::string kind, advice;
    if (!game || !game->check_files || game->check_files(*files, kind, advice)) {
        return 0;
    }
    std::snprintf(sz_out_kind, cb_kind, "%s", kind.c_str());
    std::snprintf(sz_out_advice, cb_advice, "%s", advice.c_str());
    return 1;
}

extern "C" int psport_gpu_game_has_presets(const char* serial) {
    const Game::Profile* game = Game::Find(serial);
    return game && game->add_patches ? 1 : 0;
}

extern "C" int psport_gpu_game_patches(const PsGameFiles* files, const PsPatchServices* services) {
    const Game::Profile* game = Game::Find(files->sz_serial);
    if (!game || !game->add_patches) {
        return -1;
    }
    return game->add_patches(*files, *services) ? 1 : 0;
}

extern "C" unsigned psport_gpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
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

extern "C" void psport_gpu_mouse_take(double* dx, double* dy, int* wheel_up, int* wheel_down) {
    double x = 0, y = 0;
    int up = 0, down = 0;
    if (g_window) {
        g_window->TakeMouse(x, y, up, down);
    }
    *dx = x;
    *dy = y;
    *wheel_up = up;
    *wheel_down = down;
}

extern "C" int psport_gpu_mouse_captured(void) {
    return g_window && g_window->MouseCaptured() ? 1 : 0;
}

extern "C" int psport_gpu_game_mouse_take(int32_t* dx, int32_t* dy, int32_t* wheel, int32_t* tilt, uint32_t* buttons) {
    *dx = *dy = *wheel = *tilt = 0;
    *buttons = 0;
    return g_window && g_window->TakeGameMouse(*dx, *dy, *wheel, *tilt, *buttons) ? 1 : 0;
}

extern "C" void psport_gpu_game_keyboard_open(int open) {
    Frontend::WindowSDL::OpenGameKeyboard(open != 0);
}

extern "C" int psport_gpu_game_keyboard_take(PsKeyEvent* events, int max, int* keyboard, uint32_t* dropped) {
    bool host = false;
    u32 lost = 0;
    const int count = g_window ? g_window->TakeGameKeys(events, max, host, lost) : 0;
    *keyboard = host ? 1 : 0;
    *dropped = lost;
    return count;
}

extern "C" void psport_gpu_mouse_look_enable(int enabled) {
    Frontend::WindowSDL::EnableMouseLook(enabled != 0);
}

extern "C" int psport_gpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" void psport_gpu_text_input_end(void) {
    if (g_window) g_window->EndTextInput();
}

extern "C" int psport_gpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
