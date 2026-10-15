// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_wait_trace.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>

namespace PsWaitTrace {
namespace {
constexpr int Skip = 1; // Scope::Scope itself
constexpr int Kept = 6; // frames that identify a stack

struct Key {
    std::array<void*, Kept> m_frames{};
    std::array<char, 16> m_thread{};
    bool operator<(const Key& other) const {
        if (m_frames != other.m_frames) {
            return m_frames < other.m_frames;
        }
        return m_thread < other.m_thread;
    }
};
struct Total {
    std::uint64_t ns = 0, c_waits = 0;
};
std::mutex s_mutex;
std::map<Key, Total> s_totals;

std::string Describe(void* p_address) {
    Dl_info info{};
    if (!dladdr(p_address, &info)) {
        char sz_text[32];
        std::snprintf(sz_text, sizeof(sz_text), "%p", p_address);
        return sz_text;
    }
    const char* sz_file = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
    sz_file = sz_file ? sz_file + 1 : (info.dli_fname ? info.dli_fname : "?");
    char sz_text[512];
    if (info.dli_sname) {
        std::snprintf(sz_text, sizeof(sz_text), "%s+%#lx", info.dli_sname,
                      (unsigned long)(static_cast<char*>(p_address) -
                                      static_cast<char*>(info.dli_saddr)));
    } else {
        std::snprintf(sz_text, sizeof(sz_text), "%s+%#lx", sz_file,
                      (unsigned long)(static_cast<char*>(p_address) -
                                      static_cast<char*>(info.dli_fbase)));
    }
    return sz_text;
}
} // namespace

bool Enabled() {
    static const bool s_is_enabled = [] {
        const char* sz_env = std::getenv("PS_WAIT_TRACE");
        return sz_env && sz_env[0] == '1';
    }();
    return s_is_enabled;
}

Scope::Scope() {
    if (!Enabled()) {
        return;
    }
    m_depth = backtrace(m_frames, int(std::size(m_frames)));
    m_start = std::chrono::steady_clock::now();
}

Scope::~Scope() {
    if (m_depth == 0) {
        return;
    }
    const auto ns = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - m_start)
                                      .count());
    Key key;
    for (int i = 0; i < Kept && Skip + i < m_depth; ++i) {
        key.m_frames[i] = m_frames[Skip + i];
    }
    pthread_getname_np(pthread_self(), key.m_thread.data(), key.m_thread.size());
    std::scoped_lock lk{s_mutex};
    auto& total = s_totals[key];
    total.ns += ns;
    ++total.c_waits;
}

void Report(double window_s) {
    if (!Enabled()) {
        return;
    }
    std::vector<std::pair<Key, Total>> list;
    {
        std::scoped_lock lk{s_mutex};
        list.assign(s_totals.begin(), s_totals.end());
        s_totals.clear();
    }
    std::sort(list.begin(), list.end(),
              [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    std::uint64_t all = 0;
    for (const auto& [key, total] : list) {
        all += total.ns;
    }
    std::printf("GPU tick waits: %.1f%% of the time in all, by stack:\n",
                window_s > 0 ? all / (window_s * 1e7) : 0.0);
    for (std::size_t i = 0; i < list.size() && i < 6; ++i) {
        const auto& [key, total] = list[i];
        std::printf("  %5.1f%% %6llu waits, thread %s:", total.ns / (window_s * 1e7),
                    (unsigned long long)total.c_waits, key.m_thread.data());
        for (void* p_frame : key.m_frames) {
            if (p_frame) {
                std::printf(" <- %s", Describe(p_frame).c_str());
            }
        }
        std::printf("\n");
    }
}
} // namespace PsWaitTrace
