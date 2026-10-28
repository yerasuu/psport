/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The native Ngs2's way into LibAtrac9 (third_party/LibAtrac9, MIT; Atrac9Glue.c). */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bytes one decoder's state takes, which a sampler rack lays out in its buffer. */
size_t ngs2_atrac9_get_decoder_size(void);
/* Starts p_decoder afresh, overlap and band history cleared, for the 4-byte ATRAC9 header p_config;
 * 0, or LibAtrac9's error. */
int ngs2_atrac9_reset(void* p_decoder, const unsigned char* p_config);
/* Decodes the frame at p_frame into one plane of floats per channel (1.0 = 32768), each holding the
 * config's frame samples; *p_out_cb_used gets the bytes it took. 0, or LibAtrac9's error. */
int ngs2_atrac9_decode_frame(void* p_decoder, const unsigned char* p_frame, int cb_frame, float* const* pp_out,
                             int* p_out_cb_used);

#ifdef __cplusplus
}
#endif
