// SPDX-License-Identifier: GPL-2.0-or-later
// The mastering rack, 0x3000: its VoiceControl parameters (0x13930; spec 03 §5.4, kb #151, #395) and
// its processing (0x14280; spec 04 §2.5): input meter, the channel matrix for the pair of input and
// output channel classes, LFE low-pass, limiter, gain, output meter, and the game's render buffer.
#include <algorithm>
#include <cstring>

#include "Internal.h"
#include "core/libraries/ngs2/ngs2_mastering.h"

namespace Libraries::Ngs2 {
namespace {

enum MasteringParamId : u32 {
    MASTERING_SETUP = 0x30000000,
    MASTERING_MATRIX = 0x30000001,
    MASTERING_LFE = 0x30000002,
    MASTERING_LIMITER = 0x30000003,
    MASTERING_GAIN = 0x30000004,
    MASTERING_OUTPUT = 0x30000005,
    MASTERING_PEAK_METER = 0x30000006,
    MASTERING_READ_SETUP = 0x30008000,
    MASTERING_READ_MATRIX = 0x30008001,
    MASTERING_READ_LFE = 0x30008002,
    MASTERING_READ_LIMITER = 0x30008003,
    MASTERING_READ_GAIN = 0x30008004,
    MASTERING_READ_OUTPUT = 0x30008005,
    MASTERING_READ_PEAK_METER = 0x30008006,
};

constexpr u32 kClassChannels[] = {1, 2, 6, 8};
constexpr u32 kChannelClasses = 4;
constexpr u32 kLfeCutoffsHz[] = {80, 120, 240};
constexpr u32 kDefaultLfeCutoffHz = 120;
// The LFE low-pass runs on voices whose setup has more than 4 channels, whatever the buffer has.
constexpr u32 kMinLfeInputChannels = 5;

template <class Param>
Param* ParamOf(const OrbisNgs2VoiceParamHeader* p_header) {
    return reinterpret_cast<Param*>(const_cast<OrbisNgs2VoiceParamHeader*>(p_header));
}

u32 ParamSize(u32 id) {
    switch (id) {
    case MASTERING_MATRIX:
    case MASTERING_READ_MATRIX:
        return sizeof(OrbisNgs2MasteringVoiceMatrixParam);
    case MASTERING_SETUP:
    case MASTERING_LFE:
    case MASTERING_LIMITER:
    case MASTERING_GAIN:
    case MASTERING_OUTPUT:
    case MASTERING_PEAK_METER:
    case MASTERING_READ_SETUP:
    case MASTERING_READ_LFE:
    case MASTERING_READ_LIMITER:
    case MASTERING_READ_GAIN:
    case MASTERING_READ_OUTPUT:
    case MASTERING_READ_PEAK_METER:
        return 0x10;
    default:
        return 0;
    }
}

// What the setup puts back (0x13930; spec 03 §5.4, 09 §3.1, §4.1): default matrices, the LFE
// low-pass at 120 Hz, the limiter on at threshold 1.0 with slope 0, no knee, attack 1 ms, release
// 10 ms and unit output (kb #395 and #790 had knee and attack shifted by one field), and render
// buffer 0. Gain and meters wait for their own parameters.
void Setup(Voice* p_voice, u32 c_channels) {
    VoiceParams& params = *p_voice->p_params;
    params.c_in_channels = c_channels;
    params.c_out_channels = p_voice->p_rack->limits.max_channels;
    params.is_setup = true;
    params.set_modules = MODULE_LFE | MODULE_COMPRESSOR | MODULE_OUTPUT;
    ResetMasteringMatrices(p_voice->p_mastering_matrices);
    params.lfe = {1, kDefaultLfeCutoffHz};
    params.compressor = {0, 1.0f, 0.0f, 0.0f, 0.001f, 0.01f, 1.0f};
    params.i_output_buffer = 0;
    p_voice->control.fetch_or(VOICE_PENDING_SETUP, std::memory_order_relaxed);
}

s32 SetMatrix(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2MasteringVoiceMatrixParam* p_param) {
    if (p_param->type >= kMasteringMatrixTypes) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX, "Invalid mastering matrix type ({})",
                    p_param->type);
    }
    if (p_param->numLevels > kMatrixCells) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_MATRIX_LEVELS, "Invalid mastering matrix levels ({})",
                    p_param->numLevels);
    }
    if (p_param->numLevels > 0 && !p_param->aLevel) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_MATRIX_LEVEL_ADDRESS,
                    "Invalid mastering matrix level address ({})", static_cast<const void*>(p_param->aLevel));
    }
    const u32 c_in = kClassChannels[p_param->type / kChannelClasses];
    const u32 c_out = kClassChannels[p_param->type % kChannelClasses];
    MasteringMatrices& matrices = *p_voice->p_mastering_matrices;
    float* p_levels = matrices.levels[p_param->type];
    std::fill_n(p_levels, kMatrixCells, 0.0f);
    // Ordered by input channel, unlike the voice matrices (spec 03 §5.4).
    for (u32 i_in = 0; i_in < c_in; ++i_in) {
        for (u32 i_out = 0; i_out < c_out; ++i_out) {
            const u32 i_level = i_in * c_out + i_out;
            if (i_level < p_param->numLevels) {
                p_levels[i_in * kMaxChannels + i_out] = std::clamp(p_param->aLevel[i_level], -4.0f, 4.0f);
            }
        }
    }
    matrices.is_set[p_param->type] = true;
    return ORBIS_OK;
}

