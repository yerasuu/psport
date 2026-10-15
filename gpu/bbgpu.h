/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
    const char *app0;           /* game folder: sce_sys/icon0.png is the window icon */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* PS_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog, libSceIme). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* Ends the entry begin started, as cancelled, when the game closes it first (libSceIme). */
void bbgpu_text_input_end(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* PS_PAD_FILE's ps, cross and circle for the overlay's close prompt (Guide opens it): 1 when the
 * prompt took them, so cross and circle are not passed to the game. */
int bbgpu_overlay_pad_file_press(int guide, int cross, int circle);
/* Mouse look (runtime_pad.c): the window takes the mouse when it is clicked (relative mode) and
 * lets it go on F1, losing focus or the menu opening. take: motion (pixels) and wheel notches
 * since the last call, reset; captured: 1 while the window holds it; enable: mouse_look=0 in
 * bbport.ini keeps the window from taking it. */
void bbgpu_mouse_take(double *dx, double *dy, int *wheel_up, int *wheel_down);
int bbgpu_mouse_captured(void);
void bbgpu_mouse_look_enable(int enabled);
/* libSceMouse (runtime_mouse.c): the host mouse's buttons (SDL_BUTTON_MASK bits) and its motion
 * (pixels), wheel and tilt (notches) since the last call, reset; returns 0 while there is no window
 * or the host has no mouse. */
int bbgpu_game_mouse_take(int32_t *dx, int32_t *dy, int32_t *wheel, int32_t *tilt, uint32_t *buttons);
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
} BbKeyEvent;
void bbgpu_game_keyboard_open(int open);
int bbgpu_game_keyboard_take(BbKeyEvent *events, int max, int *keyboard, uint32_t *dropped);
/* The game as psport's launch found it, for its profile (gpu/games/game_profile.h), which is found
 * by TITLE_ID whatever PS_GAME_PROFILE says: what it adds to the launch is part of running the
 * game, not an optimisation of the translator. */
typedef struct {
    const char *sz_game_dir;   /* the folder loaded (the mods' view when mods are on) */
    const char *sz_serial;     /* param.sfo TITLE_ID and APP_VER, "" when absent */
    const char *sz_version;
    const char *sz_patch_file; /* the profile's own patch file, resolved; NULL without one */
    /* The SHA-256 of eboot.bin's loadable segments at their addresses, the same whatever tool
     * dumped the game; read from the folder on each call. */
    void (*pfn_eboot_sha256)(const char *sz_game_dir, unsigned char *p_out_digest);
} BbGameFiles;
/* The index-th TITLE_ID of the game's executable (patch files and mod folders named after any of
 * them apply); NULL past the last one or for a title without a profile. */
const char *bbgpu_game_serial(const char *serial, size_t index);
/* The profile's own patch file (shadPS4 XML), relative to psport's folder (the one holding out/ or
 * bin/); NULL without one. External patch folders leave it out. */
const char *bbgpu_game_patch_file(const char *serial);
/* Before anything of the game runs, before bbgpu_init: the environment defaults its profile
 * chooses (PS_VBLANK_HZ for its frame rate presets). */
void bbgpu_prepare_game(const BbGameFiles *game);
/* The profile's check of the game's files (PS_SKIP_GAME_CHECK=1 skips it): 0 when they can run,
 * else 1 with a short kind for launchers ("missing_update", ...) and what to do, in English. */
int bbgpu_check_game(const BbGameFiles *game, char *sz_out_kind, size_t cb_kind, char *sz_out_advice, size_t cb_advice);
/* Patch writes for the eboot at image offsets (the eboot's vaddr 0 at offset 0), owned by psport,
 * which applies them after relocation and before bbgpu_patch_image. */
typedef struct BbPatchList BbPatchList;
typedef struct {
    BbPatchList *p_list;
    /* Appends the writes of the named patches of a shadPS4/GoldHEN XML file for sz_app_version and
     * eboot.bin, in the order named; 0 with the reason in sz_out_error when one is missing, a line
     * is unsupported or writes outside the eboot (nothing is appended then). */
    int (*pfn_add_named)(BbPatchList *p_list, const char *sz_file, const char *const *pp_names, size_t c_names,
                         const char *sz_app_version, char *sz_out_error, size_t cb_error);
    size_t (*pfn_count)(const BbPatchList *p_list);
    /* The index-th write so far: its bytes, which may be rewritten in place, and where they go. */
    unsigned char *(*pfn_write)(BbPatchList *p_list, size_t i_write, uint64_t *p_out_offset, uint64_t *p_out_length);
} BbPatchServices;
/* 1 when the title's profile has its own frame rate and resolution presets (bbgpu_game_patches):
 * only such a title takes run.sh's scaled output path and its larger direct memory. */
int bbgpu_game_has_presets(const char *serial);
/* The profile's own patches of the executable, chosen by the player's settings, before the
 * external XML patches (which win over them): -1 when the title has none, 0 when the launch must
 * stop (the profile printed why), else 1. */
int bbgpu_game_patches(const BbGameFiles *game, const BbPatchServices *services);
/* Patches the loaded image before the game runs (image still writable): libGnm entry hooks. */
void bbgpu_patch_image(unsigned char *image, uint64_t size);
/* libSceAjm's MP3 and AAC decoders (runtime_ajm.c) over libSceAudiodec's FFmpeg codecs (audiodec/).
 * create returns NULL when FFmpeg cannot open the decoder. decode takes the frame at the start of data
 * and writes it to pcm as interleaved float: 0 decoded, 1 the frame is longer than size, 2 not a frame
 * or not decodable, 3 pcm too small. mp3_header is the frame header word (0 for AAC); encoder_delay and
 * total_samples are the gapless window an MP3 frame's LAME, VBRI or FGH header announces (0 without one). */
typedef struct {
    uint64_t consumed;
    uint32_t samples, channels, sample_rate, bitrate, mp3_header, high_efficiency, encoder_delay, total_samples;
} BbAudioFrame;
void *bbgpu_audio_decoder_create(int aac, int adts, uint32_t frequency_index, uint32_t channels);
int bbgpu_audio_decoder_decode(void *decoder, const uint8_t *data, uint64_t size, float *pcm, uint64_t pcm_size,
                               BbAudioFrame *frame);
void bbgpu_audio_decoder_reset(void *decoder);
void bbgpu_audio_decoder_destroy(void *decoder);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
