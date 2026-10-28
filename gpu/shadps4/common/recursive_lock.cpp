// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdint>
#include <unordered_map>
#include <unistd.h>
#include "common/logging/log.h"
#include "common/recursive_lock.h"

namespace Common::Detail {

struct RecursiveLockState {
    RecursiveLockType type;
    int count;
};

thread_local std::unordered_map<void*, RecursiveLockState> g_recursive_locks;

namespace {

enum class Misuse { StaleType, TypeMismatch, ReleaseWithoutLock, Count };

constexpr std::array<const char*, size_t(Misuse::Count)> MISUSE_REASONS{
    "an unheld lock kept its type (taken as the type asked)",
    "lock taken again with the other type on the same thread (kept as first taken)",
    "lock released without being held (ignored)",
};
std::array<std::uint64_t, size_t(Misuse::Count)> s_c_misuses{};

void AppendText(char*& p_out, const char* p_end, const char* sz_text) {
    while (*sz_text != '\0' && p_out < p_end) {
        *p_out++ = *sz_text++;
    }
}

/// psport: these locks may be taken inside the GPU fault handler (Rasterizer::IsMapped), so a
/// misuse is counted lock-free and written with write(2) from a stack buffer, on its 1st, 2nd,
/// 4th... occurrence as LOG_ERROR_COUNTED does. Its call summary entry is registered once: that
/// fills a fixed slot under a lock that is never held around guest memory, so a guest fault
/// cannot interrupt its holder.
void NoteMisuse(Misuse misuse) {
    const auto i_reason = size_t(misuse);
    const std::uint64_t c_hits = __atomic_add_fetch(&s_c_misuses[i_reason], 1, __ATOMIC_RELAXED);
    if (c_hits == 1) {
        Common::Log::Fallback("RecursiveLock", MISUSE_REASONS[i_reason], &s_c_misuses[i_reason]);
    }
    if ((c_hits & (c_hits - 1)) != 0) {
        return;
    }
    char sz_count[24];
    char* p_digit = sz_count + sizeof(sz_count) - 1;
    *p_digit = '\0';
    std::uint64_t value = c_hits;
    do {
        *--p_digit = char('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char line[256];
    char* p_out = line;
    const char* p_end = line + sizeof(line) - 1;
    AppendText(p_out, p_end, "GPU [Common] <Error> recursive_lock.cpp: RecursiveLock: ");
    AppendText(p_out, p_end, MISUSE_REASONS[i_reason]);
    AppendText(p_out, p_end, " (");
    AppendText(p_out, p_end, p_digit);
    AppendText(p_out, p_end, " so far)");
    *p_out++ = '\n';
    (void)!::write(STDERR_FILENO, line, size_t(p_out - line));
}

} // namespace

bool IncrementRecursiveLock(void* mutex, RecursiveLockType type) {
    auto& state = g_recursive_locks[mutex];
    if (state.count == 0) {
        if (state.type != RecursiveLockType::None) [[unlikely]] {
            NoteMisuse(Misuse::StaleType);
        }
        state.type = type;
    } else if (state.type != type) [[unlikely]] {
        NoteMisuse(Misuse::TypeMismatch);
    }
    return state.count++ == 0;
}

// The release of a lock taken with the other type was counted when it was taken.
bool DecrementRecursiveLock(void* mutex, [[maybe_unused]] RecursiveLockType type) {
    const auto it_state = g_recursive_locks.find(mutex);
    if (it_state == g_recursive_locks.end() || it_state->second.count <= 0) [[unlikely]] {
        NoteMisuse(Misuse::ReleaseWithoutLock);
        return false;
    }
    if (--it_state->second.count == 0) {
        g_recursive_locks.erase(it_state);
        return true;
    }
    return false;
}

} // namespace Common::Detail
