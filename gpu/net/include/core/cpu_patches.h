// SPDX-License-Identifier: GPL-2.0-or-later
// psport online module: shadPS4's guest code hook interface (core/cpu_patches.h of the shadp2p and
// BBSC forks) as a title's extension calls it (BBSC's game code), over the port's guest
// code layer (gpu/port/psport_guest_code.h), so that every hook is byte-checked and logged there.
// From bbport (gpu/shim/bbport_guest_code_hooks.h).
#pragma once

#include <cstdint>
#include <span>

#include "../port/psport_guest_code.h"
#include "common/types.h"

namespace Core {

using GuestRegisterSnapshot = PsGuestCode::Registers;

/// Called on the game's thread, on its stack (keep it small).
using GuestCodeHook = void(PS4_SYSV_ABI*)(std::uint64_t tag, const GuestRegisterSnapshot* p_registers);

/// PsGuestCode::InstallHook at p_address over expected_instructions; false when it refused (its
/// reason is logged). p_skip_flag, for a function's entry only, must point to writable memory: the
/// trampoline clears it when it returns early.
bool InstallGuestCodeHook(void* p_address, std::span<const std::uint8_t> expected_instructions, std::uint64_t tag,
                          GuestCodeHook pfn_hook, const std::uint8_t* p_skip_flag = nullptr);

} // namespace Core
