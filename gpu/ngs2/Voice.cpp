// SPDX-License-Identifier: GPL-2.0-or-later
// The voice queries (spec 03 §3): state flags, state, owner, matrix and port information.
#include <cstring>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

Voice* VoiceOf(const HandleRef& ref) {
    return &ref.Owner<Rack>()->p_voices[ref.VoiceIndex()];
}

} // namespace

s32 PS4_SYSV_ABI sceNgs2VoiceGetStateFlags(OrbisNgs2Handle h_voice, u32* p_out_flags) {
    if (!p_out_flags) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid voice state address ({})",
                    static_cast<void*>(p_out_flags));
    }
    HandleRef ref{h_voice, HANDLE_VOICE};
    if (!ref) {
        *p_out_flags = 0;
        return InvalidHandleError(h_voice, HANDLE_VOICE);
    }
    *p_out_flags = VoiceOf(ref)->control.load(std::memory_order_relaxed) & VOICE_STATE_MASK;
    return ORBIS_OK;
}

// A voice that has not been rendered since its PLAY reads as {IN_USE, 0...}, an idle one as zeros, a
// playing one as its rack's process left it in the last render (spec 02 §5, 03 §3).
s32 PS4_SYSV_ABI sceNgs2VoiceGetState(OrbisNgs2Handle h_voice, void* p_out_state, std::size_t cb_state) {
    if (!p_out_state) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid voice state address ({})", p_out_state);
    }
    if (cb_state < sizeof(OrbisNgs2VoiceState)) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_OUT_SIZE, "Invalid voice state size ({})", cb_state);
    }
    HandleRef ref{h_voice, HANDLE_VOICE};
    if (!ref) {
        return InvalidHandleError(h_voice, HANDLE_VOICE);
    }
    Voice* p_voice = VoiceOf(ref);
    if (cb_state == sizeof(OrbisNgs2VoiceState)) {
        *static_cast<u32*>(p_out_state) = p_voice->control.load(std::memory_order_relaxed) & VOICE_STATE_MASK;
        return ORBIS_OK;
    }
    // A buffer shorter than the rack's state is left alone, and the call still succeeds.
    const u32 cb_voice_state = p_voice->p_rack->limits.cb_voice_state;
    if (cb_state < cb_voice_state) {
        return ORBIS_OK;
    }
    VoiceLock lock{p_voice};
    const u32 control = p_voice->control.load(std::memory_order_relaxed);
    std::memset(p_out_state, 0, cb_voice_state);
    if (control & VOICE_INITIAL_STATE) {
        *static_cast<u32*>(p_out_state) = VOICE_FLAG_IN_USE;
    } else if (control & VOICE_STATE_MASK) {
        std::memcpy(p_out_state, p_voice->p_state, cb_voice_state);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetOwner(OrbisNgs2Handle h_voice, OrbisNgs2Handle* p_out_rack, u32* p_out_index) {
    HandleRef ref{h_voice, HANDLE_VOICE};
    if (!ref) {
        if (p_out_rack) {
            *p_out_rack = 0;
        }
        if (p_out_index) {
            *p_out_index = 0;
        }
        return InvalidHandleError(h_voice, HANDLE_VOICE);
    }
    if (p_out_rack) {
        *p_out_rack = reinterpret_cast<OrbisNgs2Handle>(&ref.Owner<Rack>()->handle);
    }
    if (p_out_index) {
        *p_out_index = ref.VoiceIndex();
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetMatrixInfo(OrbisNgs2Handle h_voice, u32 matrix_id, OrbisNgs2VoiceMatrixInfo* p_out_info,
                                           std::size_t cb_info) {
    if (!p_out_info) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid voice matrix information address ({})",
                    static_cast<void*>(p_out_info));
    }
    if (cb_info < sizeof(OrbisNgs2VoiceMatrixInfo)) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_OUT_SIZE, "Invalid voice matrix information size ({})",
                    cb_info);
    }
    HandleRef ref{h_voice, HANDLE_VOICE};
    if (!ref) {
        return InvalidHandleError(h_voice, HANDLE_VOICE);
    }
    Voice* p_voice = VoiceOf(ref);
    if (matrix_id >= p_voice->p_rack->limits.max_matrices) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX, "Invalid voice matrix index ({})", matrix_id);
    }
    VoiceLock lock{p_voice};
    const VoiceMatrix& matrix = p_voice->p_matrices[matrix_id];
    p_out_info->numLevels = matrix.c_levels;
    std::memcpy(p_out_info->aLevel, matrix.levels, sizeof(p_out_info->aLevel));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetPortInfo(OrbisNgs2Handle h_voice, u32 i_port, OrbisNgs2VoicePortInfo* p_out_info,
                                         std::size_t cb_info) {
    if (!p_out_info) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid voice port information address ({})",
                    static_cast<void*>(p_out_info));
    }
    if (cb_info < sizeof(OrbisNgs2VoicePortInfo)) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_OUT_SIZE, "Invalid voice port information size ({})", cb_info);
    }
    HandleRef ref{h_voice, HANDLE_VOICE};
    if (!ref) {
        return InvalidHandleError(h_voice, HANDLE_VOICE);
    }
    Voice* p_voice = VoiceOf(ref);
    if (i_port >= p_voice->p_rack->limits.max_ports) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_PORT_INDEX, "Invalid voice port index ({})", i_port);
    }
    VoiceLock lock{p_voice};
    const VoicePort& port = p_voice->p_ports[i_port];
    if (!port.is_used) {
        *p_out_info = {-1, 1.0f, 0, 0, 0};
        return ORBIS_OK;
    }
    *p_out_info = {port.matrix_id, port.volume, port.delay_samples, port.i_dest_input, port.h_dest};
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
