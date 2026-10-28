// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2VoiceControl (spec 03 §1): walks the game's chain of parameters under the voice's lock,
// applies the parameters every rack shares, hands the rack's own to its dispatcher and stops at the
// first error, leaving the earlier ones applied. What a parameter changes only reaches the sound at
// the render's flush. Custom racks (F6) still refuse their own parameters.
#include <algorithm>
#include <cstring>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

enum VoiceParamId : u32 {
    PARAM_MATRIX_LEVELS = 1,
    PARAM_PORT_VOLUME = 2,
    PARAM_PORT_MATRIX = 3,
    PARAM_PORT_DELAY = 4,
    PARAM_PATCH = 5,
    PARAM_EVENT = 6,
    PARAM_CALLBACK = 7,
    PARAM_MODULE_READ = 0xc001,
};
constexpr u32 kParamReadFlag = 0x8000;
constexpr u32 kSamplerRackId = 0x1000;
constexpr u32 kSubmixerRackId = 0x2000;
constexpr u32 kReverbRackId = 0x2001;
constexpr u32 kMasteringRackId = 0x3000;

enum VoiceEvent : u32 {
    EVENT_PLAY = 0,
    EVENT_STOP = 1,
    EVENT_STOP_IMMEDIATE = 2,
    EVENT_KILL = 3,
    EVENT_PAUSE = 4,
    EVENT_RESUME = 5,
};

constexpr u32 kRackFlagCallbacks = 0x40000000;
constexpr u32 kMaxCallbackFlags = 3;
constexpr u32 kMaxPortDelaySamples = 0xffff;
// Port matrices 0x10 to 0x13 are the fixed ones the render provides (spec 04 §3.2).
constexpr s32 kFirstFixedMatrix = 0x10;
constexpr s32 kLastFixedMatrix = 0x13;

float ClampLevel(float level) {
    return std::clamp(level, -4.0f, 4.0f);
}

template <class Param>
const Param* ParamOf(const OrbisNgs2VoiceParamHeader* p_header) {
    return reinterpret_cast<const Param*>(p_header);
}

s32 CheckPort(OrbisNgs2Handle h_voice, const Voice* p_voice, u32 i_port) {
    if (i_port >= p_voice->p_rack->limits.max_ports) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_PORT_INDEX, "Invalid voice port index ({})", i_port);
    }
    return ORBIS_OK;
}

// A port's first touch sets it to no destination, no matrix and unit volume (spec 03 §1.2).
VoicePort& TouchPort(Voice* p_voice, u32 i_port) {
    VoicePort& port = p_voice->p_ports[i_port];
    if (!port.is_used) {
        port = {0, 0, 0, 0, 0, 1.0f, -1, true};
    }
    return port;
}

s32 SetMatrixLevels(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceMatrixLevelsParam* p_param) {
    if (p_param->matrixId >= p_voice->p_rack->limits.max_matrices) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX, "Invalid voice matrix ID ({})",
                    p_param->matrixId);
    }
    if (p_param->numLevels < 1 || p_param->numLevels > kMaxMatrixLevels) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_MATRIX_LEVELS, "Invalid voice matrix levels ({})",
                    p_param->numLevels);
    }
    if (!p_param->aLevel) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_MATRIX_LEVEL_ADDRESS, "Invalid voice matrix level address ({})",
                    static_cast<const void*>(p_param->aLevel));
    }
    VoiceMatrix& matrix = p_voice->p_matrices[p_param->matrixId];
    matrix = {};
    matrix.c_levels = p_param->numLevels;
    std::transform(p_param->aLevel, p_param->aLevel + p_param->numLevels, matrix.levels, ClampLevel);
    return ORBIS_OK;
}

