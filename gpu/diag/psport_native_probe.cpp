// SPDX-License-Identifier: GPL-2.0-or-later
#include "psport_native_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>
#include <x86intrin.h>

#include "psport_toggles.h"

namespace PsNativeProbe {
namespace {

constexpr std::uint32_t DcbMask = (1u << 30) - 1;
constexpr std::uint32_t DrainedBit = 1u << 30;
constexpr std::uint32_t CompileBit = 1u << 31;
constexpr std::uint32_t UnknownLayout = ~0u;
constexpr const char* ReasonNames[NumReasons] = {"new_image", "new_view",    "upload",    "buffer", "layout",
                                                 "sync",      "compile",     "descriptors", "memo_miss", "other"};

/// Counters with one writer at a time (the thread recording draws: the draw recording thread, or
/// the GPU command thread once it drained it) read by the reports from any thread.
using Counter = std::atomic<std::uint64_t>;
void Bump(Counter& counter, std::uint64_t by = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}
std::uint64_t Load(const Counter& counter) {
    return counter.load(std::memory_order_relaxed);
}

struct Totals {
    Counter c_draws{0}, c_pure{0}, cycles_draws{0}, cycles_pure{0};
    std::array<Counter, NumReasons> c_by_reason{};
    Counter c_dcbs{0}, c_dcbs_with_draws{0}, c_dcbs_pure{0};
    std::array<Counter, NumReasons> c_dcb_first{};
    Counter c_layout_overflow{0};
};

struct Snapshot {
    std::uint64_t c_draws = 0, c_pure = 0, cycles_draws = 0, cycles_pure = 0;
    std::array<std::uint64_t, NumReasons> c_by_reason{};
    std::uint64_t c_dcbs = 0, c_dcbs_with_draws = 0, c_dcbs_pure = 0;
    std::array<std::uint64_t, NumReasons> c_dcb_first{};
};

Totals s_totals;

Snapshot Take() {
    Snapshot now;
    now.c_draws = Load(s_totals.c_draws);
    now.c_pure = Load(s_totals.c_pure);
    now.cycles_draws = Load(s_totals.cycles_draws);
    now.cycles_pure = Load(s_totals.cycles_pure);
    now.c_dcbs = Load(s_totals.c_dcbs);
    now.c_dcbs_with_draws = Load(s_totals.c_dcbs_with_draws);
    now.c_dcbs_pure = Load(s_totals.c_dcbs_pure);
    for (std::uint32_t r = 0; r < NumReasons; ++r) {
        now.c_by_reason[r] = Load(s_totals.c_by_reason[r]);
        now.c_dcb_first[r] = Load(s_totals.c_dcb_first[r]);
    }
    return now;
}

Snapshot Delta(const Snapshot& now, const Snapshot& before) {
    Snapshot delta;
    delta.c_draws = now.c_draws - before.c_draws;
    delta.c_pure = now.c_pure - before.c_pure;
    delta.cycles_draws = now.cycles_draws - before.cycles_draws;
    delta.cycles_pure = now.cycles_pure - before.cycles_pure;
    delta.c_dcbs = now.c_dcbs - before.c_dcbs;
    delta.c_dcbs_with_draws = now.c_dcbs_with_draws - before.c_dcbs_with_draws;
    delta.c_dcbs_pure = now.c_dcbs_pure - before.c_dcbs_pure;
    for (std::uint32_t r = 0; r < NumReasons; ++r) {
        delta.c_by_reason[r] = now.c_by_reason[r] - before.c_by_reason[r];
        delta.c_dcb_first[r] = now.c_dcb_first[r] - before.c_dcb_first[r];
    }
    return delta;
}

// GPU command thread.
std::atomic<std::uint32_t> s_dcb_serial{0};
thread_local bool s_is_compile_pending = false;
thread_local bool s_is_drain_pending = false;
std::atomic<bool> s_has_started{false};
std::chrono::steady_clock::time_point s_start;
std::uint64_t s_start_tsc = 0;
std::uint32_t s_first_frame = 0;

// Recording thread: the open DCB and draw.
thread_local bool s_has_origin = false;
thread_local std::uint32_t s_origin = 0;
thread_local bool s_is_in_draw = false;
thread_local bool s_is_in_stream = false;
/// The texture binding helper adopted the open draw (HelperScope); a task run inline does not.
thread_local bool s_is_helping = false;
std::atomic<bool> s_is_draw_open{false};
std::atomic<std::uint32_t> s_current_dcb{0};
std::atomic<std::uint32_t> s_draw_reason{NumReasons};
std::uint32_t s_dcb_c_draws = 0;
std::uint32_t s_dcb_first = NumReasons;
std::uint64_t s_draw_start = 0;

/// Layouts the open DCB left its images in, by image backing; entries of an earlier DCB are free.
struct LayoutSlot {
    std::atomic<const void*> p_backing{nullptr};
    std::uint32_t layout = UnknownLayout;
    std::uint32_t dcb = 0;
};
constexpr std::size_t LayoutSlots = 4096;
constexpr std::size_t LayoutProbes = 32;
std::array<LayoutSlot, LayoutSlots> s_layouts;

/// The open DCB's slot for `p_backing`, a new one with an unknown layout when it has none, null
/// when the probed slots are all taken (counted).
LayoutSlot* FindLayout(const void* p_backing) {
    const std::uint32_t dcb = s_current_dcb.load(std::memory_order_relaxed);
    const std::size_t hash =
        std::size_t((reinterpret_cast<std::uintptr_t>(p_backing) >> 4) * 0x9E3779B97F4A7C15ull >> 40);
    LayoutSlot* p_free = nullptr;
    for (std::size_t i = 0; i < LayoutProbes; ++i) {
        LayoutSlot& slot = s_layouts[(hash + i) % LayoutSlots];
        const void* p_seen = slot.p_backing.load(std::memory_order_relaxed);
        if (slot.dcb == dcb && p_seen == p_backing) {
            return &slot;
        }
        if (!p_free && (slot.dcb != dcb || p_seen == nullptr)) {
            p_free = &slot;
        }
    }
    if (!p_free) {
        Bump(s_totals.c_layout_overflow);
        return nullptr;
    }
    p_free->p_backing.store(p_backing, std::memory_order_relaxed);
    p_free->layout = UnknownLayout;
    p_free->dcb = dcb;
    return p_free;
}

void CloseDcb() {
    if (s_dcb_c_draws == 0) {
        return;
    }
    Bump(s_totals.c_dcbs_with_draws);
    if (s_dcb_first == NumReasons) {
        Bump(s_totals.c_dcbs_pure);
    } else {
        Bump(s_totals.c_dcb_first[s_dcb_first]);
    }
    s_dcb_c_draws = 0;
    s_dcb_first = NumReasons;
}

double Amdahl(double parallel, double workers) {
    return 1.0 / ((1.0 - parallel) + parallel / workers);
}

/// Appends to a line; the summary is written with write(2) because it also runs from the fault
/// handler (runtime_calls_report).
struct Line {
    char sz_text[2048];
    std::size_t cb_used = 0;
    __attribute__((format(printf, 2, 3))) void Add(const char* sz_format, ...) {
        if (cb_used + 1 >= sizeof(sz_text)) {
            return;
        }
        std::va_list args;
        va_start(args, sz_format);
        const int written = std::vsnprintf(sz_text + cb_used, sizeof(sz_text) - cb_used, sz_format, args);
        va_end(args);
        if (written > 0) {
            cb_used = std::min(sizeof(sz_text) - 1, cb_used + std::size_t(written));
        }
    }
    void Write() {
        Add("\n");
        for (std::size_t off = 0; off < cb_used;) {
            const ssize_t written = write(1, sz_text + off, cb_used - off);
            if (written <= 0) {
                break;
            }
            off += std::size_t(written);
        }
        cb_used = 0;
    }
    /// Through stdout's buffer, in order with the frame stats lines printed before it.
    void Print() {
        Add("\n");
        std::fwrite(sz_text, 1, cb_used, stdout);
        cb_used = 0;
    }
};

double Percent(std::uint64_t part, std::uint64_t whole) {
    return whole ? 100.0 * double(part) / double(whole) : 0.0;
}

/// Everything after the prefix both lines share: draws, reasons, DCBs, time and the projection.
/// `cycles_base`: stage B's busy cycles over the same span, 0 without the draw pipeline.
void AddBody(Line& line, const Snapshot& s, std::uint32_t c_frames, double tsc_hz, std::uint64_t cycles_base) {
    const double per_frame = c_frames ? 1.0 / c_frames : 0.0;
    line.Add(" draws=%llu draws_per_frame=%.1f pure=%.1f%% mutating=%.1f%%", (unsigned long long)s.c_draws,
             double(s.c_draws) * per_frame, Percent(s.c_pure, s.c_draws), Percent(s.c_draws - s.c_pure, s.c_draws));
    for (std::uint32_t r = 0; r < NumReasons; ++r) {
        line.Add(" %s=%.1f%%", ReasonNames[r], Percent(s.c_by_reason[r], s.c_draws));
    }
    const std::uint64_t c_mutating_dcbs = s.c_dcbs_with_draws - s.c_dcbs_pure;
    line.Add("; dcbs=%llu dcbs_per_frame=%.1f dcbs_with_draws_per_frame=%.1f all_pure_dcbs=%.1f%% dcb_first=",
             (unsigned long long)s.c_dcbs, double(s.c_dcbs) * per_frame, double(s.c_dcbs_with_draws) * per_frame,
             Percent(s.c_dcbs_pure, s.c_dcbs_with_draws));
    bool is_first = true;
    for (std::uint32_t r = 0; r < NumReasons; ++r) {
        if (s.c_dcb_first[r]) {
            line.Add("%s%s:%.1f", is_first ? "" : ",", ReasonNames[r], Percent(s.c_dcb_first[r], c_mutating_dcbs));
            is_first = false;
        }
    }
    if (is_first) {
        line.Add("none");
    }
    const bool is_stage_b = cycles_base >= s.cycles_draws && cycles_base != 0;
    const std::uint64_t base = is_stage_b ? cycles_base : s.cycles_draws;
    const double parallel = base ? std::min(1.0, double(s.cycles_pure) / double(base)) : 0.0;
    line.Add("; us_per_draw=%.2f pure_time=%.1f%% time_base=%s amdahl_4=%.2f amdahl_8=%.2f",
             s.c_draws && tsc_hz > 0 ? 1e6 * double(s.cycles_draws) / tsc_hz / double(s.c_draws) : 0.0,
             100.0 * parallel, is_stage_b ? "stage_b" : "draws", Amdahl(parallel, 4), Amdahl(parallel, 8));
}

void AddGpuBusy(Line& line, double gpu_busy_ms, double frame_ms) {
    if (gpu_busy_ms < 0 || frame_ms <= 0) {
        line.Add("; gpu_busy=n/a");
    } else {
        line.Add("; gpu_busy_ms=%.2f frame_ms=%.2f gpu_busy=%.1f%%", gpu_busy_ms, frame_ms,
                 100.0 * gpu_busy_ms / frame_ms);
    }
}

std::uint64_t StageBBusyCycles() {
    const auto* p_busy = PsStats::g_p_stage_b_busy_cycles.load(std::memory_order_acquire);
    return p_busy ? p_busy->load(std::memory_order_relaxed) : 0;
}

// The frame stats thread: run-wide sums of the sampled GPU busy time.
double s_gpu_busy_ms_sum = 0;
std::uint64_t s_c_gpu_busy_frames = 0;

} // namespace

void NoteSlow(Reason reason) {
    if (!s_is_in_draw) {
        return;
    }
    std::uint32_t seen = s_draw_reason.load(std::memory_order_relaxed);
    while (reason < seen &&
           !s_draw_reason.compare_exchange_weak(seen, reason, std::memory_order_relaxed)) {
    }
}

void BeginDcb() {
    if (!s_has_started.load(std::memory_order_relaxed)) {
        s_start = std::chrono::steady_clock::now();
        s_start_tsc = __rdtsc();
        s_first_frame = PsStats::g_frame_number.load(std::memory_order_relaxed);
        s_has_started.store(true, std::memory_order_release);
    }
    std::uint32_t serial = (s_dcb_serial.load(std::memory_order_relaxed) + 1) & DcbMask;
    s_dcb_serial.store(serial ? serial : 1, std::memory_order_relaxed);
    Bump(s_totals.c_dcbs);
}

void BeginSelection() {
    s_is_compile_pending = false;
}

void NotePipelineCompile() {
    s_is_compile_pending = true;
}

void NoteDrainedDraw() {
    s_is_drain_pending = true;
}

std::uint32_t TakeDrawOrigin() {
    const std::uint32_t origin = s_dcb_serial.load(std::memory_order_relaxed) |
                                 (s_is_compile_pending ? CompileBit : 0) | (s_is_drain_pending ? DrainedBit : 0);
    s_is_compile_pending = false;
    s_is_drain_pending = false;
    return origin;
}

void SetDrawOrigin(std::uint32_t origin) {
    s_origin = origin;
    s_has_origin = true;
}

void DrawScope::Begin() {
    const std::uint32_t origin = s_has_origin ? s_origin : TakeDrawOrigin();
    s_has_origin = false;
    const std::uint32_t dcb = origin & DcbMask;
    if (dcb != s_current_dcb.load(std::memory_order_relaxed)) {
        CloseDcb();
        s_current_dcb.store(dcb, std::memory_order_relaxed);
    }
    s_draw_reason.store(NumReasons, std::memory_order_relaxed);
    s_is_in_draw = true;
    s_is_in_stream = true;
    s_is_draw_open.store(true, std::memory_order_release);
    if (origin & CompileBit) {
        NoteSlow(Compile);
    }
    if (origin & DrainedBit) {
        NoteSlow(Sync);
    }
    s_draw_start = __rdtsc();
}

void DrawScope::End() {
    const std::uint64_t cycles = __rdtsc() - s_draw_start;
    s_is_draw_open.store(false, std::memory_order_release);
    s_is_in_draw = false;
    s_is_in_stream = false;
    const std::uint32_t reason = s_draw_reason.load(std::memory_order_acquire);
    Bump(s_totals.c_draws);
    Bump(s_totals.cycles_draws, cycles);
    if (reason == NumReasons) {
        Bump(s_totals.c_pure);
        Bump(s_totals.cycles_pure, cycles);
    } else {
        Bump(s_totals.c_by_reason[reason]);
        if (s_dcb_first == NumReasons) {
            s_dcb_first = reason;
        }
    }
    ++s_dcb_c_draws;
}

void StreamScope::Begin() {
    s_is_in_stream = true;
}

void StreamScope::End() {
    s_is_in_stream = false;
}

void HelperScope::Begin() {
    if (!s_is_in_draw && s_is_draw_open.load(std::memory_order_acquire)) {
        s_is_in_draw = true;
        s_is_in_stream = true;
        s_is_helping = true;
    }
}

void HelperScope::End() {
    if (s_is_helping) {
        s_is_in_draw = false;
        s_is_in_stream = false;
        s_is_helping = false;
    }
}

void NoteLayoutSlow(const void* p_backing, std::uint32_t layout_before, std::uint32_t layout_after) {
    if (!s_is_in_stream) {
        return;
    }
    if (LayoutSlot* p_slot = FindLayout(p_backing)) {
        if (p_slot->layout != UnknownLayout && p_slot->layout != layout_before) {
            NoteSlow(Layout);
        }
        p_slot->layout = layout_after;
    }
}

void ForgetLayoutSlow(const void* p_backing) {
    if (!s_is_in_stream) {
        return;
    }
    if (LayoutSlot* p_slot = FindLayout(p_backing)) {
        p_slot->layout = UnknownLayout;
    }
}

void PrintWindow(double seconds, std::uint32_t c_frames, double gpu_busy_ms, std::uint64_t c_gpu_busy_frames) {
    static Snapshot s_before;
    static std::uint64_t s_before_stage_b = StageBBusyCycles();
    static std::uint64_t s_before_tsc = __rdtsc();
    const Snapshot now = Take();
    const std::uint64_t stage_b = StageBBusyCycles();
    const std::uint64_t tsc = __rdtsc();
    const Snapshot delta = Delta(now, s_before);
    const double tsc_hz = seconds > 0 ? double(tsc - s_before_tsc) / seconds : 0.0;
    Line line;
    line.Add("Native probe:");
    AddBody(line, delta, c_frames, tsc_hz, stage_b >= s_before_stage_b ? stage_b - s_before_stage_b : 0);
    AddGpuBusy(line, gpu_busy_ms, c_frames ? 1000.0 * seconds / c_frames : 0.0);
    line.Print();
    if (gpu_busy_ms >= 0 && c_gpu_busy_frames) {
        s_gpu_busy_ms_sum += gpu_busy_ms * double(c_gpu_busy_frames);
        s_c_gpu_busy_frames += c_gpu_busy_frames;
    }
    s_before = now;
    s_before_stage_b = stage_b;
    s_before_tsc = tsc;
}

} // namespace PsNativeProbe

