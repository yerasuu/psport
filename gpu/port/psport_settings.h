// SPDX-License-Identifier: GPL-2.0-or-later
// psport: user settings changed at run time from the in-game menu (psport_overlay.h) and kept
// in psport.ini (PS_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace PsSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
/// Frame generation (vk_frame_generation.h): off, or FSR 3.1 frame interpolation (2x) where the
/// device has its prerequisites. psport.ini also names `auto` and `dlss`, not available yet: they
/// leave it off.
enum FrameGen : int { FrameGenOff = 0, FrameGenFsr = 1, FrameGenCount };
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
enum class MenuLanguage { English, Russian, Spanish };

/// The most effect switches a game title offers (Game::GameEffects) the settings keep.
inline constexpr int MaxEffects = 16;
/// Output aspect ratios, and the output resolutions of each: the upscaler's output and the UI host
/// targets, changed live. The game keeps its own 1920x1080 frame; a profile whose camera follows the
/// output (Game::Profile::pfn_display_aspect) shows more to the sides (above and below) with its UI
/// at the screen's edges, any other game keeps its 16:9 picture in the middle of the output. Auto:
/// the group of the monitor the game's window is on.
enum Aspect : int { AspectAuto = 0, Aspect4x3, Aspect16x10, Aspect16x9, Aspect21x9, Aspect32x9, AspectCount };
struct OutputSize {
    int width = 1920, height = 1080;
    bool operator==(const OutputSize&) const = default;
};
inline constexpr OutputSize OutputDefault{1920, 1080}; ///< the game's own size
constexpr std::uint32_t PackSize(OutputSize size) {
    return std::uint32_t(size.width) | std::uint32_t(size.height) << 16;
}
constexpr OutputSize UnpackSize(std::uint32_t packed) {
    return {int(packed & 0xffff), int(packed >> 16)};
}
/// An aspect ratio's outputs, smallest first.
struct OutputList {
    std::array<OutputSize, 12> sizes{};
    int c_sizes = 0;
    const OutputSize* begin() const { return sizes.data(); }
    const OutputSize* end() const { return sizes.data() + c_sizes; }
    /// -1: not in the list.
    int IndexOf(OutputSize size) const;
};
/// psport.ini value ("auto", "4:3", "16:10", "16:9", "21:9", "32:9").
const char* AspectKey(int aspect);
/// The monitor the game's window is on (window.cpp): Auto follows it. 0x0 until known.
void SetMonitor(int width, int height);
OutputSize Monitor();
/// Auto resolved to the group nearest the monitor's aspect ratio (16:9 until it is known).
int ResolvedAspect(int aspect);
/// The output resolutions of an aspect ratio (Auto: the monitor's group and its own size).
OutputList Outputs(int aspect);
/// The output of aspect nearest size in height: another aspect ratio keeps the height.
OutputSize NearestOutput(int aspect, OutputSize size);
/// The output in use: the chosen resolution in the chosen aspect ratio.
OutputSize CurrentOutput();
/// "2560x1080".
std::string SizeText(OutputSize size);

