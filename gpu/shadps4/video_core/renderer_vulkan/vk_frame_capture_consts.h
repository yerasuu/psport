// SPDX-License-Identifier: GPL-2.0-or-later
// psport: PS_CAPTURE_DUMP_VSCONST=<one PS_SKIP_DRAWS entry>, part of the frame capture
// (vk_frame_capture.h). For every draw of a recorded frame the selector takes, the user data SGPRs
// of its vertex and pixel stages and every buffer they read as bound: the constant ring's and the
// flattened user data's bytes as the GPU reads them, the guest memory under a V# when the draw was
// recorded and a GPU copy of its buffer at the pass's end. Beside each range, the writes into it
// the GPU side recorded or heard of since the capture was armed (a journal kept only while the
// capture is armed or recording). Written as `<stem>_vsconst.txt` beside the frame file.

#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "psport_skip_draws.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_frame_capture_draw.h"

namespace Vulkan::CaptureConsts {

/// PS_CAPTURE_DUMP_VSCONST, parsed once; null when unset or malformed.
const PsSkipDraws::Selector* Selector();

/// The capture was armed: the journal starts over (any thread).
void ClearJournal();
/// Frame start: the selected draws and the counts start over; the journal keeps what came since
/// the capture was armed or the previous frame was written.
void StartFrame();

/// GPU thread: where the command stream is, for the journal's entries.
void SetPosition(u32 i_pass, u32 i_draw);

/// `is_recording`: a frame is being recorded; only then are a draw's stages candidates.
void BeginBindings(bool is_recording, bool is_compute, u64 vs_hash, u64 ps_hash);
bool IsCandidate(const Shader::Info& stage);
void NoteUserData(const Shader::Info& stage, u32 i_first_push);
void NoteBuffer(const Shader::Info& stage, const FrameCapture::StageBuffer& buffer);
/// A write the draw or dispatch being bound will do, journaled when it is recorded.
void NotePendingWrite(VAddr va_range, u64 cb_range, FrameCapture::WriteSource source);
/// Any thread; `tick`: the scheduler's, 0 off the GPU side.
void Journal(VAddr va_range, u64 cb_range, FrameCapture::WriteSource source, u64 detail, u64 tick);

/// The draw was recorded (or skipped by PS_SKIP_DRAWS): its pending data are kept when the
/// selector takes it, its pending writes journaled.
void TakeDraw(u32 i_pass, u32 i_draw, u64 vs_hash, u64 ps_hash, u32 i_pair_draw,
              const PsSkipDraws::Target& color0, bool is_skipped, u64 tick);
void TakeDispatch(u32 i_pass, u64 tick);

/// The GPU buffers the selected draws of pass `i_pass` read, not yet copied; valid until the next
/// selected draw.
std::vector<CaptureDraw::GpuRange*> PassRanges(u32 i_pass);

/// Frame end, after the GPU copies landed in `p_gpu_copies` (null: none): writes
/// `<stem>_vsconst.txt` and logs its summary.
void WriteFrame(const std::string& stem, const u8* p_gpu_copies);

} // namespace Vulkan::CaptureConsts
