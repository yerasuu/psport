// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2VoiceControl's module read 0xc001 (0x87f0, spec 09 §12): a copy of one module's pending
// parameter block in the firmware's own layout, not the SDK's structure, as memcpy_s(dst, dstSize, src,
// dstSize) with src the block of a module set since the last setup and NULL otherwise.
#include <algorithm>
#include <cstring>

#include "Custom.h"
#include "Internal.h"
#include "Reverb.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kSamplerRackId = 0x1000;
constexpr u32 kSubmixerRackId = 0x2000;
constexpr u32 kReverbRackId = 0x2001;
constexpr u32 kMasteringRackId = 0x3000;
// The largest block: the source with a ring of kMaxWaveformBlocks slots.
constexpr u32 kSourceHeaderSize = 0x30;
constexpr u32 kSlotSize = 0x30;
constexpr std::size_t kMaxBlockSize = kSourceHeaderSize + kMaxWaveformBlocks * kSlotSize;
static_assert(kMaxBlockSize >= kReverbBlockSize);

enum ModuleKind : u32 {
    KIND_NONE,
    KIND_SOURCE,
    KIND_PITCH,
    KIND_ENVELOPE,
    KIND_DISTORTION,
    KIND_COMPRESSOR,
    KIND_FILTER,
    KIND_LFE,
    KIND_GAIN,
    KIND_MATRIX,
    KIND_METER,
    KIND_USER_FX,
    KIND_OUTPUT,
    KIND_REVERB,
    KIND_CUSTOM,
};

// A module of the voice's rack by its index there (0x15640, 0x161c0, 0x14280, 0x14b60; spec 09 §10.1, §4.1),
// with the filter or matrix it is; in a custom rack, i_item is the module's index (spec 02 §1: the game's
// modules, then the custom sampler's source and pitch or the custom mastering's output).
struct ModuleRef {
    ModuleKind kind;
    u32 i_item;
};

ModuleRef FindModule(const Voice& voice, u32 i_module) {
    const RackLimits& limits = voice.p_rack->limits;
    switch (voice.p_rack->p_kind->rack_id) {
    case kSamplerRackId: {
        constexpr ModuleKind kSamplerModules[] = {KIND_SOURCE,   KIND_PITCH,   KIND_DISTORTION,
                                                  KIND_ENVELOPE, KIND_USER_FX, KIND_METER};
        if (i_module < std::size(kSamplerModules)) {
            return {kSamplerModules[i_module], 0};
        }
        const u32 i_filter = i_module - u32(std::size(kSamplerModules));
        return i_filter < limits.max_filters ? ModuleRef{KIND_FILTER, i_filter} : ModuleRef{KIND_NONE, 0};
    }
    case kSubmixerRackId: {
        constexpr ModuleKind kSubmixerModules[] = {KIND_ENVELOPE, KIND_COMPRESSOR, KIND_DISTORTION, KIND_USER_FX,
                                                   KIND_METER};
        if (i_module < std::size(kSubmixerModules)) {
            return {kSubmixerModules[i_module], 0};
        }
        const u32 i_filter = i_module - u32(std::size(kSubmixerModules));
        return i_filter < limits.max_filters ? ModuleRef{KIND_FILTER, i_filter} : ModuleRef{KIND_NONE, 0};
    }
    case kMasteringRackId: {
        if (i_module < kMasteringMatrixTypes) {
            return {KIND_MATRIX, i_module};
        }
        constexpr ModuleKind kMasteringModules[] = {KIND_LFE, KIND_COMPRESSOR, KIND_GAIN, KIND_METER, KIND_METER,
                                                    KIND_OUTPUT};
        const u32 i_after = i_module - kMasteringMatrixTypes;
        return i_after < std::size(kMasteringModules) ? ModuleRef{kMasteringModules[i_after], 0}
                                                      : ModuleRef{KIND_NONE, 0};
    }
    case kReverbRackId:
        return i_module == 0 ? ModuleRef{KIND_REVERB, 0} : ModuleRef{KIND_NONE, 0};
    default: {
        const u32 c_modules = limits.c_custom_modules;
        if (i_module < c_modules) {
            return {KIND_CUSTOM, i_module};
        }
        if (voice.p_rack->p_kind->rack_class == RACK_CLASS_SOURCE && i_module <= c_modules + 1) {
            return {i_module == c_modules ? KIND_SOURCE : KIND_PITCH, i_module};
        }
        if (voice.p_rack->p_kind->rack_class == RACK_CLASS_MASTERING && i_module == c_modules) {
            return {KIND_OUTPUT, i_module};
        }
        return {KIND_NONE, 0};
    }
    }
}

