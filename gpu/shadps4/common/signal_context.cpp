// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/signal_context.h"
// psport: fault handlers get psport's PsHostFaultContext, whose accessors are per OS.
#include "../../src/platform/ps_host.h"

namespace Common {

void* GetRip(void* ctx) {
    return reinterpret_cast<void*>(ps_host_fault_register(static_cast<PsHostFaultContext*>(ctx), PS_HOST_REG_RIP));
}

void IncrementRip(void* ctx, u64 length) {
    auto* const p_context = static_cast<PsHostFaultContext*>(ctx);
    ps_host_fault_set_register(p_context, PS_HOST_REG_RIP, ps_host_fault_register(p_context, PS_HOST_REG_RIP) + length);
}

void SetRegister(void* ctx, Register reg, u64 value) {
    ps_host_fault_set_register(static_cast<PsHostFaultContext*>(ctx),
                               reg == Register::Rdi ? PS_HOST_REG_RDI : PS_HOST_REG_R10, value);
}

bool IsWriteError(void* ctx) {
    return ps_host_fault_access(static_cast<PsHostFaultContext*>(ctx)) == PS_HOST_FAULT_WRITE;
}

} // namespace Common