extern "C" void psport_gpu_native_probe_report(void) {
    using namespace PsNativeProbe;
    if (!g_is_enabled) {
        return;
    }
    static std::atomic<bool> s_has_reported{false};
    if (s_has_reported.exchange(true)) {
        return;
    }
    static Line s_line;
    Line& line = s_line;
    if (!s_has_started.load(std::memory_order_acquire)) {
        line.Add("Native probe summary: seconds=0 frames=0 draws=0 (no command buffer reached the GPU thread)");
        line.Write();
        line.Add("Native probe summary: decision=Undecided (no draws)");
        line.Write();
        return;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
    const std::uint32_t c_frames = PsStats::g_frame_number.load(std::memory_order_relaxed) - s_first_frame;
    const Snapshot totals = Take();
    const double tsc_hz = seconds > 0 ? double(__rdtsc() - s_start_tsc) / seconds : 0.0;
    line.Add("Native probe summary: seconds=%.1f frames=%u", seconds, c_frames);
    AddBody(line, totals, c_frames, tsc_hz, StageBBusyCycles());
    const double gpu_busy_ms = s_c_gpu_busy_frames ? s_gpu_busy_ms_sum / double(s_c_gpu_busy_frames) : -1.0;
    const double frame_ms = c_frames ? 1000.0 * seconds / c_frames : 0.0;
    AddGpuBusy(line, gpu_busy_ms, frame_ms);
    line.Add(" layout_overflow=%llu", (unsigned long long)Load(s_totals.c_layout_overflow));
    line.Write();

    const double pure = Percent(totals.c_pure, totals.c_draws);
    const bool has_gpu_busy = gpu_busy_ms >= 0 && frame_ms > 0;
    const double gpu_busy = has_gpu_busy ? 100.0 * gpu_busy_ms / frame_ms : 0.0;
    const char* sz_decision = totals.c_draws == 0                ? "Undecided"
                              : pure < 90.0                      ? "No-go"
                              : !has_gpu_busy                    ? "Undecided"
                              : gpu_busy < 70.0                  ? "Go"
                                                                 : "No-go";
    line.Add("Native probe summary: decision=%s pure=%.1f%% gpu_busy=", sz_decision, pure);
    if (has_gpu_busy) {
        line.Add("%.1f%%", gpu_busy);
    } else {
        line.Add("n/a");
    }
    line.Add(" rule=pure>=90%%,gpu_busy<70%% main_reasons=");
    std::array<std::uint32_t, NumReasons> order;
    for (std::uint32_t r = 0; r < NumReasons; ++r) {
        order[r] = r;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return totals.c_by_reason[a] > totals.c_by_reason[b]; });
    bool is_first = true;
    for (std::uint32_t i = 0; i < 3; ++i) {
        if (const std::uint64_t c_draws = totals.c_by_reason[order[i]]) {
            line.Add("%s%s:%.1f", is_first ? "" : ",", ReasonNames[order[i]], Percent(c_draws, totals.c_draws));
            is_first = false;
        }
    }
    if (is_first) {
        line.Add("none");
    }
    line.Write();
}
