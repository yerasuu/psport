// SPDX-License-Identifier: GPL-2.0-or-later
// psport: BBSC, Bloodborne's seamless co-op (GPL-2.0-or-later, https://seamlesshunt.com), as
// Bloodborne's extension of the online module (Game::OnlineExtension "bbsc", switch PS_BBSC=1). Its
// game code (core/, BBSC's own files) is installed here once the image is mapped, and what it asks
// of shadPS4's UI is answered by the port's: its loading screen is a line in the overlay, its
// cinematic camera sees the window's events first. From bbport (gpu/bbnet/bbsc_glue.cpp and the
// BBSC part of bbnet_image_ready in gpu/bbnet/bbnet_glue.cpp).
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "../psport_gpu.h"
#include "../port/psport_guest_code.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/singleton.h"
#include "core/bbsc_cinematic.h"
#include "core/bbsc_persistence.h"
#include "core/bloodborne_re.h"
#include "core/file_format/psf.h"
#include "core/libraries/network/sockets.h"
#include "core/libraries/np/np_signaling/np_signaling_state.h"
#include "imgui/bbsc_intro_layer.h"
#include "imgui/bbsc_loading_layer.h"
#include "psport_net_title.h"

namespace Core::Bbsc {

namespace {
std::atomic<bool> s_is_started{false};
} // namespace

bool Started() {
    return s_is_started.load(std::memory_order_acquire);
}

void ShowLoadingScreen(LoadingScreenKind kind) {
    psport_gpu_overlay_banner(kind == LoadingScreenKind::Summon ? "Joining the host's world..."
                                                                : "Following the host...");
}

void HideLoadingScreen() {
    psport_gpu_overlay_banner(nullptr);
}

void StartIntro(std::string_view) {}

} // namespace Core::Bbsc

