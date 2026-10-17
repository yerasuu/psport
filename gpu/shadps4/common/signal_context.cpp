// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/signal_context.h"
// psport: fault handlers get psport's PsHostFaultContext, whose accessors are per OS.
#include "../../src/platform/ps_host.h"

namespace Common {

void* GetRip(void* ctx) {
    return reinterpret_cast<void*>(ps_host_fault_register(static_cast<PsHostFaultContext*>(ctx), PS_HOST_REG_RIP));
}

bool IsWriteError(void* ctx) {
    return ps_host_fault_access(static_cast<PsHostFaultContext*>(ctx)) == PS_HOST_FAULT_WRITE;
}

} // namespace Common