bool IsModuleSet(const Voice& voice, ModuleRef module) {
    const VoiceParams& params = *voice.p_params;
    if (voice.p_rack->p_kind->is_custom && (module.kind == KIND_CUSTOM || module.kind == KIND_OUTPUT)) {
        return params.set_modules & (1u << module.i_item);
    }
    switch (module.kind) {
    case KIND_SOURCE:
    case KIND_PITCH:
        return params.is_setup;
    case KIND_ENVELOPE:
        return params.set_modules & MODULE_ENVELOPE;
    case KIND_DISTORTION:
        return params.set_modules & MODULE_DISTORTION;
    case KIND_COMPRESSOR:
        return params.set_modules & MODULE_COMPRESSOR;
    case KIND_FILTER:
        return params.set_modules & FilterModule(module.i_item);
    case KIND_LFE:
        return params.set_modules & MODULE_LFE;
    case KIND_GAIN:
        return params.set_modules & MODULE_GAIN;
    case KIND_MATRIX:
        return voice.p_mastering_matrices && voice.p_mastering_matrices->is_set[module.i_item];
    case KIND_METER:
        return params.set_modules & MODULE_METER;
    case KIND_USER_FX:
        return params.set_modules & MODULE_USER_FX;
    case KIND_OUTPUT:
        return params.set_modules & MODULE_OUTPUT;
    case KIND_REVERB:
        return params.set_modules & MODULE_REVERB;
    case KIND_CUSTOM:
    case KIND_NONE:
        break;
    }
    return false;
}

template <class T>
void Put(u8* p_block, u32 off, T value) {
    std::memcpy(p_block + off, &value, sizeof(value));
}

u32 WriteSource(const Voice& voice, u8* p_block) {
    const SourceFormat& format = voice.p_params->format;
    const WaveformQueue& queue = voice.queue;
    Put(p_block, 0x00, static_cast<const void*>(voice.p_data_lock));
    Put(p_block, 0x08, u8(voice.data_lock_shift / 2));
    Put(p_block, 0x09, u8(format.waveform_type));
    Put(p_block, 0x0a, u8(format.c_channels));
    Put(p_block, 0x0b, u8(queue.is_terminated));
    Put(p_block, 0x0c, format.sample_rate_hz);
    Put(p_block, 0x10, u16(format.off_frame));
    Put(p_block, 0x12, u16(format.cb_frame_margin));
    Put(p_block, 0x14, format.config_data);
    Put(p_block, 0x18, queue.c_exit_requests);
    Put(p_block, 0x1c, format.flags);
    const u32 c_slots = std::min(queue.c_capacity, kMaxWaveformBlocks);
    for (u32 i_slot = 0; i_slot < c_slots; ++i_slot) {
        const WaveformBlock& slot = queue.p_slots[i_slot];
        u8* p_slot = p_block + kSourceHeaderSize + i_slot * kSlotSize;
        Put(p_slot, 0x00, static_cast<const void*>(slot.p_data));
        Put(p_slot, 0x08, slot.user_data);
        Put(p_slot, 0x10, slot.off_data);
        Put(p_slot, 0x14, slot.cb_data);
        Put(p_slot, 0x18, slot.num_skip_samples);
        Put(p_slot, 0x1c, slot.num_samples);
        Put(p_slot, 0x20, slot.num_repeats);
        Put(p_slot, 0x24, u8(slot.is_continuation));
        Put(p_slot, 0x25, u8(slot.is_marked));
    }
    return kSourceHeaderSize + queue.c_capacity * kSlotSize;
}

