// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: what the launched title declares for online play (PsNetHost), and the
// title's own part of the module (Game::OnlineExtension): code under games/<game>/net/ that needs
// the network libraries, built into libpsport-net.so, registered by name and started only when the
// runtime names it (the title has it and the player turned it on); and the client of its summon
// calls, chosen by the title's summon route.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include "psport_net.h"

namespace Libraries::Http {
struct SendRequestPlan;
struct HttpResponse;
} // namespace Libraries::Http

namespace PsNetTitle {

struct Extension {
    const char* sz_name = nullptr;
    /// psport_net_init, before the libraries register: false when it cannot run (it logged why;
    /// online play goes on without it).
    bool (*pfn_start)(const PsNetHost& host) = nullptr;
    /// psport_net_image_ready: the game's image is mapped and patched, before any of its code runs.
    void (*pfn_image_ready)(std::uint64_t va_image, std::uint64_t cb_image) = nullptr;
    /// A packet on the np_signaling socket the library does not know, on its receive thread: true
    /// takes it, else it goes on as an unknown packet.
    bool (*pfn_signaling_packet)(const std::uint8_t* p_packet, std::size_t cb_packet) = nullptr;
    /// sceNpMatching2LeaveRoom, called from the game's code at va_return.
    void (*pfn_leave_room)(std::uintptr_t va_return, std::uint64_t room_id) = nullptr;
};

/// The title's client for the summon calls on its community server (PsNetHost sz_summon_route).
/// It serves the title declaring sz_route whether or not the title's extension runs, and a title
/// declaring no route when pfn_is_forced says the player forced it. Every pfn is set, and each
/// sees every request libSceHttp sends: the client picks its own calls.
struct SummonClient {
    const char* sz_route = nullptr;
    bool (*pfn_is_forced)() = nullptr;
    /// sceHttpSendRequest, before the request goes to its worker thread: may send it elsewhere.
    void (*pfn_prepare)(Libraries::Http::SendRequestPlan& plan) = nullptr;
    /// The worker, online, before the request is sent.
    void (*pfn_before_send)(const Libraries::Http::SendRequestPlan& plan) = nullptr;
    /// The worker, once the request was answered: may send it again and give the game a later
    /// answer instead (response, event_bits), and stops when is_abandoned (the game no longer
    /// waits for it).
    void (*pfn_answered)(const Libraries::Http::SendRequestPlan& plan, Libraries::Http::HttpResponse& response,
                         std::uint32_t& event_bits, const std::function<bool()>& is_abandoned) = nullptr;
    /// The worker, with what the game gets: request_error 0 or the ORBIS_HTTP_ERROR it sees.
    void (*pfn_finished)(const Libraries::Http::SendRequestPlan& plan, const Libraries::Http::HttpResponse& response,
                         std::int32_t request_error) = nullptr;
};

/// From a static initializer of the extension's own file; never removed. False when the table is
/// full or the name is taken (logged).
bool Register(const Extension& extension);

/// Starts the extension host names, if any (psport_net_init).
void Start(const PsNetHost& host);

/// The started extension's pfn_image_ready, if any.
void ImageReady(std::uint64_t va_image, std::uint64_t cb_image);

/// The started extension's pfn_signaling_packet; false without one.
bool ReceiveSignaling(const std::uint8_t* p_packet, std::size_t cb_packet);

/// The started extension's pfn_leave_room, if any.
void LeaveRoom(std::uintptr_t va_return, std::uint64_t room_id);

/// From a static initializer of the client's own file; never removed. False when the table is
/// full or the route is taken (logged).
bool RegisterSummon(const SummonClient& client);

/// The client serving this title's summon calls (chosen by Start), or nullptr.
const SummonClient* Summon();

/// The title's summon route (PsNetHost sz_summon_route), empty when it declares none.
std::string_view SummonRoute();

} // namespace PsNetTitle
