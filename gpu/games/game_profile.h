// SPDX-License-Identifier: GPL-2.0-or-later
// Game profiles: what the translator knows about one game, kept out of its core. The core (the
// command processor, the caches, the shader recompiler) runs any game on its generic paths and
// asks the active profile only for the rest: hooks in the game's own code that announce its
// writes earlier than a write trap would, guest code known to write per-frame data, shaders with
// a known purpose, diagnostics that need to know its code. A game without a profile runs on the
// generic paths alone. One file per game (games/<game>.cpp, or games/<game>/profile.cpp beside the
// code it hooks in) defines its profile; game_profile.cpp lists them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace Game {

/// Guest addresses [begin, end).
struct CodeRange {
    std::uint64_t begin = 0, end = 0;
};

/// Guest code that frees the game's GPU label blocks (the fence targets of its command buffers),
/// for the free check's PS_LABEL_TRAP=2 breakpoints (bbport_free_check.h).
struct LabelReleaseCode {
    /// The allocator's release: its first instruction must be `mov rax, rsi` followed by
    /// `test rax, rax`, rsi the block released.
    std::uint64_t release = 0;
    /// The collector's branch for a block it takes as done: its first instruction must be
    /// `mov rcx, [rsp + 0x28]`, r14 the block (its first qword the label, or with bit 0 set a
    /// pointer to the label of its batch).
    std::uint64_t collector_done = 0;
};

/// What the core offers a profile's hooks.
struct CoreServices {
    /// The game handed out GPU memory [address, address + size): what the GPU had there is dead.
    void (*fresh_range)(std::uint64_t address, std::uint64_t size) = nullptr;
};

struct Profile {
    const char* name = nullptr;
    /// The param.sfo TITLE_IDs it is for.
    std::span<const char* const> serials;
    /// Hooks in the game's code (each checks the bytes it replaces: another build of the game is
    /// left alone). They make the core learn of writes sooner; it does not depend on them for
    /// what it shows (write traps catch what nobody announces).
    void (*install_hooks)(const CoreServices& services) = nullptr;
    /// Patches the game's code once, before any of it runs, while the image is still writable
    /// (each patch checks the bytes it replaces, as the hooks do).
    void (*patch_image)(unsigned char* image, std::uint64_t size) = nullptr;
    /// Guest code known to write per-frame data into GPU memory: the blocks it writes stay in
    /// the game's memory from its first write instead of after a few frames.
    std::span<const CodeRange> dynamic_writers;
    /// Changes to the user's saves the game needs, made before any of its code runs. saves is the
    /// folder holding one folder per user, each with the game's saves under save_title.
    void (*prepare_saves)(const char* saves, const char* save_title) = nullptr;
    /// A compute shader (GCN program hash) that only copies buffers, into memory the translator
    /// reads on the CPU: its stores go through to the game's memory; 0 = none known.
    std::uint64_t buffer_copy_shader = 0;
    /// The core's GnmDriver wrote packets into command buffer space the game reserved,
    /// [commands, commands + dwords) (the packets end at the first zero dword). Called on the
    /// game's thread after each such sceGnm call returns its packets; it must not write them.
    void (*driver_commands_written)(std::uint32_t* commands, std::uint64_t dwords) = nullptr;
    /// The game submitted [commands, commands + dwords) (each DCB and CCB of a submission, on the
    /// submitting thread, before the core translates it); it must not write them.
    void (*commands_submitted)(const std::uint32_t* commands, std::uint64_t dwords) = nullptr;
    /// Once a frame while frames overlap (PS_SUBMIT_AHEAD above 0), from sceGnmSubmitDone under
    /// the submission lock: true when the game shows it cannot take the overlap, with why in
    /// reason (a NUL-terminated phrase that fits in size); the core then stops it for the session.
    bool (*frame_overlap_unsafe)(char* reason, std::size_t size) = nullptr;
    /// With each frame statistics report (PS_FRAME_STATS, the video out thread): the profile's
    /// own diagnostics.
    void (*report_stats)() = nullptr;
    /// Each presented frame, on the presenter thread before the overlay is drawn: what the
    /// player changed in the game's own menus is applied here.
    void (*before_present)() = nullptr;
    /// The game's label release code; zeros when unknown, and the free check's breakpoints stay
    /// off.
    LabelReleaseCode label_release;
};

/// Chooses the profile for the game by its TITLE_ID (bbgpu_init). PS_GAME_PROFILE=none runs the
/// game as an unknown one (no profile), for checking the generic paths.
void Select(const char* serial);

/// The active profile, or nullptr (an unknown game, or PS_GAME_PROFILE=none).
const Profile* Active();

/// The active profile's buffer copy shader, or 0.
inline std::uint64_t BufferCopyShader() {
    const Profile* profile = Active();
    return profile ? profile->buffer_copy_shader : 0;
}

/// Whether guest_rip is in code the active profile knows to write per-frame data.
bool IsDynamicWriter(std::uint64_t guest_rip);

} // namespace Game
