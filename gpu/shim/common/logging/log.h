// bbport: replaces shadPS4's spdlog-based logger with a small fmt sink.
// Level from PS_GPU_LOG (trace|debug|info|warning|error; default warning).
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <string_view>
#include <type_traits>
#include <fmt/format.h>
#include "common/logging/classes.h"
#include "common/path_util.h"
#include "common/thread.h"

// Level type keeps shadPS4's spdlog spelling so vendored callers compile unchanged.
namespace spdlog {
enum class level : int { trace, debug, info, warn, err, critical, off };
}
namespace Common::Log {
using Level = spdlog::level;
bool ShouldLog(Level level);
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message);
inline void Flush() {}
// bbport: a "(STUBBED)" line names a function that answers without doing its work. It is shown on
// the first call whatever PS_GPU_LOG says, and every call is counted for the call summary
// (src/runtime_calls.c).
template <class T>
constexpr bool IsStubbed(const T& format) {
    if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return std::string_view(format).find("(STUBBED)") != std::string_view::npos;
    } else {
        return false;
    }
}
void Stubbed(const char* function, const std::uint64_t* count);
} // namespace Common::Log

#define LOG_GENERIC(log_class, log_level, fmt_str, ...)                                             \
    do {                                                                                           \
        if constexpr (Common::Log::IsStubbed(fmt_str)) {                                           \
            static std::uint64_t psport_stub_calls;                                                \
            if (__atomic_fetch_add(&psport_stub_calls, 1, __ATOMIC_RELAXED)) {                     \
                break;                                                                             \
            }                                                                                      \
            Common::Log::Stubbed(__func__, &psport_stub_calls);                                    \
        } else if (!Common::Log::ShouldLog(log_level)) {                                           \
            break;                                                                                 \
        }                                                                                          \
        Common::Log::Write(log_class, log_level, __FILE__, __LINE__, __func__,                     \
                           fmt::format(fmt_str __VA_OPT__(, ) __VA_ARGS__));                        \
    } while (false)
#define LOG_TRACE(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::trace, __VA_ARGS__)
#define LOG_DEBUG(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::debug, __VA_ARGS__)
#define LOG_INFO(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::info, __VA_ARGS__)
#define LOG_WARNING(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::warn, __VA_ARGS__)
#define LOG_ERROR(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::err, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::critical, __VA_ARGS__)