// The firmware writes the type's c_in * c_out levels whatever numLevels the game gave, an unset type
// reading as the identity (spec 09 §4.1, §13). It neither checks the type nor the array: a type past
// 15 keeps 0x804A0700 and a NULL array only gets the count, so that neither reads or writes outside
// the voice's matrices.
s32 ReadMatrix(OrbisNgs2Handle h_voice, const Voice& voice, OrbisNgs2MasteringVoiceMatrixParam* p_param) {
    if (p_param->type >= kMasteringMatrixTypes) {
        return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX, "Invalid mastering matrix type ({})",
                    p_param->type);
    }
    const u32 c_in = kClassChannels[p_param->type / kChannelClasses];
    const u32 c_out = kClassChannels[p_param->type % kChannelClasses];
    const MasteringMatrices& matrices = *voice.p_mastering_matrices;
    const bool is_set = matrices.is_set[p_param->type];
    if (p_param->aLevel) {
        auto* p_out = const_cast<float*>(p_param->aLevel);
        for (u32 i_in = 0; i_in < c_in; ++i_in) {
            for (u32 i_out = 0; i_out < c_out; ++i_out) {
                const float identity = i_in == i_out ? 1.0f : 0.0f;
                *p_out++ = is_set ? matrices.levels[p_param->type][i_in * kMaxChannels + i_out] : identity;
            }
        }
    }
    p_param->numLevels = c_in * c_out;
    return ORBIS_OK;
}

bool IsMixerChannelCount(u32 c_channels) {
    return c_channels == 1 || c_channels == 2 || c_channels == 6 || c_channels == 8;
}

} // namespace

