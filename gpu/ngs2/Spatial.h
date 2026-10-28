// SPDX-License-Identifier: GPL-2.0-or-later
// Pan, Geom and FFT (phase F7, kb #1099; rea spec 12): pure math over the game's own structures, with
// no system, rack or voice behind them.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_geom.h"
#include "core/libraries/ngs2/ngs2_pan.h"

namespace Libraries::Ngs2 {

// Where firmware 11.00 would hang, fault or overrun a buffer, psport answers instead; each such
// departure is counted in the call report and logged once.
void NoteDeparture(std::uint64_t* p_c_uses, const char* sz_what);

s32 PS4_SYSV_ABI sceNgs2PanInit(OrbisNgs2PanWork* p_work, const float* p_speaker_angles, u32 c_speakers,
                                float unit_angle);
s32 PS4_SYSV_ABI sceNgs2PanGetVolumeMatrix(const OrbisNgs2PanWork* p_work, const OrbisNgs2PanParam* p_params,
                                           u32 c_params, u32 matrix_format, float* p_out_levels);

s32 PS4_SYSV_ABI sceNgs2GeomResetListenerParam(OrbisNgs2GeomListenerParam* p_out_param);
s32 PS4_SYSV_ABI sceNgs2GeomResetSourceParam(OrbisNgs2GeomSourceParam* p_out_param);
s32 PS4_SYSV_ABI sceNgs2GeomCalcListener(const OrbisNgs2GeomListenerParam* p_param,
                                         OrbisNgs2GeomListenerWork* p_out_work, u32 coordinate);
s32 PS4_SYSV_ABI sceNgs2GeomApply(const OrbisNgs2GeomListenerWork* p_listener, const OrbisNgs2GeomSourceParam* p_source,
                                  OrbisNgs2GeomAttribute* p_out_attribute, u32 flags);

// The FFT work is opaque to the game: it lives in the buffer the game sizes with FftQuerySize and
// hands to FftInit, whose address is the handle.
s32 PS4_SYSV_ABI sceNgs2FftQuerySize(u32 log2_points, u32 flags, std::size_t* p_out_cb_work);
s32 PS4_SYSV_ABI sceNgs2FftInit(void* p_buffer, std::size_t cb_buffer, u32 log2_points, u32 flags,
                                void** pp_out_work);
s32 PS4_SYSV_ABI sceNgs2FftProcess(void* p_work, const float* p_in, float* p_out, float* p_scratch, u64 unused,
                                   u32 flags);

} // namespace Libraries::Ngs2