u32 WriteEnvelope(const Voice& voice, u8* p_block) {
    const EnvelopeParams& envelope = voice.p_params->envelope;
    Put(p_block, 0, u8(envelope.c_forward));
    Put(p_block, 1, u8(envelope.c_release));
    const u32 c_points = std::min(voice.p_rack->limits.max_envelope_points, kMaxEnvelopePoints);
    for (u32 i_point = 0; i_point < c_points; ++i_point) {
        const OrbisNgs2EnvelopePoint& point = envelope.points[i_point];
        Put(p_block, 4 + 12 * i_point, point.curve);
        Put(p_block, 8 + 12 * i_point, point.duration);
        Put(p_block, 12 + 12 * i_point, point.height);
    }
    return 4 + 12 * voice.p_rack->limits.max_envelope_points;
}

u32 WriteCompressor(const CompressorParams& compressor, u8* p_block) {
    Put(p_block, 0, u8(!(compressor.flags & COMPRESSOR_DISABLED)));
    Put(p_block, 1, u8((compressor.flags & COMPRESSOR_SIDECHAIN) != 0));
    Put(p_block, 2, u8((compressor.flags & COMPRESSOR_RMS) != 0));
    Put(p_block, 3, u8((compressor.flags & COMPRESSOR_LINK_WITHOUT_LFE) != 0));
    const float values[] = {compressor.threshold, compressor.slope,     compressor.knee,
                            compressor.attack_s,  compressor.release_s, compressor.level};
    std::memcpy(p_block + 4, values, sizeof(values));
    return 0x1c;
}

// Serialises the module's pending block into p_block (kMaxBlockSize bytes, zeroed) and answers its
// size in the firmware's layout (spec 09 §12's table).
u32 WriteModule(const Voice& voice, ModuleRef module, u8* p_block) {
    const VoiceParams& params = *voice.p_params;
    switch (module.kind) {
    case KIND_SOURCE:
        return WriteSource(voice, p_block);
    case KIND_PITCH:
        Put(p_block, 0, params.pitch_ratio);
        return 4;
    case KIND_ENVELOPE:
        return WriteEnvelope(voice, p_block);
    case KIND_DISTORTION: {
        const DistortionParams& distortion = params.distortion;
        Put(p_block, 0, u8(!(distortion.flags & 1)));
        const float values[] = {distortion.a,    distortion.b,   distortion.clip,
                                distortion.gate, distortion.wet, distortion.dry};
        std::memcpy(p_block + 4, values, sizeof(values));
        return 0x1c;
    }
    case KIND_COMPRESSOR:
        return WriteCompressor(params.compressor, p_block);
    case KIND_FILTER: {
        const FilterParams& filter = voice.p_filters[module.i_item];
        Put(p_block, 0, u8(filter.location));
        Put(p_block, 1, u8(filter.type));
        Put(p_block, 2, u8(filter.bypass_mask));
        std::memcpy(p_block + 4, filter.f, sizeof(filter.f));
        return 0x20;
    }
    case KIND_LFE:
        Put(p_block, 0, u32(params.lfe.is_enabled != 0));
        Put(p_block, 4, params.lfe.cutoff_hz);
        return 8;
    case KIND_GAIN:
        // NOTE: inferred: one level per channel, the LFE's for channel 3 and the full-band one for the
        // others, which is how the render applies them.
        for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
            Put(p_block, 4 * i_channel, i_channel == kLfeChannel ? params.gain_lfe : params.gain_fbw);
        }
        return 0x20;
    case KIND_MATRIX:
        std::memcpy(p_block, voice.p_mastering_matrices->levels[module.i_item], kMatrixCells * sizeof(float));
        return kMatrixCells * sizeof(float);
    case KIND_METER:
        Put(p_block, 0, u8(params.meter_mode));
        return 1;
    case KIND_USER_FX: {
        const UserFxParams& user_fx = params.user_fx;
        Put(p_block, 0, user_fx.pfn_handler);
        std::memcpy(p_block + 8, user_fx.user_data, sizeof(user_fx.user_data));
        return 0x20;
    }
    case KIND_OUTPUT:
        Put(p_block, 0, params.i_output_buffer);
        return 4;
    case KIND_REVERB:
        WriteReverbBlock(*voice.p_reverb_params, p_block);
        return kReverbBlockSize;
    case KIND_CUSTOM:
        return WriteCustomModuleBlock(voice, module.i_item, p_block, kMaxBlockSize);
    case KIND_NONE:
        break;
    }
    return 0;
}

} // namespace

