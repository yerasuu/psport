// SPDX-License-Identifier: GPL-2.0-or-later
// The failing results of the native Ngs2's exports (kb #1161): the first time an export fails with a
// code from one cause (a Fail format, or none when the code is returned without one) it is logged
// with the first message, and every failure is counted by export, code and cause in a summary line
// after the call report.
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "Internal.h"

extern "C" void runtime_calls_add_summary(void (*pfn_summary)(char* sz_out, std::size_t cb_out));

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kMaxCauses = 128;

struct Cause {
    const char* sz_export;
    const char* p_format;
    s32 code;
    char sz_reason[160];
    std::atomic<std::uint64_t> c_calls;
};

// Published by s_c_causes: a cause's fields are written before the store that counts it, and never
// again, so that lookups and the summary read them without the lock.
std::array<Cause, kMaxCauses> s_causes;
std::atomic<u32> s_c_causes;
std::atomic<std::uint64_t> s_c_unlisted;
std::mutex s_causes_mutex;
constinit thread_local ExportCall* s_p_call = nullptr;

Cause* FindCause(const char* sz_export, s32 code, const char* p_format) {
    const u32 c_causes = s_c_causes.load(std::memory_order_acquire);
    for (u32 i_cause = 0; i_cause < c_causes; ++i_cause) {
        Cause& cause = s_causes[i_cause];
        if (cause.sz_export == sz_export && cause.code == code && cause.p_format == p_format) {
            return &cause;
        }
    }
    return nullptr;
}

// Runs after the call report, possibly from the fault and watchdog handlers: no lock, only the
// published causes and their counters.
void Summarize(char* sz_out, std::size_t cb_out) {
    const u32 c_causes = s_c_causes.load(std::memory_order_acquire);
    const std::uint64_t c_unlisted = s_c_unlisted.load(std::memory_order_relaxed);
    std::array<const Cause*, kMaxCauses> p_sorted{};
    std::uint64_t c_calls = c_unlisted;
    for (u32 i_cause = 0; i_cause < c_causes; ++i_cause) {
        p_sorted[i_cause] = &s_causes[i_cause];
        c_calls += s_causes[i_cause].c_calls.load(std::memory_order_relaxed);
    }
    std::sort(p_sorted.begin(), p_sorted.begin() + c_causes, [](const Cause* p_left, const Cause* p_right) {
        return p_left->c_calls.load(std::memory_order_relaxed) > p_right->c_calls.load(std::memory_order_relaxed);
    });
    std::size_t cb_used = 0;
    const auto append = [&](int c_written) {
        cb_used = c_written < 0 ? cb_out : std::min(cb_out, cb_used + static_cast<std::size_t>(c_written));
    };
    append(std::snprintf(sz_out, cb_out,
                         "Ngs2: summary: %" PRIu64 " failed calls, %u causes (export -> code calls: first message):",
                         c_calls, c_causes));
    for (u32 i_cause = 0; i_cause < c_causes && cb_used < cb_out; ++i_cause) {
        const Cause& cause = *p_sorted[i_cause];
        append(std::snprintf(sz_out + cb_used, cb_out - cb_used, " %s -> %#x %" PRIu64 ": %s;", cause.sz_export,
                             static_cast<u32>(cause.code), cause.c_calls.load(std::memory_order_relaxed),
                             cause.sz_reason));
    }
    if (c_unlisted && cb_used < cb_out) {
        append(std::snprintf(sz_out + cb_used, cb_out - cb_used, " %" PRIu64 " calls of causes past the first %u;",
                             c_unlisted, kMaxCauses));
    }
    if (cb_used + 2 > cb_out) {
        std::snprintf(sz_out + cb_out - 5, 5, "...\n");
    } else {
        std::snprintf(sz_out + cb_used, cb_out - cb_used, "\n");
    }
}

} // namespace

ExportCall::ExportCall(const char* sz_export) : m_sz_export{sz_export}, m_p_outer{s_p_call} {
    m_sz_reason[0] = '\0';
    s_p_call = this;
}

ExportCall::~ExportCall() {
    s_p_call = m_p_outer;
}

void ExportCall::NoteResult(s32 result) {
    if (result == ORBIS_OK) {
        return;
    }
    const char* p_format = m_code == result ? m_p_format : nullptr;
    Cause* p_cause = FindCause(m_sz_export, result, p_format);
    if (!p_cause) {
        std::scoped_lock lock{s_causes_mutex};
        p_cause = FindCause(m_sz_export, result, p_format);
        if (!p_cause) {
            const u32 i_cause = s_c_causes.load(std::memory_order_relaxed);
            if (i_cause == kMaxCauses) {
                if (s_c_unlisted.fetch_add(1, std::memory_order_relaxed) == 0) {
                    LOG_ERROR(Lib_Ngs2, "{} returned {:#x}: past {} distinct failures of the native Ngs2; the next "
                              "new ones are only counted", m_sz_export, static_cast<u32>(result), kMaxCauses);
                }
                return;
            }
            p_cause = &s_causes[i_cause];
            p_cause->sz_export = m_sz_export;
            p_cause->code = result;
            p_cause->p_format = p_format;
            const char* sz_reason = m_sz_reason[0] ? m_sz_reason : "message not kept";
            if (!p_format) {
                sz_reason = "no message: the code itself names the cause, or a callee returned it";
            }
            std::snprintf(p_cause->sz_reason, sizeof(p_cause->sz_reason), "%s", sz_reason);
            s_c_causes.store(i_cause + 1, std::memory_order_release);
            if (i_cause == 0) {
                runtime_calls_add_summary(&Summarize);
            }
            LOG_ERROR(Lib_Ngs2, "{} returned {:#x}: {} (first failure from this cause; all counted in the Ngs2 "
                      "summary after the call report)", m_sz_export, static_cast<u32>(result), p_cause->sz_reason);
        }
    }
    p_cause->c_calls.fetch_add(1, std::memory_order_relaxed);
}

bool IsNewFailureCause(s32 code, const char* p_format) {
    return s_p_call && s_c_causes.load(std::memory_order_relaxed) < kMaxCauses &&
           !FindCause(s_p_call->m_sz_export, code, p_format);
}

void NoteFailureReason(s32 code, const char* p_format, std::string_view message) {
    if (!s_p_call) {
        return;
    }
    s_p_call->m_code = code;
    s_p_call->m_p_format = p_format;
    const std::size_t cb_reason = std::min(message.size(), sizeof(s_p_call->m_sz_reason) - 1);
    if (cb_reason) {
        std::memcpy(s_p_call->m_sz_reason, message.data(), cb_reason);
    }
    s_p_call->m_sz_reason[cb_reason] = '\0';
}

} // namespace Libraries::Ngs2
