/* Audio output to the host (decision #1009, phase 1b): libSceAudioOut's ports as streams to the host's
 * default playback device. A stream takes interleaved frames at a fixed format, channel count and rate,
 * queues what it is given and plays it on the host's own audio thread; libSceAudioOut paces and steers
 * the queue level itself (runtime_audio.c). Channels are in the order FL FR FC LFE BL BR SL SR for 7.1,
 * and FL FR LFE (2.1), FL FR BL BR (quad), FL FR LFE BL BR (4.1), FL FR FC LFE BL BR (5.1) and FL FR FC
 * LFE BC SL SR (6.1) for fewer. Errors are the host's errno values, with the backend's reason for the
 * last one in ps_host_audio_error; host_features.inc says what each OS has (audio_out). One shared
 * backend serves every OS that has it (src/platform/common/). */
#ifndef PS_HOST_AUDIO_H
#define PS_HOST_AUDIO_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsHostAudioOut PsHostAudioOut;
typedef enum {
    PS_HOST_AUDIO_S16,
    PS_HOST_AUDIO_F32,
} PsHostAudioFormat;

/* Starts the host's audio support: 0, or EIO when there is no audio device to play on. */
int ps_host_audio_start(void);
/* The name of the host's audio driver, for the log; NULL before a successful ps_host_audio_start. */
const char *ps_host_audio_driver(void);
/* The channel count of the device p_out plays on, or of the default playback device when p_out is
 * NULL, in *p_out_channels: 0, or EIO. */
int ps_host_audio_device_channels(PsHostAudioOut *p_out, int *p_out_channels);

/* A stream to the default playback device, paused: NULL with the host's reason in ps_host_audio_error. */
PsHostAudioOut *ps_host_audio_out_open(PsHostAudioFormat format, int channels, int rate_hz);
/* Changes the frames the stream takes from now on, which the host converts to the device's: 0, or EIO
 * with the stream unchanged. */
int ps_host_audio_out_set_format(PsHostAudioOut *p_out, PsHostAudioFormat format, int channels, int rate_hz);
/* Starts the device playing what the stream queues: 0, or EIO. */
int ps_host_audio_out_resume(PsHostAudioOut *p_out);
/* Queues cb_data bytes of whole frames: 0, or EIO. */
int ps_host_audio_out_submit(PsHostAudioOut *p_out, const void *p_data, int cb_data);
/* The bytes queued and not yet played, or -1 when the host cannot tell. */
int ps_host_audio_out_queued(PsHostAudioOut *p_out);
/* Stops and frees the stream; waits for the host's audio thread, so not to be called under a lock that
 * thread or a game's audio thread needs soon. */
void ps_host_audio_out_close(PsHostAudioOut *p_out);
/* The backend's reason for the last failure on the calling thread. */
const char *ps_host_audio_error(void);

#ifdef __cplusplus
}
#endif
#endif
