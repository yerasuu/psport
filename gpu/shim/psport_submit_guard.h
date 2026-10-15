// SPDX-License-Identifier: GPL-2.0-or-later
// psport: frames overlap (PS_SUBMIT_AHEAD) only while the game leaves a submitted frame's command
// buffers alone until the GPU has retired it. Two watches on every submitted DCB and CCB: its
// whole pages are read-only for the game until its frame retires (runtime_memory_trap,
// FrameCommands), and its bytes are checksummed at submit and again at retire. A game that builds
// the next frame over them (Gal Guardians 2: another thread reuses the region of F-1 as soon as
// the submit of F returns, so the GPU's late labels land in the new commands and pending draws
// read descriptor tables already overwritten) faults once: that thread waits for the frame, and
// the frames no longer overlap for the session (PS_SUBMIT_AHEAD 0, the submission lock's
// contract). The checksum catches what no page fault shows (writes by the kernel or the GPU,
// memory above the trap table) a frame later. PS_SUBMIT_GUARD=0 turns both off, =trap or =hash
// keeps one.
#pragma once
#include <cstdint>

namespace PsSubmitGuard {
/// A command buffer handed to the GPU as part of frame `frame` (numbered by the sceGnmSubmitDone
/// that closes it, from 1): watched until Retire(frame).
void Arm(const void* address, std::uint64_t size, std::uint64_t frame);
/// sceGnmSubmitDone closed frame `frame`: a write into its buffers from now on waits for the GPU.
void Close(std::uint64_t frame);
/// The GPU has finished every frame up to `frame`: their buffers are checked and the game's again.
void Retire(std::uint64_t frame);
/// The game wrote into a frame in flight: no overlap for the rest of the session.
bool Tripped();
} // namespace PsSubmitGuard
