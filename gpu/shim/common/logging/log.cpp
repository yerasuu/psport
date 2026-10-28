// SPDX-License-Identifier: GPL-2.0-or-later
// psport: see log.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include "common/logging/log.h"

extern "C" void runtime_calls_hle_stub(const char* function, const std::uint64_t* count);
extern "C" void runtime_calls_fallback(const char* function, const char* message,
                                       const std::uint64_t* count);
extern "C" int runtime_debug_open(const char* sz_name);

namespace Common::Log {
static Level threshold() {
    static const Level level = [] {
        const char* v = std::getenv("PS_GPU_LOG");
        if (!v) return Level::warn;
        static const char* names[] = {"trace", "debug", "info", "warning", "error", "critical"};
        for (int i = 0; i < 6; ++i)
            if (!std::strcmp(v, names[i])) return Level(i);
        return Level::warn;
    }();
    return level;
}
bool ShouldLog(Level level) { return level >= threshold(); }
void Stubbed(const char* function, const std::uint64_t* count) { runtime_calls_hle_stub(function, count); }
void Fallback(const char* function, const char* message, const std::uint64_t* count) {
    runtime_calls_fallback(function, message, count);
}
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message) {
    static std::mutex mutex;
    static const char* names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
    const char* base = std::strrchr(file, '/');
    std::scoped_lock lock{mutex};
    std::fprintf(stderr, "GPU [%.*s] <%s> %s:%d %s: %s\n", int(log_class.size()), log_class.data(),
                 names[int(level)], base ? base + 1 : file, line, function, message.c_str());
    // Debug mode's gpu.log (src/runtime/runtime_debug.c), asked again until it opens since a line
    // may come before main starts debug mode.
    static int s_h_debug_log = -1;
    if (s_h_debug_log < 0) {
        s_h_debug_log = runtime_debug_open("gpu.log");
    }
    if (s_h_debug_log >= 0) {
        const std::string text = fmt::format("GPU [{}] <{}> {}:{} {}: {}\n", log_class, names[int(level)],
                                             base ? base + 1 : file, line, function, message);
        (void)!::write(s_h_debug_log, text.data(), text.size());
    }
}
} // namespace Common::Log