s32 SetPortMatrix(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoicePortMatrixParam* p_param) {
    const s32 matrix_id = p_param->matrixId;
    // The firmware compares with '>', so matrix id maxMatrices passes too (spec 03 §1.2).
    const bool is_voice_matrix = matrix_id >= 0 && u32(matrix_id) <= p_voice->p_rack->limits.max_matrices;
    const bool is_fixed_matrix = matrix_id >= kFirstFixedMatrix && matrix_id <= kLastFixedMatrix;
    if (matrix_id != -1 && !is_voice_matrix && !is_fixed_matrix) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX, "Invalid voice matrix ID ({})", matrix_id);
    }
    TouchPort(p_voice, p_param->port).matrix_id = matrix_id;
    return ORBIS_OK;
}

s32 SetPatch(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoicePatchParam* p_param) {
    if (!p_param->destHandle) {
        VoicePort& port = TouchPort(p_voice, p_param->port);
        port.h_dest = 0;
        port.dest_rack_uid = 0;
        port.i_dest_voice = 0;
        port.i_dest_input = 0;
        return ORBIS_OK;
    }
    HandleRef dest_ref{p_param->destHandle, HANDLE_RACK | HANDLE_VOICE};
    if (!dest_ref) {
        return InvalidHandleError(p_param->destHandle, HANDLE_RACK | HANDLE_VOICE);
    }
    const Rack* p_dest_rack = dest_ref.Owner<Rack>();
    const System* p_system = p_voice->p_rack->p_system;
    const System* p_dest_system = p_dest_rack->p_system;
    if (!p_system || p_dest_system != p_system) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_PATCH, "Destination rack owned by different system ({}!={})",
                    static_cast<const void*>(p_dest_system), static_cast<const void*>(p_system));
    }
    if (p_param->destInputId >= p_dest_rack->limits.max_inputs) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_PATCH, "Invalid voice input ({})", p_param->destInputId);
    }
    VoicePort& port = TouchPort(p_voice, p_param->port);
    port.h_dest = p_param->destHandle;
    port.dest_rack_uid = p_dest_rack->uid;
    port.i_dest_voice = dest_ref.VoiceIndex();
    port.i_dest_input = p_param->destInputId;
    return ORBIS_OK;
}

bool TryReserve(std::atomic<u32>* p_c_used, u32 c_works, u32 c_max) {
    u32 c_used = p_c_used->load(std::memory_order_relaxed);
    do {
        if (c_used + c_works > c_max) {
            return false;
        }
    } while (!p_c_used->compare_exchange_weak(c_used, c_used + c_works, std::memory_order_relaxed));
    return true;
}

// An ATRAC9 setup's decoder (spec 10 §5): one of the rack's internal decoders and a channel work of
// maxAtrac9ChannelWorks per channel, or with setup flags bit 0 one of its AJM decoders. A pool of one
// decoder per voice gives each voice its own. False, with nothing reserved, when a pool is empty.
bool ReserveAtrac9Decoder(Voice* p_voice) {
    const SourceFormat& format = p_voice->p_params->format;
    p_voice->p_atrac9 = nullptr;
    if (format.waveform_type != WAVEFORM_ATRAC9) {
        return true;
    }
    Rack& rack = *p_voice->p_rack;
    const bool is_ajm = format.flags & 1;
    const u32 i_first = is_ajm ? rack.limits.max_atrac9_decoders : 0;
    const u32 c_pool = is_ajm ? rack.limits.max_ajm_atrac9_decoders : rack.limits.max_atrac9_decoders;
    const u32 i_voice = u32(p_voice - rack.p_voices);
    const u32 i_start = c_pool == rack.limits.max_voices ? i_voice : 0;
    for (u32 i = 0; i < c_pool && !p_voice->p_atrac9; ++i) {
        Atrac9Decoder& decoder = rack.p_atrac9_decoders[i_first + (i_start + i) % c_pool];
        Voice* p_owner = nullptr;
        if (decoder.p_owner.compare_exchange_strong(p_owner, p_voice, std::memory_order_acquire)) {
            p_voice->p_atrac9 = &decoder;
        }
    }
    if (!p_voice->p_atrac9) {
        return false;
    }
    if (!is_ajm) {
        if (!TryReserve(&rack.c_atrac9_channel_works_used, format.c_channels, rack.limits.max_atrac9_channel_works)) {
            ReleaseAtrac9Decoder(p_voice);
            p_voice->p_atrac9 = nullptr;
            return false;
        }
        p_voice->c_atrac9_channel_works = format.c_channels;
    }
    return true;
}

