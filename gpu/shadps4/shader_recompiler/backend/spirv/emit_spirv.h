// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <expected>
#include <vector>
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/translation_error.h"

namespace Shader::Backend::SPIRV {

/// A program the backend cannot emit comes back as the error, with binding partly advanced.
[[nodiscard]] std::expected<std::vector<u32>, TranslationError> EmitSPIRV(
    const Profile& profile, const RuntimeInfo& runtime_info, const IR::Program& program,
    Bindings& binding);

} // namespace Shader::Backend::SPIRV
