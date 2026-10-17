// SPDX-License-Identifier: GPL-2.0-or-later
// What psport knows about one game, kept out of its core, in two parts. Its Title is part of
// running the game and always applies, found by TITLE_ID whatever PS_GAME_PROFILE says: the launch
// (serials, mod folders, its file check, its patches and their settings), the changes its saves
// need, and the port's settings pages in its own menu. Its Profile is what the translator knows
// about its code, and PS_GAME_PROFILE=none turns it off to check the generic paths: hooks in the
// game's own code that announce its writes earlier than a write trap would, guest code known to
// write per-frame data, shaders with a known purpose, where the upscaler goes, diagnostics that
// need to know its code. A game without either runs on the generic paths alone. One folder per
// game (games/<game>/: title.cpp, profile.cpp beside the code they hook in) defines them;
// game_profile.cpp lists the titles.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include "../psport_gpu.h"
#include "psport_quirks.h"

namespace Game {

/// Guest addresses [begin, end).
struct CodeRange {
    std::uint64_t begin = 0, end = 0;
};

/// Guest code that frees the game's GPU label blocks (the fence targets of its command buffers),
/// for the free check's PS_LABEL_TRAP=2 breakpoints (psport_free_check.h).
struct LabelReleaseCode {
    /// The allocator's release: its first instruction must be `mov rax, rsi` followed by
    /// `test rax, rax`, rsi the block released.
    std::uint64_t release = 0;
    /// The collector's branch for a block it takes as done: its first instruction must be
    /// `mov rcx, [rsp + 0x28]`, r14 the block (its first qword the label, or with bit 0 set a
    /// pointer to the label of its batch).
    std::uint64_t collector_done = 0;
};

/// The game's small-object pool, for the free check (PS_FREE_CHECK, psport_free_check.h): 4 KiB
/// pages, blocks of cb_block from the page start, a header at page + off_header (page list
/// next and prev, free list head and free count, a qword each), the free list linked through the
/// first qword of each free block. A page has at most 128 blocks.
struct SmallObjectPool {
    std::uint64_t off_header = 0;
    std::uint64_t cb_block = 0;
};

/// The scene constants the camera motion vectors come from: a vertex shader constant buffer
/// (stride 16) of the G-buffer draws, told apart from the game's other buffers of its size by the
/// far plane and its reciprocal. Offsets are in floats from the buffer's start; every value read
/// must lie inside cb_constants. Zeros when unknown: the upscaler never has a camera and passes
/// the scene through at its own resolution.
struct CameraConstants {
    std::uint32_t cb_constants = 0;
    /// The far plane at off_dw_far, its reciprocal in the next float.
    float far_plane = 0;
    std::uint32_t off_dw_far = 0;
    /// The scene's render width and height, two floats.
    std::uint32_t off_dw_render_size = 0;
    /// World to view, 3x4 rows.
    std::uint32_t off_dw_view = 0;
    /// The inverse of the view, 3x4 rows (PS_CAMERA_INVERSE=own computes it instead).
    std::uint32_t off_dw_inverse_view = 0;
    /// The projection, 4x4 rows: x scale at +0, y scale at +5, z scale at +10 and z offset at +11
    /// (depth = z scale + z offset / view z).
    std::uint32_t off_dw_projection = 0;
};

/// The vertex shader constant buffers (stride 16) holding bone palettes of 3x4 matrices, cb_bone
/// bytes each, which give a draw object motion vectors. A buffer from cb_min_skeleton to
/// cb_max_skeleton bytes is a character's skeleton: its draws always keep a vertex history. A
/// multiple of cb_bone from cb_min_small_skeleton up to cb_min_skeleton is a small one (weapons
/// and props, but also static pieces): its draws keep one only while the palette changes. The
/// camera constants' size is never a palette. Zeros when unknown: no draw gets object motion, and
/// the upscaler has the camera's alone.
struct BonePalettes {
    std::uint32_t cb_bone = 0;
    std::uint32_t cb_min_small_skeleton = 0;
    std::uint32_t cb_min_skeleton = 0;
    std::uint32_t cb_max_skeleton = 0;
};

/// Where the temporal upscaler goes in the game's frame (docs/en/upscaler.md): it upscales the
/// scene color right before the first dispatch of scene_combine_shader, and the frame's UI starts
/// with the first draw of ui_shader (GCN program hashes). The upscaler, its jitter and the camera
/// and object motion vectors run only once the scene combine is known.
struct UpscalerPlacement {
    std::uint64_t scene_combine_shader = 0;
    std::uint64_t ui_shader = 0;
    /// Vertex shaders of the UI's draws (ui_shader is one even when not listed): when the scene is
    /// rendered below the output size, their draws go to the output-size UI instead.
    std::span<const std::uint64_t> ui_movie_shaders;
    /// The size of the game's scene targets, which scene scaling (presets, outputs other than
    /// 1080p) replaces with proxies, and the divisor of its post-processing targets
    /// (scene_width / post_divisor wide), scaled by the same factor; post_divisor 1 when it has
    /// none smaller. The defaults hold without a profile too: the scene scaling runs for any game
    /// whose output is not 1080p.
    std::uint32_t scene_width = 1920;
    std::uint32_t scene_height = 1080;
    std::uint32_t post_divisor = 2;
    CameraConstants camera;
    BonePalettes bone_palettes;
};

/// What the core offers a profile's hooks.
struct CoreServices {
    /// The game handed out GPU memory [address, address + size): what the GPU had there is dead.
    void (*pfn_fresh_range)(std::uint64_t va_range, std::uint64_t cb_range) = nullptr;
};

/// An overlay menu text in its languages (PsSettings::MenuText); all null for none.
struct LocalizedText {
    const char* sz_english = nullptr;
    const char* sz_russian = nullptr;
    const char* sz_spanish = nullptr;
};

/// A switch of the game's own patches: a psport.ini key (0 or 1) set in the overlay menu and the
/// launcher, which pfn_add_patches reads at start, so a change applies after a restart.
struct EffectSwitch {
    const char* sz_key = nullptr;
    LocalizedText label;
    /// The game's own behaviour, unpatched.
    bool is_default_on = false;
    LocalizedText hint;
};

/// What the overlay's "Game effects" section offers; with neither, the section is not shown.
struct GameEffects {
    std::span<const EffectSwitch> switches;
    /// The model_lod choice of psport.ini (-2 highest .. 2 lowest, 0 the game's).
    bool has_model_detail = false;
    /// After the section's note that the patches apply at start (from sz_patch_file).
    LocalizedText hint;
};

/// The servers the game's online play (PS_ONLINE=1) goes to, which run.sh's host overrides
/// redirect (psport --online-overrides, scripts/online.py); all unset for a title without any.
struct OnlineServices {
    /// The game's own server as it addresses it (scheme://host:port); the community server answers
    /// for it on the same port.
    const char* sz_game_server = nullptr;
    /// The community server, unless PS_COMMUNITY_SERVER names another.
    const char* sz_community_server = nullptr;
    /// The port the community server sends the game to for its API (summons among them).
    std::uint16_t community_api_port = 0;
    /// The path of its summon calls on that API, without a trailing slash: routed to the shadNet
    /// WebAPI when that has the service, and the only path the online module's summon helpers
    /// (host placement, SHADPS4_BLOODBORNE_SEAMLESS_COOP, SHADPS4_CAPTURE_BLOODBORNE_SUMMON) act on.
    const char* sz_summon_route = nullptr;
};

/// The mouse in the game's own code (keyboard-and-mouse play), from hooks the profile installs.
struct MouseHooks {
    /// Native mouse look (src/input/runtime_pad.c): turns the game's camera by pitch and yaw
    /// (radians) at its next update. False while the camera does not take turns (lock-on, menus)
    /// or its hook is not in: the mouse drives the right stick then.
    bool (*pfn_camera_look)(float pitch, float yaw) = nullptr;
    /// Every window event first (an SDL_Event and the SDL_Window, on the window thread): true takes
    /// it, and neither the port nor the game sees it.
    bool (*pfn_window_event)(const void* p_sdl_event, void* p_sdl_window) = nullptr;
    /// True while the game shows a menu the mouse points into: the window lets the mouse go and
    /// shows the cursor, and takes the mouse back after (as it had it).
    bool (*pfn_wants_pointer)() = nullptr;
    /// Changes to the pad state the game reads (runtime_pad.c, after the host's and the scripted
    /// input, on the game's thread): the buttons (PS4 bits); the sticks (lx ly rx ry) as they are.
    void (*pfn_filter_pad)(std::uint32_t* p_buttons, const std::uint8_t* p_sticks) = nullptr;
};

/// The translator's knowledge of the game's code, gated by PS_GAME_PROFILE: it makes the game
/// faster or easier to diagnose, never makes it work.
struct Profile {
    /// Hooks in the game's code (each checks the bytes it replaces: another build of the game is
    /// left alone). They make the core learn of writes sooner; it does not depend on them for
    /// what it shows (write traps catch what nobody announces).
    void (*pfn_install_hooks)(const CoreServices& services) = nullptr;
    /// Patches the game's code once, before any of it runs, while the image is still writable
    /// (each patch checks the bytes it replaces, as the hooks do).
    void (*pfn_patch_image)(unsigned char* p_image, std::uint64_t cb_range) = nullptr;
    /// True once the profile's hooks pace the game's frames themselves: the video out shows each
    /// frame as it comes, whatever flip rate the game asks for (sceVideoOutSetFlipRate).
    bool (*pfn_drives_frame_rate)() = nullptr;
    /// Guest code known to write per-frame data into GPU memory: the blocks it writes stay in
    /// the game's memory from its first write instead of after a few frames.
    std::span<const CodeRange> dynamic_writers;
    /// A compute shader (GCN program hash) that only copies buffers, into memory the translator
    /// reads on the CPU: its stores go through to the game's memory; 0 = none known.
    std::uint64_t buffer_copy_shader = 0;
    /// The core's GnmDriver wrote packets into command buffer space the game reserved,
    /// [commands, commands + dwords) (the packets end at the first zero dword). Called on the
    /// game's thread after each such sceGnm call returns its packets; it must not write them.
    void (*pfn_driver_commands_written)(std::uint32_t* p_commands, std::uint64_t dwords) = nullptr;
    /// The game submitted [commands, commands + dwords) (each DCB and CCB of a submission, on the
    /// submitting thread, before the core translates it); it must not write them.
    void (*pfn_commands_submitted)(const std::uint32_t* p_commands, std::uint64_t dwords) = nullptr;
    /// Once a frame while frames overlap (PS_SUBMIT_AHEAD above 0), from sceGnmSubmitDone under
    /// the submission lock: true when the game shows it cannot take the overlap, with why in
    /// reason (a NUL-terminated phrase that fits in size); the core then stops it for the session.
    bool (*pfn_frame_overlap_unsafe)(char* sz_reason, std::size_t cb_range) = nullptr;
    /// Submissions of a frame that go to the GPU before it has finished the frame before
    /// (PS_SUBMIT_AHEAD, which overrides it). Only for a game whose first c_submits_ahead
    /// submissions of a frame never touch what the previous frame's GPU work still uses; 0, the
    /// submission lock's contract, for any other.
    std::uint32_t c_submits_ahead = 0;
    /// Anisotropic filtering forced on the game's mipmapped textures in graphics draws (2, 4, 8 or
    /// 16; PS_ANISO overrides it); 0 keeps the game's own samplers.
    std::uint32_t anisotropy = 0;
    /// Zeros when unknown: the temporal upscaler stays off (PS_UPSCALE_BEFORE_CS and
    /// PS_UI_TRIGGER_VS place it by hand).
    UpscalerPlacement upscaler;
    /// The output's aspect ratio (width / height), from the upscaler when the chosen output's
    /// changes: true when the game's camera, HUD and UI placement follow it (its hooks are in),
    /// and a wider (taller) output shows more to the sides (above and below), with the UI's
    /// full-screen fills reaching the output's edges (UiComposition::EdgeParam). false, or
    /// nullptr: the core keeps the game's 16:9 picture in the middle of any output.
    bool (*pfn_display_aspect)(float aspect) = nullptr;
    /// With each frame statistics report (PS_FRAME_STATS, the video out thread): the profile's
    /// own diagnostics.
    void (*pfn_report_stats)() = nullptr;
    /// The game's label release code; zeros when unknown, and the free check's breakpoints stay
    /// off.
    LabelReleaseCode label_release;
    /// The pool holding the game's GPU labels; zeros when unknown, and the free check stays off.
    SmallObjectPool small_object_pool;
    MouseHooks mouse;
};

/// The game as psport runs it, whatever PS_GAME_PROFILE says.
struct Title {
    const char* sz_name = nullptr;
    /// The param.sfo TITLE_IDs it is for (one executable: patch files and mod folders named after
    /// any of them apply to all).
    std::span<const char* const> serials;
    /// Folders a loose-file mod may consist of without dvdroot_ps4 around them (src/loader/loader_mods.c
    /// adds the game's own): those of the game's dvdroot_ps4 and the ones mods add to it.
    std::span<const char* const> mod_folders;
    /// Its own patch file (shadPS4 XML), relative to psport's folder; external patch folders leave
    /// it out.
    const char* sz_patch_file = nullptr;
    /// Before anything of the game runs, before psport_gpu_init: environment defaults it chooses.
    void (*pfn_prepare_environment)() = nullptr;
    /// Checks the game's files before any of its code runs (PS_SKIP_GAME_CHECK=1 is its own to
    /// honour): false when they cannot work, with a short kind for launchers and what to do.
    bool (*pfn_check_files)(const PsGameFiles& game, std::string& kind, std::string& advice) = nullptr;
    /// Its own patches of the executable, chosen by the player's settings, appended through
    /// services before the external ones; false stops the launch (it printed why). A title with
    /// them has frame rate and resolution presets (psport --has-presets).
    bool (*pfn_add_patches)(const PsGameFiles& game, const PsPatchServices& services) = nullptr;
    /// The settings of pfn_add_patches the player changes in the overlay menu.
    GameEffects effects;
    /// Changes to the user's saves the game needs, made before any of its code runs. saves is the
    /// folder holding one folder per user, each with the game's saves under sz_save_title.
    void (*pfn_prepare_saves)(const char* sz_saves, const char* sz_save_title) = nullptr;
    /// Patches the game's code once, before any of it runs, while the image is still writable,
    /// after the profile's (each patch checks the bytes it replaces): the port's own pages in the
    /// game's menus.
    void (*pfn_patch_image)(unsigned char* p_image, std::uint64_t cb_range) = nullptr;
    /// Each presented frame, on the presenter thread before the overlay is drawn: what the
    /// player changed in the game's own menus is applied here.
    void (*pfn_before_present)() = nullptr;
    /// Its keyboard and mouse layout: psport.ini key.<input>= and pad.<input>= lines, one per line,
    /// over the neutral layout (src/input/runtime_pad.c); the player's own lines win over both.
    /// nullptr keeps the neutral one.
    const char* sz_default_controls = nullptr;
    OnlineServices online;
    /// Its entries of the quirk registry (src/runtime/runtime_quirks.inc): LAUNCH ones always
    /// apply, WORKAROUND ones also with PS_GAME_PROFILE=none, OPTIMIZATION ones not then
    /// (docs/en/QUIRKS.md). Only this list activates a WORKAROUND, whose code lives in the title's
    /// folder.
    std::span<const RuntimeQuirkId> quirks;
    /// Its translator profile; nullptr for a title without one.
    const Profile* p_profile = nullptr;
};

/// The title for a TITLE_ID, whatever PS_GAME_PROFILE says.
const Title* Find(const char* sz_serial);

/// Chooses the game's title by its TITLE_ID, and with it the active profile (psport_gpu_init).
/// PS_GAME_PROFILE=none leaves the profile off (the generic paths only); the title still applies.
void Select(const char* sz_serial);

/// The title of the running game (Find at Select), or nullptr.
const Title* Launched();

/// The active profile: the running title's, or nullptr (an unknown game, a title without a
/// profile, or PS_GAME_PROFILE=none).
const Profile* Active();

/// The running title's effects (Launched), or none.
inline GameEffects Effects() {
    const Title* p_title = Launched();
    return p_title ? p_title->effects : GameEffects{};
}

/// The active profile's buffer copy shader, or 0.
inline std::uint64_t BufferCopyShader() {
    const Profile* p_profile = Active();
    return p_profile ? p_profile->buffer_copy_shader : 0;
}

/// The active profile's frame overlap (Profile::c_submits_ahead), or 0.
inline std::uint32_t SubmitsAhead() {
    const Profile* p_profile = Active();
    return p_profile ? p_profile->c_submits_ahead : 0;
}

/// The active profile's forced anisotropy, or 0.
inline std::uint32_t Anisotropy() {
    const Profile* p_profile = Active();
    return p_profile ? p_profile->anisotropy : 0;
}

/// The active profile's upscaler placement and motion data, or zeros.
inline UpscalerPlacement Upscaler() {
    const Profile* p_profile = Active();
    return p_profile ? p_profile->upscaler : UpscalerPlacement{};
}

/// The active profile's mouse hooks, or none.
inline MouseHooks Mouse() {
    const Profile* p_profile = Active();
    return p_profile ? p_profile->mouse : MouseHooks{};
}

/// Whether the active profile's hooks pace the game's frames (Profile::pfn_drives_frame_rate).
inline bool DrivesFrameRate() {
    const Profile* p_profile = Active();
    return p_profile && p_profile->pfn_drives_frame_rate && p_profile->pfn_drives_frame_rate();
}

/// Whether guest_rip is in code the active profile knows to write per-frame data.
bool IsDynamicWriter(std::uint64_t guest_rip);

} // namespace Game