// PLAY trades the voice's channel works for max(input, output) channels of its setup (spec 03 §1.3):
// from the rack's pool, and for a rack with inputs from each input's pool too, then the decoder an
// ATRAC9 setup asks for. A voice that was never set up has 0 channels, so PLAY on it fails before it
// reserves anything (spec 09 §1); a failure later gives back what PLAY took.
s32 ReserveChannelWorks(OrbisNgs2Handle h_voice, Voice* p_voice) {
    const VoiceParams& params = *p_voice->p_params;
    const u32 c_works = std::max(params.c_in_channels, params.c_out_channels);
    if (c_works < 1 || c_works > kMaxChannels) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS, "Invaild voice channels ({})", c_works);
    }
    ReleaseChannelWorks(p_voice);
    Rack* p_rack = p_voice->p_rack;
    const u32 c_max = p_rack->limits.max_channel_works;
    if (!TryReserve(&p_rack->c_channel_works_used, c_works, c_max)) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_EMPTY_CHANNEL_WORK, "Empty channel work ({}+{}>{})",
                    p_rack->c_channel_works_used.load(), c_works, c_max);
    }
    for (u32 i_input = 0; i_input < p_rack->limits.max_inputs; ++i_input) {
        if (!TryReserve(&p_rack->c_input_works_used[i_input], c_works, c_max)) {
            for (u32 i_reserved = 0; i_reserved < i_input; ++i_reserved) {
                p_rack->c_input_works_used[i_reserved].fetch_sub(c_works, std::memory_order_relaxed);
            }
            p_rack->c_channel_works_used.fetch_sub(c_works, std::memory_order_relaxed);
            return Fail(h_voice, ORBIS_NGS2_ERROR_EMPTY_CODEC_DECODER, "Empty input channel work ({}:{}>{})",
                        i_input, c_works, c_max);
        }
    }
    p_voice->c_channel_works = c_works;
    if (!ReserveAtrac9Decoder(p_voice)) {
        ReleaseChannelWorks(p_voice);
        return Fail(h_voice, ORBIS_NGS2_ERROR_EMPTY_CODEC_DECODER, "Empty ATRAC9 decoder ({}, {} channels)",
                    p_voice->p_params->format.flags & 1 ? "AJM" : "internal", p_voice->p_params->format.c_channels);
    }
    return ORBIS_OK;
}

// Events wait in the control word for the render's flush (spec 03 §1.3); STOP to RESUME only count on
// a voice in use and are dropped without error otherwise. KILL locks the voice's data before it
// returns, in use or not, because the game may free the waveform right after it.
s32 PostEvent(OrbisNgs2Handle h_voice, Voice* p_voice, u32 event) {
    if (event == EVENT_KILL) {
        LockVoiceData(p_voice);
    }
    u32 control = p_voice->control.load(std::memory_order_relaxed);
    switch (event) {
    case EVENT_PLAY: {
        if (const s32 result = ReserveChannelWorks(h_voice, p_voice); result < 0) {
            return result;
        }
        control = (control & 0xdf000000u) | VOICE_INITIAL_STATE | VOICE_PENDING_START | VOICE_FLAG_IN_USE;
        break;
    }
    case EVENT_STOP:
    case EVENT_STOP_IMMEDIATE:
    case EVENT_KILL:
    case EVENT_PAUSE:
    case EVENT_RESUME:
        if (control & VOICE_FLAG_IN_USE) {
            // STOP_IMM and KILL hand the channel works back at once; the voice still fades out in
            // its next grain. KILL waits for the render to leave the voice, which the voice lock does.
            if (event == EVENT_STOP_IMMEDIATE || event == EVENT_KILL) {
                ReleaseChannelWorks(p_voice);
            }
            constexpr u32 kEventBits[] = {0, VOICE_PENDING_STOP, VOICE_PENDING_STOP_IMMEDIATE, VOICE_PENDING_KILL,
                                          VOICE_PENDING_PAUSE, VOICE_PENDING_RESUME};
            if (event == EVENT_PAUSE) {
                control &= ~u32(VOICE_PENDING_RESUME);
            } else if (event == EVENT_RESUME) {
                control &= ~u32(VOICE_PENDING_PAUSE);
            }
            control |= kEventBits[event];
        }
        break;
    default:
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_EVENT_TYPE, "Invalid voice event ({})", event);
    }
    p_voice->control.store(control, std::memory_order_relaxed);
    return ORBIS_OK;
}

