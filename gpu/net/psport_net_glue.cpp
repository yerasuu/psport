// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module (libpsport-net.so): what the network and NP libraries from shadp2p need
// from the rest of shadPS4, on top of the psport runtime (PsNetHost), their registration and the
// module's C interface (psport_net.h). From the Linux co-op fork of bbport, which took it from the
// Windows port; the libraries are shadp2p's (a shadPS4 fork, GPL-2.0).

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "common/elf_info.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "core/bloodborne_re.h"
#include "core/debugger.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/system/systemservice.h"
#include "core/libraries/system/userservice.h"
#include "core/user_manager.h"
#include "imgui/invitation_prompt_layer.h"
#include "imgui/shadnet_notifications_layer.h"

#include "core/libraries/network/http.h"
#include "core/libraries/network/http2.h"
#include "core/libraries/network/net.h"
#include "core/libraries/network/netctl.h"
#include "core/libraries/network/ssl.h"
#include "core/libraries/network/ssl2.h"
#include "core/libraries/np/np_auth.h"
#include "core/libraries/np/np_common.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/np/np_matching2/np_matching2.h"
#include "core/libraries/np/np_partner.h"
#include "core/libraries/np/np_party.h"
#include "core/libraries/np/np_score/np_score.h"
#include "core/libraries/np/np_signaling/np_signaling.h"
#include "core/libraries/np/np_tus.h"
#include "core/libraries/np/np_web_api/np_web_api.h"
#include "core/libraries/np/np_web_api2/np_web_api2.h"

#include "psport_net.h"

namespace {
/// The runtime's side (psport_net_init).
PsNetHost s_host{};
} // namespace

// ---- Kernel ---------------------------------------------------------------------------------

