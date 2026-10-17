/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef PSPORT_GPU_H
#define PSPORT_GPU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *sz_title;          /* window title */
    const char *sz_serial;         /* CUSA id, names the pipeline cache */
    const char *sz_user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
    const char *sz_app0;           /* game folder: sce_sys/icon0.png is the window icon */
} PsGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void psport_gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int psport_gpu_init(const PsGpuConfig *p_config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t psport_gpu_resolve(const char *sz_scoped_nid);
struct PsHostFaultContext;
/* Called first by the loader's access fault handler: 1 when a GPU page-tracking fault was handled. */
int psport_gpu_handle_fault(struct PsHostFaultContext *p_context, void *p_address);
/* PS_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void psport_gpu_dump_guest_writes(const struct PsHostFaultContext *p_context);
/* Before the process ends through _exit or exec: waits up to 2 s for the pipeline cache writer to
 * finish what is queued and logs what was flushed or dropped. can_wait=0 (signal handlers, failed
 * assertions) takes no lock and only reports the blobs still queued. */
void psport_gpu_flush_caches(int can_wait);
/* Keyboard text entry through the game window (IME dialog, libSceIme). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int psport_gpu_text_input_begin(const char *sz_initial_utf8, const char *sz_prompt_utf8);
int psport_gpu_text_input_poll(char *sz_out_utf8, uint64_t cb_out);
/* Ends the entry begin started, as cancelled, when the game closes it first (libSceIme). */
void psport_gpu_text_input_end(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int psport_gpu_overlay_captures_input(void);
/* PS_PAD_FILE's ps, cross and circle for the overlay's close prompt (Guide opens it): 1 when the
 * prompt took them, so cross and circle are not passed to the game. */
int psport_gpu_overlay_pad_file_press(int guide, int cross, int circle);
/* Mouse look (runtime_pad.c): the window takes the mouse when it is clicked (relative mode) and
 * lets it go on F1, losing focus or the menu opening. take: motion (pixels) and wheel notches
 * since the last call, reset; captured: 1 while the window holds it; enable: mouse_look=0 in
 * psport.ini keeps the window from taking it. */
void psport_gpu_mouse_take(double *p_dx, double *p_dy, int *p_wheel_up, int *p_wheel_down);
int psport_gpu_mouse_captured(void);
void psport_gpu_mouse_look_enable(int is_enabled);
/* Asks the window to take the mouse at its next poll, as a click in the game does (motion
 * emulation's spray can: the mouse aims); not while a menu or a text entry wants the cursor. */
void psport_gpu_mouse_capture(void);
/* Native mouse look (Game::MouseHooks pfn_camera_look): turns the game's camera by pitch and yaw
 * (radians) at its next update; 0 without such a hook or while the camera does not take turns
 * (lock-on, menus): drive the right stick then. */
int psport_gpu_camera_look(float pitch, float yaw);
/* The game profile's changes to the pad state the game reads (Game::MouseHooks pfn_filter_pad):
 * the buttons (PS4 bits); the sticks (lx ly rx ry) as they are. */
void psport_gpu_filter_pad(uint32_t *p_buttons, const uint8_t *p_sticks);
/* A game module's look at every window event first (an SDL_Event and the SDL_Window, on the window
 * thread), before the profile's (Game::MouseHooks pfn_window_event): nonzero takes the event, and
 * neither the port nor the game sees it. Up to 4, never removed; 0 when all are taken. */
typedef int (*PsGpuEventFilter)(const void *p_sdl_event, void *p_sdl_window);
int psport_gpu_add_event_filter(PsGpuEventFilter pfn_filter);
/* A game module's line of text over the game, near the bottom, until called with NULL (any
 * thread): what it shows while the game's own picture is black (a co-op extension's loads). */
void psport_gpu_overlay_banner(const char *sz_utf8);
/* libSceMouse (runtime_mouse.c): the host mouse's buttons (SDL_BUTTON_MASK bits) and its motion
 * (pixels), wheel and tilt (notches) since the last call, reset; returns 0 while there is no window
 * or the host has no mouse. */
int psport_gpu_game_mouse_take(int32_t *p_dx, int32_t *p_dy, int32_t *p_wheel, int32_t *p_tilt, uint32_t *p_buttons);
/* libSceImeKeyboard (runtime_keyboard.c): open=1 while the game has the keyboard open; the window
 * then queues for the game the keys the port does not keep (Insert, Home, the menu, F1 while it
 * holds the mouse). take: up to max queued events, oldest first (max 0 takes none); *keyboard is 0
 * while there is no window or the host has no keyboard; *dropped counts the events lost to a full
 * queue. character is the key's Unicode character with the held modifiers, 0 for none. */
typedef struct {
    uint64_t time_us; /* CLOCK_REALTIME */
    uint32_t character;
    uint16_t scancode, mod; /* SDL_Scancode (USB HID usage), SDL_Keymod */
    uint8_t down, repeat;
} PsKeyEvent;
void psport_gpu_game_keyboard_open(int is_open);
int psport_gpu_game_keyboard_take(PsKeyEvent *p_events, int max, int *p_keyboard, uint32_t *p_dropped);
/* The game as psport's launch found it, for its title (gpu/games/game_profile.h). */
typedef struct {
    const char *sz_game_dir;   /* the folder loaded (the mods' view when mods are on) */
    const char *sz_serial;     /* param.sfo TITLE_ID and APP_VER, "" when absent */
    const char *sz_version;
    const char *sz_patch_file; /* the title's own patch file, resolved; NULL without one */
    /* The SHA-256 of eboot.bin's loadable segments at their addresses, the same whatever tool
     * dumped the game; read from the folder on each call. */
    void (*pfn_eboot_sha256)(const char *sz_game_dir, unsigned char *p_out_digest);
} PsGameFiles;
/* Patch writes for the eboot at image offsets (the eboot's vaddr 0 at offset 0), owned by psport,
 * which applies them after relocation and before PsTitleInfo's pfn_patch_image. */
typedef struct PsPatchList PsPatchList;
typedef struct {
    PsPatchList *p_list;
    /* Appends the writes of the named patches of a shadPS4/GoldHEN XML file for sz_app_version and
     * eboot.bin, in the order named; 0 with the reason in sz_out_error when one is missing, a line
     * is unsupported or writes outside the eboot (nothing is appended then). */
    int (*pfn_add_named)(PsPatchList *p_list, const char *sz_file, const char *const *pp_names, size_t c_names,
                         const char *sz_app_version, char *sz_out_error, size_t cb_error);
    size_t (*pfn_count)(const PsPatchList *p_list);
    /* The index-th write so far: its bytes, which may be rewritten in place, and where they go. */
    unsigned char *(*pfn_write)(PsPatchList *p_list, size_t i_write, uint64_t *p_out_offset, uint64_t *p_out_length);
} PsPatchServices;
/* A title's own part of the online module (Game::OnlineExtension): all NULL for none. */
typedef struct {
    const char *sz_name;
    const char *sz_switch;
    const char *sz_local_server;
    const char *sz_local_server_args;
    const char *sz_local_switch;
    const char *sz_status_path;
} PsTitleOnlineExtension;
/* The servers of a title's online play (Game::OnlineServices): NULL and 0 for a title without any. */
typedef struct {
    const char *sz_game_server;
    const char *sz_community_server;
    uint16_t community_api_port;
    const char *sz_summon_route;
    PsTitleOnlineExtension extension;
} PsTitleOnline;
/* What the launch takes from the game's title (Game::Title), found by TITLE_ID whatever
 * PS_GAME_PROFILE says: it is part of running the game, not an optimisation of the translator.
 * All zero for a title psport has none for; each pointer may be NULL. */
typedef struct {
    const char *sz_name;
    /* The TITLE_IDs of the game's executable (patch files and mod folders named after any of them
     * apply). */
    const char *const *pp_serials;
    size_t c_serials;
    /* Folders a loose-file mod may consist of without dvdroot_ps4 around them (the game's own are
     * added by src/loader/loader_mods.c). */
    const char *const *pp_mod_folders;
    size_t c_mod_folders;
    /* Its own patch file (shadPS4 XML), relative to psport's folder (the one holding out/ or bin/).
     * External patch folders leave it out. */
    const char *sz_patch_file;
    /* 1 when it has its own frame rate and resolution presets (pfn_add_patches): only such a title
     * takes run.sh's scaled output path and its larger direct memory. */
    int has_presets;
    /* Before anything of the game runs, before psport_gpu_init: the environment defaults it
     * chooses (PS_VBLANK_HZ for its frame rate presets). */
    void (*pfn_prepare_environment)(void);
    /* Its check of the game's files (PS_SKIP_GAME_CHECK=1 skips it): 0 when they can run, else 1
     * with a short kind for launchers ("missing_update", ...) and what to do, in English. */
    int (*pfn_check_files)(const PsGameFiles *p_game, char *sz_out_kind, size_t cb_kind, char *sz_out_advice,
                           size_t cb_advice);
    /* Its own patches of the executable, chosen by the player's settings, before the external XML
     * patches (which win over them): 0 when the launch must stop (it printed why), else 1. */
    int (*pfn_add_patches)(const PsGameFiles *p_game, const PsPatchServices *p_services);
    /* Patches the loaded image before the game runs (image still writable), after psport_gpu_init
     * chose the running title: its translator profile's patches unless PS_GAME_PROFILE=none, then
     * its own. */
    void (*pfn_patch_image)(unsigned char *p_image, uint64_t cb_image);
    /* Its quirks: RuntimeQuirkId values of src/runtime/runtime_quirks.inc. */
    const uint16_t *p_quirks;
    size_t c_quirks;
    PsTitleOnline online;
} PsTitleInfo;
PsTitleInfo psport_gpu_title(const char *sz_serial);
/* The running title's default controls (Game::Title sz_default_controls: psport.ini key.<input>= and
 * pad.<input>= lines, one per line, over the neutral layout of src/input/runtime_pad.c), or NULL. */
const char *psport_gpu_title_controls(void);
/* A text in the overlay's languages (Game::LocalizedText); NULL where it has none. */
typedef struct {
    const char *sz_english, *sz_russian, *sz_spanish;
} PsLocalizedText;
/* A switch of a title's own patches (Game::EffectSwitch): a psport.ini key, 0 or 1, read at start;
 * is_default_on is the game's unpatched behaviour. */
typedef struct {
    const char *sz_key;
    int is_default_on;
    PsLocalizedText label, hint;
} PsEffectSwitch;
/* The overlay's "Game effects" section of a title (Game::GameEffects), for launchers: model_lod
 * first when has_model_detail, then c_switches switches in the overlay's order, then the section's
 * hint. All zero for a title without the section. */
typedef struct {
    size_t c_switches;
    int has_model_detail;
    PsLocalizedText hint;
} PsTitleEffects;
PsTitleEffects psport_gpu_title_effects(const char *sz_serial);
/* The i_switch-th switch of psport_gpu_title_effects: 0 past c_switches, *p_out_switch untouched. */
int psport_gpu_title_effect_switch(const char *sz_serial, size_t i_switch, PsEffectSwitch *p_out_switch);
/* libSceAjm's MP3 and AAC decoders (runtime_ajm.c) over libSceAudiodec's FFmpeg codecs (audiodec/).
 * create returns NULL when FFmpeg cannot open the decoder. decode takes the frame at the start of data
 * and writes it to pcm as interleaved float: 0 decoded, 1 the frame is longer than size, 2 not a frame
 * or not decodable, 3 pcm too small. mp3_header is the frame header word (0 for AAC); encoder_delay and
 * total_samples are the gapless window an MP3 frame's LAME, VBRI or FGH header announces (0 without one). */
typedef struct {
    uint64_t consumed;
    uint32_t samples, channels, sample_rate, bitrate, mp3_header, high_efficiency, encoder_delay, total_samples;
} PsAudioFrame;
void *psport_gpu_audio_decoder_create(int aac, int adts, uint32_t i_frequency, uint32_t channels);
int psport_gpu_audio_decoder_decode(void *p_decoder, const uint8_t *p_data, uint64_t cb_data, float *p_pcm, uint64_t cb_pcm,
                               PsAudioFrame *p_frame);
void psport_gpu_audio_decoder_reset(void *p_decoder);
void psport_gpu_audio_decoder_destroy(void *p_decoder);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned psport_gpu_symbol_count(void);
/* PS_CP_CENSUS=1: the command stream census, printed once after the call report (async-signal
 * tolerant: write(2) only). */
void psport_gpu_cp_census_report(void);
/* PS_NATIVE_PROBE=1: the parallel recording probe's summary and decision, printed once after the
 * census (write(2) only, like it). */
void psport_gpu_native_probe_report(void);
#ifdef __cplusplus
}
#endif
#endif
