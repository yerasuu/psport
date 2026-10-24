// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);

void IncrementRip(void* ctx, u64 length);

// psport: the registers SRT walkers load into, the only ones a fault handler here rewrites.
enum class Register : u8 { Rdi, R10 };

u64 GetRegister(void* ctx, Register reg);

void SetRegister(void* ctx, Register reg, u64 value);

bool IsWriteError(void* ctx);

} // namespace Common