s32 ControlMastering(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header) {
    const u32 cb_expected = ParamSize(p_header->id);
    if (cb_expected == 0) {
        return InvalidControlId(h_voice, p_header);
    }
    if (p_header->size != cb_expected) {
        return InvalidControlSize(h_voice, p_header, cb_expected);
    }
    VoiceParams& params = *p_voice->p_params;
    switch (p_header->id) {
    case MASTERING_SETUP: {
        const u32 c_channels = ParamOf<OrbisNgs2MasteringVoiceSetupParam>(p_header)->numInputChannels;
        if (!IsMixerChannelCount(c_channels) || c_channels > p_voice->p_rack->limits.max_channels) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS, "Invaild voice channels ({})", c_channels);
        }
        Setup(p_voice, c_channels);
        return ORBIS_OK;
    }
    case MASTERING_MATRIX:
        return SetMatrix(h_voice, p_voice, ParamOf<OrbisNgs2MasteringVoiceMatrixParam>(p_header));
    case MASTERING_LFE: {
        const auto* p_param = ParamOf<OrbisNgs2MasteringVoiceLfeParam>(p_header);
        if (std::find(std::begin(kLfeCutoffsHz), std::end(kLfeCutoffsHz), p_param->fc) == std::end(kLfeCutoffsHz)) {
            return Fail(h_voice, ORBIS_NGS2_ERROR_INVALID_LFE_FC, "Invalid LFE cut off frequency ({})", p_param->fc);
        }
        params.lfe = {p_param->enableFlag, p_param->fc};
        params.set_modules |= MODULE_LFE;
        return ORBIS_OK;
    }
    case MASTERING_LIMITER: {
        // Sub 1 of module 0x12 writes into the block sub 0 fills (kb #395).
        const auto* p_param = ParamOf<OrbisNgs2MasteringVoiceLimiterParam>(p_header);
        CompressorParams& limiter = params.compressor;
        limiter.flags = p_param->enableFlag ? limiter.flags & ~COMPRESSOR_DISABLED
                                            : limiter.flags | COMPRESSOR_DISABLED;
        limiter.threshold = std::clamp(p_param->threshold, 0.0f, 4.0f);
        params.set_modules |= MODULE_COMPRESSOR;
        return ORBIS_OK;
    }
    case MASTERING_GAIN: {
        const auto* p_param = ParamOf<OrbisNgs2MasteringVoiceGainParam>(p_header);
        params.gain_fbw = std::clamp(p_param->fbwLevel, 0.0f, 4.0f);
        params.gain_lfe = std::clamp(p_param->lfeLevel, 0.0f, 4.0f);
        params.set_modules |= MODULE_GAIN;
        return ORBIS_OK;
    }
    case MASTERING_OUTPUT:
        params.i_output_buffer = ParamOf<OrbisNgs2MasteringVoiceOutputParam>(p_header)->outputId;
        params.set_modules |= MODULE_OUTPUT;
        return ORBIS_OK;
    case MASTERING_PEAK_METER:
        return SetPeakMeter(h_voice, ParamOf<OrbisNgs2SamplerVoicePeakMeterParam>(p_header), &params);
    case MASTERING_READ_SETUP:
        ParamOf<OrbisNgs2MasteringVoiceSetupParam>(p_header)->numInputChannels = params.c_in_channels;
        return ORBIS_OK;
    case MASTERING_READ_MATRIX:
        return ReadMatrix(h_voice, *p_voice, ParamOf<OrbisNgs2MasteringVoiceMatrixParam>(p_header));
    case MASTERING_READ_LFE: {
        auto* p_param = ParamOf<OrbisNgs2MasteringVoiceLfeParam>(p_header);
        const bool is_set = params.set_modules & MODULE_LFE;
        p_param->enableFlag = is_set ? params.lfe.is_enabled : 0;
        p_param->fc = is_set ? params.lfe.cutoff_hz : 0;
        return ORBIS_OK;
    }
    case MASTERING_READ_LIMITER: {
        auto* p_param = ParamOf<OrbisNgs2MasteringVoiceLimiterParam>(p_header);
        const bool is_set = params.set_modules & MODULE_COMPRESSOR;
        p_param->enableFlag = is_set && !(params.compressor.flags & COMPRESSOR_DISABLED);
        p_param->threshold = is_set ? params.compressor.threshold : 0.0f;
        return ORBIS_OK;
    }
    case MASTERING_READ_GAIN: {
        auto* p_param = ParamOf<OrbisNgs2MasteringVoiceGainParam>(p_header);
        const bool is_set = params.set_modules & MODULE_GAIN;
        p_param->fbwLevel = is_set ? params.gain_fbw : 0.0f;
        p_param->lfeLevel = is_set ? params.gain_lfe : 0.0f;
        return ORBIS_OK;
    }
    case MASTERING_READ_OUTPUT:
        ParamOf<OrbisNgs2MasteringVoiceOutputParam>(p_header)->outputId =
            (params.set_modules & MODULE_OUTPUT) ? params.i_output_buffer : 0;
        return ORBIS_OK;
    case MASTERING_READ_PEAK_METER:
        ParamOf<OrbisNgs2MasteringVoicePeakMeterParam>(p_header)->enableFlag = ReadPeakMeter(params);
        return ORBIS_OK;
    default:
        return InvalidControlId(h_voice, p_header);
    }
}