s32 SetCallback(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceCallbackParam* p_param) {
    if (!(p_voice->p_rack->limits.flags & kRackFlagCallbacks)) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_UNABLE_CALLBACK, "The rack unable to set the callback ({:#x})",
                    reinterpret_cast<OrbisNgs2Handle>(&p_voice->p_rack->handle));
    }
    if (p_param->flags > kMaxCallbackFlags) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_CALLBACK_FLAG, "Invalid voice callback flag ({:X})",
                    p_param->flags);
    }
    if (p_param->flags != 0 && !p_param->callbackHandler) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_CALLBACK_HANDLER, "Invalid voice callback handler ({})",
                    reinterpret_cast<const void*>(p_param->callbackHandler));
    }
    p_voice->pfn_callback = p_param->callbackHandler;
    p_voice->callback_data = p_param->callbackData;
    p_voice->callback_flags = p_param->flags;
    return ORBIS_OK;
}

u32 CommonParamSize(u32 id) {
    switch (id) {
    case PARAM_MATRIX_LEVELS:
        return sizeof(OrbisNgs2VoiceMatrixLevelsParam);
    case PARAM_PORT_VOLUME:
        return sizeof(OrbisNgs2VoicePortVolumeParam);
    case PARAM_PORT_MATRIX:
        return sizeof(OrbisNgs2VoicePortMatrixParam);
    case PARAM_PORT_DELAY:
        return sizeof(OrbisNgs2VoicePortDelayParam);
    case PARAM_PATCH:
        return sizeof(OrbisNgs2VoicePatchParam);
    case PARAM_EVENT:
        return sizeof(OrbisNgs2VoiceEventParam);
    case PARAM_CALLBACK:
        return sizeof(OrbisNgs2VoiceCallbackParam);
    case PARAM_MODULE_READ:
        return 0x20;
    default:
        return 0;
    }
}