namespace Libraries::Kernel {

struct PsNetMutex {
    std::recursive_mutex mutex;
    bool is_recursive{};
    int depth{};
};
struct PsNetMutexAttr {
    PthreadMutexType type{PthreadMutexType::ErrorCheck};
};
struct PsNetCond {
    std::condition_variable_any cond;
};
struct PsNetCondAttr {};
struct PsNetThreadAttr {
    u64 stack{};
};

s32 ErrnoToSceKernelError(s32 e) {
    return e > 0 && e <= 0xffff ? s32(0x80020000u | u32(e)) : e;
}

s32 PS4_SYSV_ABI sceKernelGetSystemSwVersion(SwVersionStruct* p_ret) {
    if (!p_ret) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    std::memset(p_ret->text_representation, 0, sizeof(p_ret->text_representation));
    std::snprintf(p_ret->text_representation, sizeof(p_ret->text_representation), "%s", "11.000.000");
    p_ret->hex_representation = 0x11000001;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelClockGettime(u32 clock_id, OrbisKernelTimespec* p_kernel_timespec) {
    if (!p_kernel_timespec) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    s64 ns;
    if (clock_id == ORBIS_CLOCK_REALTIME || clock_id == ORBIS_CLOCK_REALTIME_PRECISE ||
        clock_id == ORBIS_CLOCK_REALTIME_FAST || clock_id == ORBIS_CLOCK_SECOND) {
        ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
    } else {
        ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count();
    }
    p_kernel_timespec->tv_sec = ns / 1000000000;
    p_kernel_timespec->tv_nsec = ns % 1000000000;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelMapNamedFlexibleMemory(void** pp_out_addr_in, u64 len, s32, s32,
                                                 const char*) {
    if (!pp_out_addr_in || !len) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    void* p_memory = std::calloc(1, len);
    if (!p_memory) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    *pp_out_addr_in = p_memory;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelMunmap(void* p_addr, u64) {
    std::free(p_addr);
    return ORBIS_OK;
}

int posix_pthread_mutexattr_init(PthreadMutexAttrT* pp_attr) {
    if (!pp_attr) return 22;
    *pp_attr = new PsNetMutexAttr;
    return 0;
}
int posix_pthread_mutexattr_settype(PthreadMutexAttrT* pp_attr, PthreadMutexType type) {
    if (!pp_attr || !*pp_attr) return 22;
    (*pp_attr)->type = type;
    return 0;
}
int posix_pthread_mutexattr_destroy(PthreadMutexAttrT* pp_attr) {
    if (!pp_attr) return 22;
    delete *pp_attr;
    *pp_attr = nullptr;
    return 0;
}
int scePthreadMutexInit(PthreadMutexT* pp_mutex, const PthreadMutexAttrT* pp_attr, const char*) {
    if (!pp_mutex) return 22;
    *pp_mutex = new PsNetMutex;
    (*pp_mutex)->is_recursive = pp_attr && *pp_attr && (*pp_attr)->type == PthreadMutexType::Recursive;
    return 0;
}
int posix_pthread_mutex_destroy(PthreadMutexT* pp_mutex) {
    if (!pp_mutex || !*pp_mutex) return 22;
    delete *pp_mutex;
    *pp_mutex = nullptr;
    return 0;
}
int posix_pthread_mutex_lock(PthreadMutexT* pp_mutex) {
    if (!pp_mutex || !*pp_mutex) return 22;
    (*pp_mutex)->mutex.lock();
    if (++(*pp_mutex)->depth > 1 && !(*pp_mutex)->is_recursive) {
        --(*pp_mutex)->depth;
        (*pp_mutex)->mutex.unlock();
        return 11; // EDEADLK
    }
    return 0;
}
int posix_pthread_mutex_trylock(PthreadMutexT* pp_mutex) {
    if (!pp_mutex || !*pp_mutex) return 22;
    if (!(*pp_mutex)->mutex.try_lock()) return 16; // EBUSY
    if (++(*pp_mutex)->depth > 1 && !(*pp_mutex)->is_recursive) {
        --(*pp_mutex)->depth;
        (*pp_mutex)->mutex.unlock();
        return 16;
    }
    return 0;
}
int posix_pthread_mutex_unlock(PthreadMutexT* pp_mutex) {
    if (!pp_mutex || !*pp_mutex) return 22;
    --(*pp_mutex)->depth;
    (*pp_mutex)->mutex.unlock();
    return 0;
}

int posix_pthread_condattr_init(PthreadCondAttrT* pp_attr) {
    if (!pp_attr) return 22;
    *pp_attr = new PsNetCondAttr;
    return 0;
}
int posix_pthread_condattr_destroy(PthreadCondAttrT* pp_attr) {
    if (!pp_attr) return 22;
    delete *pp_attr;
    *pp_attr = nullptr;
    return 0;
}
int scePthreadCondInit(PthreadCondT* pp_cond, const PthreadCondAttrT*, const char*) {
    if (!pp_cond) return 22;
    *pp_cond = new PsNetCond;
    return 0;
}
int posix_pthread_cond_destroy(PthreadCondT* pp_cond) {
    if (!pp_cond || !*pp_cond) return 22;
    delete *pp_cond;
    *pp_cond = nullptr;
    return 0;
}
int posix_pthread_cond_signal(PthreadCondT* pp_cond) {
    if (!pp_cond || !*pp_cond) return 22;
    (*pp_cond)->cond.notify_one();
    return 0;
}
namespace {
// The caller holds the mutex once; the wait releases and retakes it like pthread_cond_wait.
struct HeldMutex {
    PsNetMutex* m_p_mutex;
    void lock() {
        m_p_mutex->mutex.lock();
        ++m_p_mutex->depth;
    }
    void unlock() {
        --m_p_mutex->depth;
        m_p_mutex->mutex.unlock();
    }
};
} // namespace
int posix_pthread_cond_wait(PthreadCondT* pp_cond, PthreadMutexT* pp_mutex) {
    if (!pp_cond || !*pp_cond || !pp_mutex || !*pp_mutex) return 22;
    HeldMutex held{*pp_mutex};
    (*pp_cond)->cond.wait(held);
    return 0;
}
int posix_pthread_cond_reltimedwait_np(PthreadCondT* pp_cond, PthreadMutexT* pp_mutex, u64 usec) {
    if (!pp_cond || !*pp_cond || !pp_mutex || !*pp_mutex) return 22;
    HeldMutex held{*pp_mutex};
    const auto status = (*pp_cond)->cond.wait_for(held, std::chrono::microseconds(usec));
    return status == std::cv_status::timeout ? 60 : 0; // ETIMEDOUT
}

int posix_pthread_attr_init(PthreadAttrT* pp_attr) {
    if (!pp_attr) return 22;
    *pp_attr = new PsNetThreadAttr;
    return 0;
}
int posix_pthread_attr_destroy(PthreadAttrT* pp_attr) {
    if (!pp_attr) return 22;
    delete *pp_attr;
    *pp_attr = nullptr;
    return 0;
}
int posix_pthread_attr_setstacksize(PthreadAttrT* pp_attr, size_t cb_stack) {
    if (!pp_attr || !*pp_attr) return 22;
    (*pp_attr)->stack = cb_stack;
    return 0;
}
int posix_pthread_attr_setinheritsched(PthreadAttrT*, int) {
    return 0;
}
int posix_pthread_attr_setschedpolicy(PthreadAttrT*, SchedPolicy) {
    return 0;
}
int posix_pthread_attr_setschedparam(PthreadAttrT*, const SchedParam*) {
    return 0;
}
int scePthreadAttrSetaffinity(PthreadAttrT*, u64) {
    return 0;
}
int posix_pthread_create_name_np(PthreadT* pp_thread, const PthreadAttrT* pp_attr,
                                 PthreadEntryFunc pfn_start, void* p_arg, const char* sz_name) {
    if (!pp_thread || !pfn_start) return 22;
    const u64 stack = pp_attr && *pp_attr ? (*pp_attr)->stack : 0;
    return s_host.pfn_thread_spawn(pp_thread, pfn_start, p_arg, stack, sz_name ? sz_name : "np");
}
int posix_pthread_join(PthreadT p_thread, void** pp_ret) {
    return s_host.pfn_thread_join(p_thread, pp_ret);
}

} // namespace Libraries::Kernel

// ---- User and system service, debugger, Bloodborne helpers -------------------------------------

namespace Libraries::UserService {
s32 PS4_SYSV_ABI sceUserServiceGetInitialUser(int* p_user_id) {
    if (!p_user_id) {
        return s32(0x80960009); // SCE_USER_SERVICE_ERROR_INVALID_ARGUMENT
    }
    *p_user_id = UserManager::RuntimeUserId;
    return ORBIS_OK;
}
} // namespace Libraries::UserService

namespace Libraries::SystemService {
void PushSystemServiceEvent(const OrbisSystemServiceEvent& event) {
    std::printf("Co-op: system service event %#x not delivered (invitations are not ported)\n",
                unsigned(event.event_type));
}
} // namespace Libraries::SystemService

namespace Core::Debugger {
s32 GetCurrentPid() {
#ifdef _WIN32
    return s32(GetCurrentProcessId());
#else
    return s32(getpid());
#endif
}
} // namespace Core::Debugger

namespace Core::Bloodborne {
namespace {
std::mutex s_placement_mutex;
std::optional<std::string> s_placement;
} // namespace
std::optional<std::string> GetSeamlessHostPlacementHeader() {
    std::scoped_lock lock{s_placement_mutex};
    return s_placement;
}
bool SetSeamlessHostPlacementHeader(std::string_view value) {
    if (value.empty() || value.size() > 4096) {
        return false;
    }
    std::scoped_lock lock{s_placement_mutex};
    s_placement = std::string{value};
    return true;
}
void ClearSeamlessHostPlacementHeader() {
    std::scoped_lock lock{s_placement_mutex};
    s_placement.reset();
}
void TraceMatching2LeaveRoom(std::uintptr_t, std::uint64_t) {}
} // namespace Core::Bloodborne

// ---- Notifications (log only for now) ------------------------------------------------------------

namespace ImGui::ShadNetNotify {
namespace {
std::mutex s_history_mutex;
std::vector<HistoryEntry> s_history;
std::size_t s_unread{};
} // namespace
KindDisplay DisplayOf(Kind kind) {
    switch (kind) {
    case Kind::FriendRequest:
        return {"Friend request", {0.9f, 0.8f, 0.3f, 1.0f}};
    case Kind::FriendNew:
        return {"Friend", {0.4f, 0.9f, 0.4f, 1.0f}};
    case Kind::FriendLost:
        return {"Friend", {0.9f, 0.4f, 0.4f, 1.0f}};
    case Kind::Online:
        return {"Online", {0.4f, 0.8f, 0.9f, 1.0f}};
    default:
        return {"Info", {0.8f, 0.8f, 0.8f, 1.0f}};
    }
}
void Push(Kind kind, std::string text) {
    std::printf("Co-op: %s: %s\n", DisplayOf(kind).tag, text.c_str());
    std::scoped_lock lock{s_history_mutex};
    s_history.push_back({kind, std::move(text), {}});
    ++s_unread;
}
std::vector<HistoryEntry> GetHistory() {
    std::scoped_lock lock{s_history_mutex};
    return s_history;
}
void ClearHistory() {
    std::scoped_lock lock{s_history_mutex};
    s_history.clear();
    s_unread = 0;
}
std::size_t UnreadCount() {
    std::scoped_lock lock{s_history_mutex};
    return s_unread;
}
void MarkRead() {
    std::scoped_lock lock{s_history_mutex};
    s_unread = 0;
}
void Register() {}
void Unregister() {}
} // namespace ImGui::ShadNetNotify

namespace ImGui::InvitationPrompt {
void Push(s32, std::string invitation_id, std::string, std::string from_npid) {
    std::printf("Co-op: invitation %s from %s (invitations are not supported yet)\n",
                invitation_id.c_str(), from_npid.c_str());
}
void Dismiss(const std::string&) {}
void Register() {}
void Unregister() {}
} // namespace ImGui::InvitationPrompt

// ---- Game identity -------------------------------------------------------------------------------

// ElfInfo lets shadPS4's Core::Emulator fill it in; this port has no emulator class, so the
// same name is used here for that one job.
namespace Core {
class Emulator {
public:
    static void SetGame(const char* sz_app0, const char* sz_serial, const char* sz_title,
                        const char* sz_app_ver) {
        auto& info = Common::ElfInfo::Instance();
        info.game_serial = sz_serial ? sz_serial : "";
        info.title = sz_title ? sz_title : "";
        info.app_ver = sz_app_ver ? sz_app_ver : "";
        info.game_folder = sz_app0 ? sz_app0 : "";
        info.initialized = true;
        info.npCommIds = ReadNpCommIds(info.game_folder / "sce_sys" / "npbind.dat");
    }

private:
    // npbind.dat: a 0x80-byte header, then tagged records (big-endian u16 tag and length);
    // tag 0x10 holds an NP communication id such as NPWR05818_00.
    static std::vector<std::string> ReadNpCommIds(const std::filesystem::path& path) {
        std::vector<std::string> ids;
        std::ifstream file{path, std::ios::binary};
        const std::vector<u8> data{std::istreambuf_iterator<char>(file), {}};
        for (size_t at = 0x80; at + 4 <= data.size();) {
            const u16 tag = u16(data[at] << 8 | data[at + 1]);
            const u16 cb_entry = u16(data[at + 2] << 8 | data[at + 3]);
            at += 4;
            if (at + cb_entry > data.size()) {
                break;
            }
            if (tag == 0x10 && cb_entry) {
                std::string id(reinterpret_cast<const char*>(&data[at]), cb_entry);
                id.resize(std::strlen(id.c_str()));
                ids.push_back(id);
            }
            at += cb_entry;
        }
        return ids;
    }
};
} // namespace Core

namespace {
void Configure(const char* sz_app0, const char* sz_serial, const char* sz_title, const char* sz_app_ver) {
    Core::Emulator::SetGame(sz_app0, sz_serial, sz_title, sz_app_ver);
    const auto& ids = Common::ElfInfo::Instance().GetNpCommIds();
    std::printf("Co-op: %s, NP communication id %s%s\n", sz_serial ? sz_serial : "?",
                ids.empty() ? "missing" : ids.front().c_str(),
                EmulatorSettings.IsShadNetEnabled() ? "; online play on" : "");
}
} // namespace

// ---- Registration and the C interface ------------------------------------------------------------

// The module's own table (the port's libpsport-gpu keeps another one: this library's symbols are its
// own, -Bsymbolic).
namespace {
struct Symbol {
    std::string nid;
    u64 va_symbol;
};
std::vector<Symbol> s_symbols;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* sz_nid, const char*, const char*, SymbolType, u64 va_symbol) {
    s_symbols.push_back({sz_nid, va_symbol});
}
} // namespace Core::Loader

extern "C" __attribute__((visibility("default"))) uintptr_t psport_net_resolve(const char* sz_scoped_nid) {
    const char* sz_hash = std::strchr(sz_scoped_nid, '#');
    const size_t length = sz_hash ? size_t(sz_hash - sz_scoped_nid) : std::strlen(sz_scoped_nid);
    for (const auto& symbol : s_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), sz_scoped_nid, length)) {
            return uintptr_t(symbol.va_symbol);
        }
    }
    return 0;
}