void ProcessMasteringVoice(RenderContext& context, Voice* p_voice, u32* p_event_code) {
    const VoiceParams& params = *p_voice->p_active_params;
    const u32 c_in = p_voice->c_in_channels;
    const u32 c_samples = context.c_grain_samples;
    VoiceDsp& dsp = *p_voice->p_dsp;
    const bool is_paused = p_voice->flags & STATE_PAUSED;
    p_voice->flags = is_paused ? p_voice->flags & ~u32(STATE_PLAYING) : p_voice->flags | STATE_PLAYING;
    const Planes input = ReadInput(context, p_voice, 0);
    const Planes planes = WorkPlanes(context, WORK_VOICE);
    for (u32 i_channel = 0; i_channel < c_in; ++i_channel) {
        std::memcpy(planes.p_channel[i_channel], input.p_channel[i_channel], c_samples * sizeof(float));
    }
    const u32 set_modules = params.set_modules;
    const u32 first_modules = p_voice->first_active_modules;
    const bool has_meters = set_modules & MODULE_METER;
    if (!has_meters) {
        std::fill_n(dsp.peaks[0], kPeakValues, 0.0f);
        std::fill_n(dsp.peaks[1], kPeakValues, 0.0f);
    } else if (!is_paused) {
        ProcessPeakMeter(params.meter_mode, &p_voice->meters[0], planes, c_in, c_samples, first_modules & MODULE_METER,
                         dsp.peaks[0]);
    }
    // Without a buffer to write to, the output keeps the voice's own channels (0x14280, spec 09 §4.2).
    const u32 i_buffer = params.i_output_buffer;
    const bool has_buffer = (set_modules & MODULE_OUTPUT) && i_buffer < context.c_buffers;
    const u32 c_out = has_buffer ? context.p_buffers[i_buffer].numChannels : c_in;
    const Planes mixed = WorkPlanes(context, WORK_MATRIX);
    const MasteringMatrices& matrices = *p_voice->p_active_mastering_matrices;
    const u32 i_type = ChannelClass(c_in) * kChannelClasses + ChannelClass(c_out);
    if (matrices.is_set[i_type]) {
        float* p_previous = p_voice->p_previous_mastering_matrices->levels[i_type];
        if (p_voice->is_first_grain) {
            std::copy_n(matrices.levels[i_type], kMatrixCells, p_previous);
        }
        ProcessChannelMatrix(p_previous, matrices.levels[i_type], planes, c_in, mixed, c_out, c_samples);
        std::copy_n(matrices.levels[i_type], kMatrixCells, p_previous);
    } else {
        for (u32 i_channel = 0; i_channel < c_out; ++i_channel) {
            if (i_channel < c_in) {
                std::memcpy(mixed.p_channel[i_channel], planes.p_channel[i_channel], c_samples * sizeof(float));
            } else {
                std::memset(mixed.p_channel[i_channel], 0, c_samples * sizeof(float));
            }
        }
    }
    if ((set_modules & MODULE_LFE) && c_in >= kMinLfeInputChannels) {
        ProcessLfe(params.lfe, &dsp.lfe, mixed.p_channel[kLfeChannel], c_samples, context.sample_rate_hz);
    }
    if (set_modules & MODULE_COMPRESSOR) {
        ProcessCompressor(params.compressor, &dsp.compressor, mixed, mixed, c_out, c_samples, context.sample_rate_hz,
                          first_modules & MODULE_COMPRESSOR);
    }
    if (set_modules & MODULE_GAIN) {
        for (u32 i_channel = 0; i_channel < c_out; ++i_channel) {
            const float gain = i_channel == kLfeChannel ? params.gain_lfe : params.gain_fbw;
            const float gain_start = (first_modules & MODULE_GAIN) ? gain : dsp.gains[i_channel];
            RampWrite(mixed.p_channel[i_channel], mixed.p_channel[i_channel], c_samples, gain_start, gain);
            dsp.gains[i_channel] = gain;
        }
    }
    if (has_meters && !is_paused) {
        ProcessPeakMeter(params.meter_mode, &p_voice->meters[1], mixed, c_out, c_samples, first_modules & MODULE_METER,
                         dsp.peaks[1]);
    }
    ApplyEventCode(mixed, c_out, c_samples, *p_event_code);
    WriteRenderBuffer(context, i_buffer, mixed, c_out);
    OrbisNgs2MasteringVoiceState state{};
    state.voiceState.stateFlags = p_voice->flags;
    std::copy_n(dsp.peaks[0], kMaxChannels, state.aInputPeakHeight);
    if (!is_paused) {
        std::copy_n(dsp.peaks[1], kMaxChannels, state.aOutputPeakHeight);
    }
    std::memcpy(p_voice->p_state, &state, sizeof(state));
}

} // namespace Libraries::Ngs2
