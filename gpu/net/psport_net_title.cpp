// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: the title's extension (psport_net_title.h).
#include "psport_net_title.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace PsNetTitle {
namespace {
constexpr std::size_t MaxExtensions = 8;

/// Function-local so that registrations from other files' static initializers find it built.
std::array<Extension, MaxExtensions>& Table() {
    static std::array<Extension, MaxExtensions> s_table{};
    return s_table;
}

/// Function-local so that registrations from other files' static initializers find it built.
std::array<SummonClient, MaxExtensions>& SummonTable() {
    static std::array<SummonClient, MaxExtensions> s_table{};
    return s_table;
}

const Extension* s_p_started = nullptr;
const SummonClient* s_p_summon = nullptr;
const char* s_sz_summon_route = nullptr;

void ChooseSummon(const char* sz_route) {
    for (const SummonClient& client : SummonTable()) {
        if (!client.sz_route) {
            continue;
        }
        if (sz_route && !std::strcmp(client.sz_route, sz_route)) {
            s_p_summon = &client;
            return;
        }
        if (!sz_route && client.pfn_is_forced()) {
            s_p_summon = &client;
            std::printf("Co-op: summon route %s forced (the title declares none)\n", client.sz_route);
            return;
        }
    }
    if (sz_route) {
        std::printf("Co-op: this module carries no client for the summon route %s: its calls go out as the "
                    "game makes them\n",
                    sz_route);
    }
}
} // namespace

bool Register(const Extension& extension) {
    for (Extension& slot : Table()) {
        if (slot.sz_name && !std::strcmp(slot.sz_name, extension.sz_name)) {
            std::printf("Online: extension %s registered twice; the first one stays\n", extension.sz_name);
            return false;
        }
        if (!slot.sz_name) {
            slot = extension;
            return true;
        }
    }
    std::printf("Online: no room for the extension %s\n", extension.sz_name);
    return false;
}

bool RegisterSummon(const SummonClient& client) {
    for (SummonClient& slot : SummonTable()) {
        if (slot.sz_route && !std::strcmp(slot.sz_route, client.sz_route)) {
            std::printf("Online: summon client for %s registered twice; the first one stays\n", client.sz_route);
            return false;
        }
        if (!slot.sz_route) {
            slot = client;
            return true;
        }
    }
    std::printf("Online: no room for the summon client for %s\n", client.sz_route);
    return false;
}

void Start(const PsNetHost& host) {
    s_sz_summon_route = host.sz_summon_route;
    ChooseSummon(host.sz_summon_route);
    if (!host.sz_extension) {
        return;
    }
    for (const Extension& extension : Table()) {
        if (!extension.sz_name || std::strcmp(extension.sz_name, host.sz_extension)) {
            continue;
        }
        if (extension.pfn_start && !extension.pfn_start(host)) {
            std::printf("Online: extension %s did not start; online play goes on without it\n", extension.sz_name);
            return;
        }
        s_p_started = &extension;
        std::printf("Online: extension %s started\n", extension.sz_name);
        return;
    }
    std::printf("Online: this module carries no extension %s (the title's): online play goes on without it\n",
                host.sz_extension);
}

void ImageReady(std::uint64_t va_image, std::uint64_t cb_image) {
    if (s_p_started && s_p_started->pfn_image_ready) {
        s_p_started->pfn_image_ready(va_image, cb_image);
    }
}

bool ReceiveSignaling(const std::uint8_t* p_packet, std::size_t cb_packet) {
    return s_p_started && s_p_started->pfn_signaling_packet && s_p_started->pfn_signaling_packet(p_packet, cb_packet);
}

void LeaveRoom(std::uintptr_t va_return, std::uint64_t room_id) {
    if (s_p_started && s_p_started->pfn_leave_room) {
        s_p_started->pfn_leave_room(va_return, room_id);
    }
}

const SummonClient* Summon() {
    return s_p_summon;
}

std::string_view SummonRoute() {
    return s_sz_summon_route ? s_sz_summon_route : "";
}

} // namespace PsNetTitle