u32 WriteCustomModuleBlock(const Voice& voice, u32 i_module, u8* p_block, u32 cb_block) {
    const CustomModuleLayout& layout = voice.p_rack->p_custom->layout.modules[i_module];
    const u8* p_pending = voice.p_custom->p_blocks + layout.off_pending;
    switch (layout.module_id) {
    case CUSTOM_MODULE_COMPRESSOR:
        return WriteCompressor(*reinterpret_cast<const CompressorParams*>(p_pending), p_block);
    case CUSTOM_MODULE_FILTER: {
        const auto& filter = *reinterpret_cast<const FilterParams*>(p_pending);
        Put(p_block, 0, u8(filter.location));
        Put(p_block, 1, u8(filter.type));
        Put(p_block, 2, u8(filter.bypass_mask));
        std::memcpy(p_block + 4, filter.f, sizeof(filter.f));
        return 0x20;
    }
    case CUSTOM_MODULE_LFE: {
        const auto& lfe = *reinterpret_cast<const LfeParams*>(p_pending);
        Put(p_block, 0, lfe.is_enabled);
        Put(p_block, 4, lfe.cutoff_hz);
        return 8;
    }
    case CUSTOM_MODULE_GAIN:
        std::memcpy(p_block, p_pending, kMaxChannels * sizeof(float));
        return kMaxChannels * sizeof(float);
    case CUSTOM_MODULE_PEAK_METER:
        Put(p_block, 0, u8(*reinterpret_cast<const u32*>(p_pending)));
        return 1;
    case CUSTOM_MODULE_USER_FX:
        std::memcpy(p_block, p_pending, sizeof(UserFxParams));
        return sizeof(UserFxParams);
    case CUSTOM_MODULE_REVERB:
        WriteReverbBlock(*reinterpret_cast<const ReverbParams*>(p_pending), p_block);
        return kReverbBlockSize;
    case CUSTOM_MODULE_USER_FX2: {
        // NOTE: inferred: spec 09 §12 does not list UserFx2; its block is taken to be the game's own
        // parameter block, as the getter sizes it from the option's paramSize.
        const u32 cb_copied = u32(std::min<std::size_t>(layout.cb_params, cb_block));
        std::memcpy(p_block, p_pending, cb_copied);
        return cb_copied;
    }
    default: {
        static std::uint64_t s_c_uses;
        NoteUnimplemented(&s_c_uses, "sceNgs2VoiceControl 0xc001: block of a custom module without DSP yet "
                                     "reads as unset");
        return 0;
    }
    }
}

// The game's parameter: the module index at 8, the destination at 0x10 and its size at 0x18. The
// firmware copies dstSize bytes from the block whatever the block's size (what follows it in its
// buffer) and does not check the index; psport copies the block, rounded to 8 bytes as the firmware
// lays blocks out, and zeroes the rest, and an index past the rack's modules reads as unset.
void ReadModuleBlock(const Voice& voice, OrbisNgs2VoiceParamHeader* p_header) {
    const auto* p_bytes = reinterpret_cast<const u8*>(p_header);
    u32 i_module;
    void* p_dst;
    u32 cb_dst;
    std::memcpy(&i_module, p_bytes + 0x08, sizeof(i_module));
    std::memcpy(&p_dst, p_bytes + 0x10, sizeof(p_dst));
    std::memcpy(&cb_dst, p_bytes + 0x18, sizeof(cb_dst));
    if (!p_dst) {
        return;
    }
    u8 block[kMaxBlockSize] = {};
    const ModuleRef module = FindModule(voice, i_module);
    const u32 cb_block = IsModuleSet(voice, module) ? (WriteModule(voice, module, block) + 7) & ~7u : 0;
    const u32 cb_copied = std::min({cb_dst, cb_block, u32(kMaxBlockSize)});
    std::memcpy(p_dst, block, cb_copied);
    std::memset(static_cast<u8*>(p_dst) + cb_copied, 0, cb_dst - cb_copied);
}

} // namespace Libraries::Ngs2
