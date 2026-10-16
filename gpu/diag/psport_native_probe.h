// SPDX-License-Identifier: GPL-2.0-or-later
// psport: PS_NATIVE_PROBE=1 — whether the draws of each command buffer the game submits (DCB) could
// be recorded in parallel (decision #361, phase P1). Each draw the recording thread records
// (Rasterizer::DrawRecord, DrawIndirectRecord) is pure when it only hits memos and binds images,
// views and buffers that exist in a layout its own DCB predicts, or mutating, under the earliest
// Reason among those noted while it ran (a memo miss is noted before the image it then creates). Printed every 5 s after the frame stats ("Native probe:", needs
// PS_FRAME_STATS=1) and once at exit after the command stream census ("Native probe summary:"),
// both `key=value`, read by tools/regress/report.py. Off, every hook is one load of a constant flag.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace PsNativeProbe {

inline const bool g_is_enabled = [] {
    const char* sz_env = std::getenv("PS_NATIVE_PROBE");
    return sz_env && sz_env[0] == '1';
}();

/// Why a draw is mutating, most telling first: the order of the report's columns.
enum Reason : std::uint32_t {
    NewImage,
    NewView,
    Upload,
    BufferCreate,
    Layout,
    Sync,
    Compile,
    Descriptors,
    MemoMiss,
    Other,
    NumReasons,
};

void NoteSlow(Reason reason);
/// Any thread: counted when the calling thread records a draw or binds its textures (HelperScope).
inline void Note(Reason reason) {
    if (g_is_enabled) {
        NoteSlow(reason);
    }
}

/// GPU command thread: a top-level graphics command buffer starts.
void BeginDcb();
/// GPU command thread, pipeline selection: a new selection starts, and its pipeline was not in
/// the cache (compiled now, by a worker, or waited for).
void BeginSelection();
void NotePipelineCompile();
/// GPU command thread: the draw it records next waited for the draw recording thread to run dry.
void NoteDrainedDraw();
/// GPU command thread: what the draw it hands to the recording thread carries (its DCB, and the
/// compile noted since BeginSelection); the recording thread passes it to SetDrawOrigin.
std::uint32_t TakeDrawOrigin();
void SetDrawOrigin(std::uint32_t origin);

/// Image state before a full-image barrier on `p_backing` and the layout it moves to; a partial
/// one forgets the image. A layout the current DCB did not leave is unpredictable.
void NoteLayoutSlow(const void* p_backing, std::uint32_t layout_before, std::uint32_t layout_after);
void ForgetLayoutSlow(const void* p_backing);
inline void NoteLayout(const void* p_backing, std::uint32_t layout_before, std::uint32_t layout_after) {
    if (g_is_enabled) {
        NoteLayoutSlow(p_backing, layout_before, layout_after);
    }
}
inline void ForgetLayout(const void* p_backing) {
    if (g_is_enabled) {
        ForgetLayoutSlow(p_backing);
    }
}

/// The recording thread, for one draw: classified and timed (TSC) from construction to
/// destruction.
class DrawScope {
public:
    DrawScope() {
        if (g_is_enabled) {
            Begin();
        }
    }
    ~DrawScope() {
        if (g_is_enabled) {
            End();
        }
    }
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;

private:
    static void Begin();
    static void End();
};

/// The recording thread, for one dispatch: its barriers set the layouts the DCB predicts, its
/// notes count for no draw.
class StreamScope {
public:
    StreamScope() {
        if (g_is_enabled) {
            Begin();
        }
    }
    ~StreamScope() {
        if (g_is_enabled) {
            End();
        }
    }
    StreamScope(const StreamScope&) = delete;
    StreamScope& operator=(const StreamScope&) = delete;

private:
    static void Begin();
    static void End();
};

/// The texture binding helper thread, while it binds for the recording thread: its notes count
/// for the draw open there.
class HelperScope {
public:
    HelperScope() {
        if (g_is_enabled) {
            Begin();
        }
    }
    ~HelperScope() {
        if (g_is_enabled) {
            End();
        }
    }
    HelperScope(const HelperScope&) = delete;
    HelperScope& operator=(const HelperScope&) = delete;

private:
    static void Begin();
    static void End();
};

/// The frame stats thread, every window: prints the "Native probe:" line. `gpu_busy_ms` is the
/// sampled GPU busy time per frame, negative without the sampler.
void PrintWindow(double seconds, std::uint32_t c_frames, double gpu_busy_ms, std::uint64_t c_gpu_busy_frames);

} // namespace PsNativeProbe
