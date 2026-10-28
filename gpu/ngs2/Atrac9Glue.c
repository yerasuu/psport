/* SPDX-License-Identifier: GPL-2.0-or-later */
/* LibAtrac9's decoder as the sampler uses it (phase F4, kb #1096). Its state lives in a rack's buffer
 * instead of the heap, so that a render never allocates; only C sees its layout, since its headers name
 * members after their types, which C++ refuses. A frame comes back as float planes read from the
 * decoder's own samples, before LibAtrac9 rounds them to 16 bits. */
#include <string.h>

#include "Atrac9Glue.h"
#include "decinit.h"
#include "decoder.h"
#include "structures.h"

/* LibAtrac9's own value (libatrac9.c). */
#define NGS2_ATRAC9_WLENGTH 16

size_t ngs2_atrac9_get_decoder_size(void) {
    return sizeof(Atrac9Handle);
}

/* NOTE: InitDecoder also rebuilds LibAtrac9's shared tables, writing the values they already hold, so
 * a decoder reading them on another thread meanwhile sees no change. */
int ngs2_atrac9_reset(void* p_decoder, const unsigned char* p_config) {
    unsigned char config[CONFIG_DATA_SIZE];
    memcpy(config, p_config, sizeof(config));
    memset(p_decoder, 0, sizeof(Atrac9Handle));
    return (int)InitDecoder((Atrac9Handle*)p_decoder, config, NGS2_ATRAC9_WLENGTH);
}

int ngs2_atrac9_decode_frame(void* p_decoder, const unsigned char* p_frame, int cb_frame, float* const* pp_out,
                             int* p_out_cb_used) {
    Atrac9Handle* p_handle = (Atrac9Handle*)p_decoder;
    short pcm[MAX_FRAME_SAMPLES * MAX_CHANNEL_COUNT];
    const At9Status status = Decode(p_handle, p_frame, cb_frame, (unsigned char*)pcm, p_out_cb_used);
    if (status != ERR_SUCCESS) {
        return (int)status;
    }
    for (int i_channel = 0; i_channel < p_handle->Config.ChannelCount; ++i_channel) {
        const double* p_samples = p_handle->Frame.Channels[i_channel]->Pcm;
        for (int i = 0; i < p_handle->Config.FrameSamples; ++i) {
            pp_out[i_channel][i] = (float)(p_samples[i] / 32768.0);
        }
    }
    return 0;
}
