// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

struct AVFrame;

namespace Libraries::Videodec {

// Returns false, copying nothing, when the pitched and aligned picture does not fit in max_size.
[[nodiscard]] bool CopyNV12Data(u8* dst, u64 max_size, const AVFrame& src);

}