extern "C" __attribute__((visibility("default"))) int psport_net_init(const PsNetHost* p_host) {
    if (!p_host || p_host->abi != PSPORT_NET_ABI_VERSION || !p_host->pfn_thread_spawn || !p_host->pfn_thread_join) {
        std::printf("Online: the runtime and the online module are from different versions\n");
        return -1;
    }
    s_host = *p_host;
    Configure(p_host->sz_app0, p_host->sz_serial, p_host->sz_title, p_host->sz_app_ver);
    Core::Loader::SymbolsResolver resolver;
    Core::Loader::SymbolsResolver* p_sym = &resolver;
    Libraries::Net::RegisterLib(p_sym);
    Libraries::NetCtl::RegisterLib(p_sym);
    Libraries::Http::RegisterLib(p_sym);
    Libraries::Http2::RegisterLib(p_sym);
    Libraries::Ssl::RegisterLib(p_sym);
    Libraries::Ssl2::RegisterLib(p_sym);
    Libraries::Np::NpAuth::RegisterLib(p_sym);
    Libraries::Np::NpCommon::RegisterLib(p_sym);
    Libraries::Np::NpManager::RegisterLib(p_sym);
    Libraries::Np::NpMatching2::RegisterLib(p_sym);
    Libraries::Np::NpPartner::RegisterLib(p_sym);
    Libraries::Np::NpParty::RegisterLib(p_sym);
    Libraries::Np::NpScore::RegisterLib(p_sym);
    Libraries::Np::NpSignaling::RegisterLib(p_sym);
    Libraries::Np::NpTus::RegisterLib(p_sym);
    Libraries::Np::NpWebApi::RegisterLib(p_sym);
    Libraries::Np::NpWebApi2::RegisterLib(p_sym);
    std::printf("Online: %zu network and PSN functions\n", s_symbols.size());
    return 0;
}
