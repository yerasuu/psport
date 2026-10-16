// SPDX-License-Identifier: GPL-2.0-or-later
// psport: a shader the recompiler cannot translate (an instruction, operand, format, resource or
// control flow it does not handle, or IR it cannot follow) fails its translation instead of
// stopping the port. The checks below throw TranslationError; only the recompiler's entry points
// (TranslateProgram, Backend::SPIRV::EmitSPIRV) catch it and return it as a value, so no exception
// leaves the recompiler. The pipeline cache logs it and skips the draws and dispatches that need
// the shader. Code reached from outside those entry points (Info, resource descriptors, the fetch
// shader parse) logs and falls back instead of throwing.
#pragma once

#include <atomic>
#include <bit>
#include <string>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/types.h"

namespace Shader {

struct TranslationError {
    std::string reason;
    const char* sz_file;
    u32 line;
};

[[nodiscard]] TranslationError MakeTranslationError(const char* sz_file, u32 line,
                                                    std::string reason);

} // namespace Shader

// The throw stays in the macro so that GCC's -Wterminate flags one inside a noexcept function.
#define SHADER_ASSERT(_a_)                                                                         \
    do {                                                                                           \
        if (!(_a_)) [[unlikely]] {                                                                 \
            throw ::Shader::MakeTranslationError(__FILE__, __LINE__, "check failed: " #_a_);       \
        }                                                                                          \
    } while (false)

#define SHADER_ASSERT_MSG(_a_, ...)                                                                \
    do {                                                                                           \
        if (!(_a_)) [[unlikely]] {                                                                 \
            throw ::Shader::MakeTranslationError(__FILE__, __LINE__, fmt::format(__VA_ARGS__));    \
        }                                                                                          \
    } while (false)

#define SHADER_UNREACHABLE()                                                                       \
    throw ::Shader::MakeTranslationError(__FILE__, __LINE__, "unhandled case")

#define SHADER_UNREACHABLE_MSG(...)                                                                \
    throw ::Shader::MakeTranslationError(__FILE__, __LINE__, fmt::format(__VA_ARGS__))

// For the checks outside the entry points: logs the first time and at each power of two, with the
// count, before the caller takes its fallback.
#define SHADER_LOG_FALLBACK(fmt_str, ...)                                                          \
    do {                                                                                           \
        static std::atomic<u32> psport_c_fallbacks;                                                \
        const u32 c_fallbacks = psport_c_fallbacks.fetch_add(1, std::memory_order_relaxed) + 1;    \
        if (std::has_single_bit(c_fallbacks)) {                                                    \
            LOG_ERROR(Render_Recompiler, fmt_str " ({} so far)" __VA_OPT__(, ) __VA_ARGS__,        \
                      c_fallbacks);                                                                \
        }                                                                                          \
    } while (false)
