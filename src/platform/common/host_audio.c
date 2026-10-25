/* ps_host_audio.h on SDL3 audio streams, the same on Linux and Windows. SDL's channel orders are the
 * contract's. */
#include "ps_host_audio.h"
#include <errno.h>
#include <SDL3/SDL.h>

static SDL_AudioStream *sdl_stream(PsHostAudioOut *p_out) { return (SDL_AudioStream *)p_out; }
static SDL_AudioSpec sdl_spec(PsHostAudioFormat format, int channels, int rate_hz) {
    return (SDL_AudioSpec){format == PS_HOST_AUDIO_F32 ? SDL_AUDIO_F32 : SDL_AUDIO_S16, channels, rate_hz};
}

int ps_host_audio_start(void) { return SDL_InitSubSystem(SDL_INIT_AUDIO) ? 0 : EIO; }
const char *ps_host_audio_driver(void) { return SDL_GetCurrentAudioDriver(); }
int ps_host_audio_device_channels(PsHostAudioOut *p_out, int *p_out_channels) {
    SDL_AudioSpec spec;
    const SDL_AudioDeviceID h_device = p_out ? SDL_GetAudioStreamDevice(sdl_stream(p_out)) : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!SDL_GetAudioDeviceFormat(h_device, &spec, NULL)) return EIO;
    *p_out_channels = spec.channels;
    return 0;
}

PsHostAudioOut *ps_host_audio_out_open(PsHostAudioFormat format, int channels, int rate_hz) {
    const SDL_AudioSpec spec = sdl_spec(format, channels, rate_hz);
    return (PsHostAudioOut *)SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
}
int ps_host_audio_out_set_format(PsHostAudioOut *p_out, PsHostAudioFormat format, int channels, int rate_hz) {
    const SDL_AudioSpec spec = sdl_spec(format, channels, rate_hz);
    return SDL_SetAudioStreamFormat(sdl_stream(p_out), &spec, NULL) ? 0 : EIO;
}
int ps_host_audio_out_resume(PsHostAudioOut *p_out) { return SDL_ResumeAudioStreamDevice(sdl_stream(p_out)) ? 0 : EIO; }
int ps_host_audio_out_submit(PsHostAudioOut *p_out, const void *p_data, int cb_data) {
    return SDL_PutAudioStreamData(sdl_stream(p_out), p_data, cb_data) ? 0 : EIO;
}
int ps_host_audio_out_queued(PsHostAudioOut *p_out) { return SDL_GetAudioStreamQueued(sdl_stream(p_out)); }
void ps_host_audio_out_close(PsHostAudioOut *p_out) { SDL_DestroyAudioStream(sdl_stream(p_out)); }
const char *ps_host_audio_error(void) { return SDL_GetError(); }
