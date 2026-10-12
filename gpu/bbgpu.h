/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
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
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* BB_PAD_FILE's ps, cross and circle for the overlay's close prompt (Guide opens it): 1 when the
 * prompt took them, so cross and circle are not passed to the game. */
int bbgpu_overlay_pad_file_press(int guide, int cross, int circle);
/* Mouse look (runtime_pad.c): the window takes the mouse when it is clicked (relative mode) and
 * lets it go on F1, losing focus or the menu opening. take: motion (pixels) and wheel notches
 * since the last call, reset; captured: 1 while the window holds it; enable: mouse_look=0 in
 * bbport.ini keeps the window from taking it. */
void bbgpu_mouse_take(double *dx, double *dy, int *wheel_up, int *wheel_down);
int bbgpu_mouse_captured(void);
void bbgpu_mouse_look_enable(int enabled);
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
