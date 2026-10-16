// psport: glue between the C loader and the vendored shadPS4 video core.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../psport_gpu.h"
#include "common/elf_info.h"
#include "common/thread.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/signals.h"
#include "game_profile.h"
#include "psport_free_check.h"
#include "psport_settings.h"
#include "sdl_window.h"

extern "C" {
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
} // namespace Core

namespace Libraries::Kernel {
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* p_ver) {
    if (!p_ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *p_ver = s32(s_sdk_version);
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
    // What psport knows about this game (games/), before anything asks for it: the settings
    // among them, which keep its title's effect switches.
    Game::Select(p_config->sz_serial);
    PsSettings::Load();
    if (const Game::Title* p_title = Game::Launched(); p_title && p_title->pfn_prepare_saves) {
        const std::string saves = std::string(runtime_file_user_dir()) + "/savedata";
        p_title->pfn_prepare_saves(saves.c_str(), runtime_savedata_title());
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
        if (!p_window->IsCreated()) {
            delete p_window;
            {
                std::scoped_lock lock{s_window_mutex};
                s_is_window_ready = true;
            }
            s_window_cv.notify_all();
            return;
        }
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
        if (!g_window) return -1;
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

extern "C" unsigned psport_gpu_symbol_count(void) {
    return unsigned(s_symbols.size());
}