struct Values {
    std::atomic<MenuLanguage> menu_language{MenuLanguage::Russian};
    /// Chosen in the menu (or already in psport.ini): only then is it saved, so the language
    /// the menu merely started with keeps following the launcher's (#134).
    std::atomic<bool> is_menu_language_chosen{false};
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> should_sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> should_jitter{true};
    std::atomic<bool> should_use_reactive{false};
    std::atomic<bool> should_use_object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> should_show_fps{false};
    /// The settings menu's position (fraction of the screen), -1 until it is moved.
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> should_use_fsr4_auto_exposure{true};
    std::atomic<bool> should_invert_fsr4_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// The game's camera and UI follow the output's aspect ratio (Game::Profile::pfn_display_aspect);
    /// set by the upscaler.
    std::atomic<bool> is_aspect_followed{false};
    /// The presented frame's width / height, which the window letterboxes; set by the presenter.
    std::atomic<float> shown_frame_aspect{1920.0f / 1080.0f};
    /// Applied at start (the game title's patches), in the order of Game::Effects()'s switches,
    /// c_effects of them; the menu shows when a restart is needed.
    std::atomic<bool> effects[MaxEffects]{};
    int c_effects = 0;
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    /// 16:9 unless chosen: the game's own (psport.ini aspect_ratio).
    std::atomic<int> aspect{Aspect16x9};
    /// The chosen output resolution (PackSize; CurrentOutput fits it to the aspect ratio).
    std::atomic<std::uint32_t> output_res{PackSize(OutputDefault)};
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> sz_fsr4_problem{nullptr};
    std::atomic<bool> is_fsr4_supported{false}, is_fsr411_supported{false};
    /// DLSS (gpu/dlss_bridge, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> is_dlss_supported{false};
    std::atomic<const char*> sz_dlss_problem{nullptr};
    /// Frame generation, chosen at start (psport.ini frame_gen, PS_FRAME_GEN): the presentation
    /// frames' format and the swapchain's image count follow it. The renderer turns it off when it
    /// cannot run (sz_frame_gen_problem says why).
    std::atomic<int> frame_gen{FrameGenOff};
    std::atomic<const char*> sz_frame_gen_problem{nullptr};
    /// The menu's choice, written by Save(); it runs from the next start.
    std::atomic<int> frame_gen_chosen{FrameGenOff};
    /// PS_FRAME_GEN was set (a launcher's per-game value): it wins over psport.ini, which the menu
    /// then leaves alone; the launcher learns the menu's changes from ChooseFrameGen's line.
    bool is_frame_gen_from_env = false;
    /// FSR 3.1 frame generation's prerequisites on the device the game takes (set at start).
    std::atomic<bool> is_frame_gen_fsr_supported{false};
    /// Generated frames only where the display's refresh rate has room for them (frame_gen_fit_display).
    std::atomic<bool> should_fit_frame_gen_to_display{false};
    /// Set by the presenter: generated frames per game frame now (0 paused), the game's own frame
    /// rate, and the flip limit VideoOut applies while FIFO presentation shows the generated frames
    /// (0 none).
    std::atomic<int> c_frame_gen_now{0};
    std::atomic<float> game_fps{0.0f};
    std::atomic<int> frame_gen_flip_limit{0};
    /// The refresh rate of the display the window is on (window.cpp), 0 until known.
    std::atomic<int> display_refresh_hz{0};

    /// Startup settings for the explicit PS_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    int startup_frame_gen = FrameGenOff;
    bool is_object_motion_at_startup = true;
    bool startup_effects[MaxEffects]{};
    int startup_model_lod = 0;
    OutputSize startup_output;
    int startup_live_resolution = 0;
};

Values& Get();
/// Localized overlay text; the Language selector itself stays in English.
const char* MenuText(const char* sz_english, const char* sz_russian, const char* sz_spanish);

/// Reads the file, then the environment overrides. Called once at start, after Game::Select (the
/// running title's effect switches).
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool is_fsr4_supported, bool is_fsr411_supported);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool is_available, const char* sz_problem);
/// After device creation: whether FSR 3.1 frame generation can run on it (`sz_result` says what
/// was found and must outlive the process); a frame_gen=fsr setting is turned off without it.
void ConfigureFrameGenSupport(bool is_fsr_supported, const char* sz_result);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();
/// The menu's frame generation choice: prints `Frame generation setting: <off|fsr>` on its own line
/// (launchers read it from the output to keep the choice per game).
void ChooseFrameGen(int frame_gen);
/// Before the menu's restart through run.sh, which inherits this process's environment: a
/// launcher's PS_FRAME_GEN would otherwise bring back the value the menu changed.
void PrepareRestart();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);
/// psport.ini value ("off", "fsr").
const char* FrameGenName(int frame_gen);

} // namespace PsSettings
