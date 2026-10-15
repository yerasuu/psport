// SPDX-License-Identifier: GPL-2.0-or-later
// psport: every libSceFont/libSceFontFt export is called through Guarded. The sources lean on
// std::filesystem, containers and fmt, which throw on failures the console's library reports as
// codes; an exception must not unwind through the game's frames, so it becomes a logged
// ORBIS_FONT_ERROR_FATAL (or a null/zero result for the functions returning something else).
#pragma once
#include <atomic>
#include <exception>
#include <type_traits>

#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "font_error.h"

namespace Libraries::Font {

template <auto F>
struct Guarded;
template <typename R, typename... Args, R (*F)(Args...)>
struct Guarded<F> {
    static const char* name;
    static R PS4_SYSV_ABI Call(Args... args) {
        try {
            return F(args...);
        } catch (const std::exception& error) {
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_ERROR(Lib_Font, "{} failed: {}", name, error.what());
            }
            if constexpr (std::is_same_v<R, s32>) {
                return ORBIS_FONT_ERROR_FATAL;
            } else if constexpr (!std::is_void_v<R>) {
                return R{};
            }
        }
    }
};
template <typename R, typename... Args, R (*F)(Args...)>
const char* Guarded<F>::name = "?";

} // namespace Libraries::Font

#define FONT_FUNCTION(nid, lib, libversion, mod, function)                                         \
    ::Libraries::Font::Guarded<function>::name = #function;                                        \
    LIB_FUNCTION(nid, lib, libversion, mod, ::Libraries::Font::Guarded<function>::Call)
