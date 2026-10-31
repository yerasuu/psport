// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <expected>
#include "common/object_pool.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/translation_error.h"

namespace Shader {

struct Profile;
struct RuntimeInfo;

struct Pools {
    static constexpr u32 InstPoolSize = 8192;
    static constexpr u32 BlockPoolSize = 32;

    Common::ObjectPool<IR::Inst> inst_pool;
    Common::ObjectPool<IR::Block> block_pool;

    explicit Pools() : inst_pool{InstPoolSize}, block_pool{BlockPoolSize} {}

    void ReleaseContents() {
        block_pool.ReleaseContents();
        inst_pool.ReleaseContents();
    }
};

/// Runs the frontend and the IR passes. A shader it cannot translate comes back as the error,
/// with info and pools left partly written: the caller drops info, and the next call releases
/// the pools. code must be the shader in guest memory, followed by its OrbShdr footer and the
/// data the compiler appends, where jump tables are read from.
[[nodiscard]] std::expected<IR::Program, TranslationError> TranslateProgram(
    const std::span<const u32>& code, Pools& pools, Info& info, RuntimeInfo& runtime_info,
    const Profile& profile);

} // namespace Shader