s32 ApplyParam(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header) {
    const u32 cb_expected = CommonParamSize(p_header->id);
    if (cb_expected == 0) {
        const RackKind& kind = *p_voice->p_rack->p_kind;
        if (IsRackParameter(kind, p_header->id)) {
            switch (kind.rack_id) {
            case kSamplerRackId:
                return ControlSampler(h_voice, p_voice, p_header);
            case kSubmixerRackId:
                return ControlSubmixer(h_voice, p_voice, p_header);
            case kReverbRackId:
                return ControlReverb(h_voice, p_voice, p_header);
            case kMasteringRackId:
                return ControlMastering(h_voice, p_voice, p_header);
            default:
                break;
            }
            static std::uint64_t s_c_uses;
            NoteUnimplemented(&s_c_uses, "sceNgs2VoiceControl: custom rack parameters (F6)");
            return Fail(h_voice, ORBIS_NGS2_ERROR_FAIL, "Voice control ID ({:#x}) not implemented yet",
                        p_header->id);
        }
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID, "Invalid voice control ID ({})",
                    p_header->id);
    }
    if (p_header->size != cb_expected) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE, "Invalid voice parameter size ({}!={})",
                    p_header->size, cb_expected);
    }
    switch (p_header->id) {
    case PARAM_MATRIX_LEVELS:
        return SetMatrixLevels(h_voice, p_voice, ParamOf<OrbisNgs2VoiceMatrixLevelsParam>(p_header));
    case PARAM_PORT_VOLUME: {
        const auto* p_param = ParamOf<OrbisNgs2VoicePortVolumeParam>(p_header);
        if (const s32 result = CheckPort(h_voice, p_voice, p_param->port); result < 0) {
            return result;
        }
        TouchPort(p_voice, p_param->port).volume = ClampLevel(p_param->level);
        return ORBIS_OK;
    }
    case PARAM_PORT_MATRIX: {
        const auto* p_param = ParamOf<OrbisNgs2VoicePortMatrixParam>(p_header);
        if (const s32 result = CheckPort(h_voice, p_voice, p_param->port); result < 0) {
            return result;
        }
        return SetPortMatrix(h_voice, p_voice, p_param);
    }
    case PARAM_PORT_DELAY: {
        const auto* p_param = ParamOf<OrbisNgs2VoicePortDelayParam>(p_header);
        if (const s32 result = CheckPort(h_voice, p_voice, p_param->port); result < 0) {
            return result;
        }
        TouchPort(p_voice, p_param->port).delay_samples = std::min(p_param->numSamples, kMaxPortDelaySamples);
        return ORBIS_OK;
    }
    case PARAM_PATCH: {
        const auto* p_param = ParamOf<OrbisNgs2VoicePatchParam>(p_header);
        if (const s32 result = CheckPort(h_voice, p_voice, p_param->port); result < 0) {
            return result;
        }
        return SetPatch(h_voice, p_voice, p_param);
    }
    case PARAM_EVENT:
        return PostEvent(h_voice, p_voice, ParamOf<OrbisNgs2VoiceEventParam>(p_header)->eventId);
    case PARAM_CALLBACK:
        return SetCallback(h_voice, p_voice, ParamOf<OrbisNgs2VoiceCallbackParam>(p_header));
    case PARAM_MODULE_READ:
        ReadModuleBlock(*p_voice, const_cast<OrbisNgs2VoiceParamHeader*>(p_header));
        return ORBIS_OK;
    default:
        return InvalidControlId(h_voice, p_header);
    }
}

const OrbisNgs2VoiceParamHeader* NextParam(const OrbisNgs2VoiceParamHeader* p_header) {
    if (!p_header || p_header->next == 0) {
        return nullptr;
    }
    return reinterpret_cast<const OrbisNgs2VoiceParamHeader*>(reinterpret_cast<const u8*>(p_header) +
                                                               p_header->next);
}

} // namespace

s32 PS4_SYSV_ABI sceNgs2VoiceControl(OrbisNgs2Handle h_voice, const OrbisNgs2VoiceParamHeader* p_param) {
    if (!p_param) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ADDRESS, "Invalid voice control parameter ({})",
                    static_cast<const void*>(p_param));
    }
    HandleRef ref{h_voice, HANDLE_VOICE};
    if (!ref) {
        return InvalidHandleError(h_voice, HANDLE_VOICE);
    }
    Voice* p_voice = &ref.Owner<Rack>()->p_voices[ref.VoiceIndex()];
    VoiceLock lock{p_voice};
    // Floyd's two pointers, as the firmware walks the chain: the fast one meets the slow one only on a
    // cycle.
    const OrbisNgs2VoiceParamHeader* p_fast = p_param;
    for (const OrbisNgs2VoiceParamHeader* p_slow = p_param; p_slow;) {
        if (const s32 result = ApplyParam(h_voice, p_voice, p_slow); result < 0) {
            return result;
        }
        if (!(p_slow->id & kParamReadFlag)) {
            p_voice->is_dirty = true;
        }
        p_slow = NextParam(p_slow);
        p_fast = NextParam(NextParam(p_fast));
        if (p_slow && p_fast == p_slow) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_DETECTED_CIRCULAR_VOICE_CONTROL, "Detected circular voice control");
        }
    }
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
