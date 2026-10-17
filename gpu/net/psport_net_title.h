// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: what the launched title declares for online play (PsNetHost), and the
// title's own part of the module (Game::OnlineExtension): code under games/<game>/net/ that needs
// the network libraries, built into libpsport-net.so, registered by name and started only when the
// runtime names it (the title has it and the player turned it on).
#pragma once
#include <cstdint>
#include <string_view>

#include "psport_net.h"

namespace PsNetTitle {

struct Extension {
    const char* sz_name = nullptr;
    /// psport_net_init, before the libraries register: false when it cannot run (it logged why;
    /// online play goes on without it).
    bool (*pfn_start)(const PsNetHost& host) = nullptr;
    /// psport_net_image_ready: the game's image is mapped and patched, before any of its code runs.
    void (*pfn_image_ready)(std::uint64_t va_image, std::uint64_t cb_image) = nullptr;
};

/// From a static initializer of the extension's own file; never removed. False when the table is
/// full or the name is taken (logged).
bool Register(const Extension& extension);

/// Starts the extension host names, if any (psport_net_init).
void Start(const PsNetHost& host);

/// The started extension's pfn_image_ready, if any.
void ImageReady(std::uint64_t va_image, std::uint64_t cb_image);

/// The title's summon route (PsNetHost sz_summon_route), empty when it declares none.
std::string_view SummonRoute();

} // namespace PsNetTitle