namespace {

/// BBSC finds the save it backs up and journals through BBSC_SAVE_DIR (the folder of
/// userdata0000..); psport keeps it at <user>/savedata/<user id>/<save title>/SPRJ0005.
void FindSaveFolder() {
    if (std::getenv("BBSC_SAVE_DIR")) {
        return;
    }
    const char* sz_user = std::getenv("PS_GPU_USER_DIR");
    std::error_code error;
    for (const auto& user : std::filesystem::directory_iterator(
             std::filesystem::path(sz_user ? sz_user : "user") / "savedata", error)) {
        for (const auto& title : std::filesystem::directory_iterator(user.path(), error)) {
            const auto folder = title.path() / "SPRJ0005";
            if (std::filesystem::is_directory(folder, error)) {
                setenv("BBSC_SAVE_DIR", folder.c_str(), 0);
                std::printf("BBSC: save folder %s\n", folder.c_str());
                return;
            }
        }
    }
    std::printf("BBSC: no save folder yet under %s/savedata: the save backup starts with the first save\n",
                sz_user ? sz_user : "user");
}

bool Start(const PsNetHost& host) {
    auto* p_psf = Common::Singleton<PSF>::Instance();
    p_psf->app_ver = host.sz_app_ver ? host.sz_app_ver : "";
    p_psf->title_id = host.sz_serial ? host.sz_serial : "";
    p_psf->title = host.sz_title ? host.sz_title : "";
    // BBSC's game code checks for CUSA03173 01.09; every retail Bloodborne runs the same 1.09
    // executable (games/bloodborne/game_check.cpp) and each of BBSC's sites is byte-checked.
    MemoryPatcher::g_game_serial = "CUSA03173";
    Core::Bbsc::s_is_started.store(true, std::memory_order_release);
    Libraries::Net::P2PRelaySetDefaultOn(true);
    std::printf("BBSC: seamless co-op on for %s %s (every player needs it and the same server)\n",
                host.sz_serial ? host.sz_serial : "?", host.sz_app_ver ? host.sz_app_ver : "?");
    return true;
}

void ImageReady(std::uint64_t va_image, std::uint64_t cb_image) {
    MemoryPatcher::g_eboot_address = va_image;
    MemoryPatcher::g_eboot_image_size = cb_image;
    FindSaveFolder();
    // BBSC writes its byte patches straight into the game's code and switches some of them while
    // the game runs (session teardown, the host's lamp).
    PsGuestCode::KeepWritable(va_image, cb_image);
    if (Core::Bbsc::SwitchEnabled("SHADPS4_BBSC_CINEMATIC") &&
        !psport_gpu_add_event_filter([](const PsHostWindowEvent* p_event, PsHostWindow* p_window) -> int {
            return Core::Bbsc::CinematicHandleEvent(*p_event, p_window);
        })) {
        std::printf("BBSC: the cinematic camera gets no window events (F1 does nothing)\n");
    }
    std::printf("BBSC: installing its hooks and patches in the image at %#llx\n",
                static_cast<unsigned long long>(va_image));
    Core::Bloodborne::InstallSeamlessCoopPatches();
    Core::Bbsc::InstallPersistentSessionPatches();
    Core::Bloodborne::InstallReverseEngineeringTrace();
}

constexpr std::uint8_t BBSC_FIRST_SESSION_TYPE = 0x23;
// Types up to here are BBSC's, also those a newer BBSC build sends that this one does not know.
constexpr std::uint8_t BBSC_LAST_TYPE = 0x2F;
constexpr std::size_t C_BBSC_SESSION_TYPES = BBSC_LAST_TYPE - BBSC_FIRST_SESSION_TYPE + 1;
constexpr std::array<std::string_view, 5> BBSC_SESSION_PACKET_NAMES{"life", "scaling", "PvP", "progress", "host boss"};
std::array<std::atomic<std::uint64_t>, C_BBSC_SESSION_TYPES> s_c_session_handled{};
std::array<std::atomic<std::uint64_t>, C_BBSC_SESSION_TYPES> s_c_session_dropped{};

/// False when the size does not match the layout of the packet's type.
bool DispatchSessionPacket(const std::uint8_t* p_packet, std::size_t cb_packet) {
    using Libraries::Np::NpSignaling::SignalingPacketType;
    switch (static_cast<SignalingPacketType>(p_packet[4])) {
    case SignalingPacketType::BbscLife: {
        if (cb_packet != 14) {
            return false;
        }
        std::uint32_t player = 0;
        std::uint32_t sequence = 0;
        std::memcpy(&player, p_packet + 5, sizeof(player));
        std::memcpy(&sequence, p_packet + 10, sizeof(sequence));
        Core::Bloodborne::OnBbscLifePacket(player, p_packet[9], sequence);
        return true;
    }
    case SignalingPacketType::BbscScale: {
        if (cb_packet != 15) {
            return false;
        }
        std::uint16_t percent[5]{};
        std::memcpy(percent, p_packet + 5, sizeof(percent));
        Core::Bloodborne::OnBbscScalePacket(percent);
        return true;
    }
    case SignalingPacketType::BbscPvp:
        if (cb_packet != 11) {
            return false;
        }
        Core::Bloodborne::OnBbscPvpPacket(p_packet[5], p_packet[6]);
        return true;
    case SignalingPacketType::BbscProgress: {
        if (cb_packet < 6) {
            return false;
        }
        const std::size_t c_flags = p_packet[5];
        if (cb_packet != 6 + c_flags * sizeof(std::uint32_t)) {
            return false;
        }
        std::array<std::uint32_t, 255> flags{};
        std::memcpy(flags.data(), p_packet + 6, c_flags * sizeof(std::uint32_t));
        Core::Bloodborne::OnBbscProgressPacket(flags.data(), c_flags);
        return true;
    }
    case SignalingPacketType::BbscHostBoss: {
        if (cb_packet != 9) {
            return false;
        }
        std::int32_t boss = 0;
        std::memcpy(&boss, p_packet + 5, sizeof(boss));
        Core::Bloodborne::OnBbscHostBossPacket(boss);
        return true;
    }
    default:
        return false;
    }
}

bool ReceiveSignaling(const std::uint8_t* p_packet, std::size_t cb_packet) {
    using namespace Libraries::Np::NpSignaling;
    if ((cb_packet == sizeof(SignalingBbscFollow) || cb_packet == kSignalingBbscFollowV1Size) &&
        p_packet[4] == static_cast<u8>(SignalingPacketType::BbscFollow)) {
        SignalingBbscFollow follow{}; // lamp_point stays -1 for an older build's packet
        std::memcpy(&follow, p_packet, cb_packet);
        Core::Bloodborne::OnBbscHostFollowPacket(follow.packed_map, follow.x, follow.y, follow.z, follow.heading,
                                                 follow.sequence, follow.lamp_point);
        return true;
    }
    if (cb_packet == sizeof(SignalingBbscWorldFlags) &&
        p_packet[4] == static_cast<u8>(SignalingPacketType::BbscWorldFlags)) {
        Core::Bloodborne::OnBbscWorldFlagsPacket(p_packet, static_cast<u32>(cb_packet));
        return true;
    }
    const std::uint8_t type = p_packet[4];
    if (type < BBSC_FIRST_SESSION_TYPE || type > BBSC_LAST_TYPE) {
        return false;
    }
    const std::size_t i_type = type - BBSC_FIRST_SESSION_TYPE;
    const bool is_known = i_type < BBSC_SESSION_PACKET_NAMES.size();
    if (is_known && DispatchSessionPacket(p_packet, cb_packet)) {
        const std::uint64_t c_handled = s_c_session_handled[i_type].fetch_add(1, std::memory_order_relaxed) + 1;
        if (std::has_single_bit(c_handled)) {
            LOG_INFO(Lib_NpSignaling, "BBSC {} packet (type {:#x}): {} received", BBSC_SESSION_PACKET_NAMES[i_type],
                     type, c_handled);
        }
        return true;
    }
    // Taken either way, so that the library does not warn about BBSC's own traffic once per packet.
    const std::uint64_t c_dropped = s_c_session_dropped[i_type].fetch_add(1, std::memory_order_relaxed) + 1;
    if (std::has_single_bit(c_dropped)) {
        std::string bytes;
        for (std::size_t i = 0; i < std::min<std::size_t>(cb_packet, 32); ++i) {
            bytes += fmt::format("{:02x}", p_packet[i]);
        }
        LOG_WARNING(Lib_NpSignaling, "BBSC packet type {:#x} size {} dropped ({}): {} so far, first bytes {}", type,
                    cb_packet, is_known ? "the size does not match its layout" : "a type this build does not know",
                    c_dropped, bytes);
    }
    return true;
}

const bool s_is_registered = PsNetTitle::Register({.sz_name = "bbsc",
                                                   .pfn_start = Start,
                                                   .pfn_image_ready = ImageReady,
                                                   .pfn_signaling_packet = ReceiveSignaling,
                                                   .pfn_leave_room = Core::Bloodborne::TraceMatching2LeaveRoom});

} // namespace
