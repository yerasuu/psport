// SPDX-License-Identifier: GPL-2.0-or-later
// Custom racks 0x4001, 0x4002 and 0x4003 (phase F6, kb #1098), written from firmware 11.00's 0x10ea0
// (creation), 0x119c0, 0x11d80 and 0x12160 (parameters), 0x126a0, 0x12fb0 and 0x13310 (processing), 0x12a90
// (the module chain) and 0x12420 (module info) as rea read them (spec 02 §4.5, 03 §5.5, §6, 04 §2.2, §2.5,
// §2.6, §4.3, 06, 07 §6, 09 §12): the game's list of up to 24 modules, each reading one of the rack's work
// buffers and writing another, with its parameters, its DSP state and its part of the voice state. A module's
// parameters live in the rack's buffer twice, as everything else VoiceControl sets: the game's copy and the
// render's, which the flush refreshes.
#pragma once

#include <cstddef>
#include <type_traits>

#include "Internal.h"
#include "Reverb.h"

namespace Libraries::Ngs2 {

enum CustomModuleId : u32 {
    CUSTOM_MODULE_COMPRESSOR = 0x12,
    CUSTOM_MODULE_FILTER = 0x13,
    CUSTOM_MODULE_LFE = 0x14,
    CUSTOM_MODULE_GAIN = 0x15,
    CUSTOM_MODULE_PEAK_METER = 0x18,
    CUSTOM_MODULE_USER_FX = 0x19,
    CUSTOM_MODULE_REVERB = 0x1b,
    CUSTOM_MODULE_USER_FX2 = 0x1f,
};

// Where one module's blocks sit in each voice's custom block: the game's parameters, the render's, the DSP
// state, and the memory the DSP state points into (the peak meter's ring, the reverb's lines, UserFx2's work
// block).
struct CustomModuleLayout {
    u32 module_id;
    std::size_t off_pending;
    std::size_t off_active;
    std::size_t cb_params;
    std::size_t off_dsp;
    std::size_t off_extra;
    std::size_t cb_extra;
};

struct CustomLayout {
    std::size_t cb_voice;
    CustomModuleLayout modules[ORBIS_NGS2_CUSTOM_MAX_MODULES];
};

// The rack's own: the layout, the work buffers (numBuffers sets of kMaxChannels planes of maxGrainSamples
// floats, then one more set the reverb reads its input from when it writes in place), and each UserFx2
// module's common block.
struct CustomRack {
    CustomLayout layout;
    float* p_buffers;
    u32 c_buffers;
    u32 c_plane_samples;
    void* p_commons[ORBIS_NGS2_CUSTOM_MAX_MODULES];
};

// Each voice's: the modules whose parameter VoiceControl changed since the last flush, the ones the flush
// handed to the render since its last grain (the firmware's control word +0x4c and process state +0x28, spec
// 09), and the module blocks.
struct CustomVoice {
    u32 changed_modules;
    u32 active_changed_modules;
    u8* p_blocks;
};

// The filter's render side: the parameters with their designed coefficients, the rate they were designed for,
// and each channel's history.
struct CustomFilterDsp {
    FilterParams filter;
    u32 designed_rate_hz;
    BiquadState states[kMaxChannels];
};

struct CustomGainDsp {
    float previous[kMaxChannels];
};

static_assert(std::is_trivially_copyable_v<CustomFilterDsp> && std::is_trivially_copyable_v<CustomGainDsp>);

// Rack layout (Rack.cpp calls these in both of its passes: measuring, where the arena hands out nothing, and
// building).
CustomRack* LayOutCustomRack(Arena* p_arena, const RackLimits& limits);
CustomVoice* LayOutCustomVoice(Arena* p_arena, const RackLimits& limits, const CustomRack* p_custom_rack);
// UserFx2's setup and cleanup handlers, once per voice after the rack is linked and before it is freed.
void StartCustomModules(Rack* p_rack);
void StopCustomModules(Rack* p_rack);

// VoiceControl ids 0x4001xxxx to 0x4003xxxx and 0x4000xxxx (spec 03 §5.5), under the voice lock.
s32 ControlCustom(OrbisNgs2Handle h_voice, Voice* p_voice, const OrbisNgs2VoiceParamHeader* p_header);
// The flush's part (spec 09): the changed modules' blocks reach the render.
void FlushCustomVoice(Voice* p_voice);
void ProcessCustomVoice(RenderContext& context, Voice* p_voice, u32* p_event_code);
// 0xc001 on a module of the game's list (spec 09 §12): its pending block in the firmware's layout, into
// p_block of cb_block zeroed bytes; 0 when it has no parameter or rea has not read its layout.
u32 WriteCustomModuleBlock(const Voice& voice, u32 i_module, u8* p_block, u32 cb_block);

// The modules (CustomModules.cpp).
std::size_t GetCustomParamsSize(const CustomModule& module);
std::size_t GetCustomDspSize(const CustomModule& module);
std::size_t GetCustomExtraSize(const CustomModule& module, const RackLimits& limits);
void InitCustomModule(const CustomModule& module, const RackLimits& limits, u8* p_dsp, u8* p_extra);
bool IsCustomModuleImplemented(u32 module_id);
s32 SetCustomModule(OrbisNgs2Handle h_voice, Voice* p_voice, u32 i_module, u32 selector,
                    const OrbisNgs2VoiceParamHeader* p_header);

// What a module's processing gets (0x12a90 → the module's process, spec 04 §4.2): flags bit 0 is its first
// grain since PLAY, bit 1 a parameter change; p_state is its part of the voice state or null.
struct CustomModuleRun {
    const CustomModule* p_module;
    const void* p_params;
    u8* p_dsp;
    u8* p_extra;
    void* p_common;
    u8* p_state;
    u32 cb_state;
    u32 flags;
    u32 c_in_channels;
    u32 c_out_channels;
};
void RunCustomModule(const RenderContext& context, Voice* p_voice, const CustomModuleRun& run, const Planes& in,
                     const Planes& extra, const Planes& out, const Planes& scratch, u32 c_channels);

// UserFx with the module's own grain flags.
void CallUserFx(const RenderContext& context, Voice* p_voice, const UserFxParams& user_fx, const Planes& planes,
                u32 c_channels, u32 flags);
// UserFx2's handlers (UserFx.cpp). Control and process are called under the voice lock, which they release
// for the game's code; setup and cleanup outside of it.
s32 CallUserFx2Control(Voice* p_voice, const CustomModuleOption& option, void* p_common, void* p_param,
                       const void* p_data, std::size_t cb_data);
s32 CallUserFx2Process(Voice* p_voice, const CustomModuleOption& option, const CustomModuleRun& run,
                       const Planes& planes, u32 c_samples, u32 rate_hz);
void CallUserFx2Setup(const CustomModuleOption& option, void* p_common, void* p_param, void* p_work, u32 max_voices,
                      u32 i_voice);
void CallUserFx2Cleanup(const CustomModuleOption& option, void* p_common, void* p_param, void* p_work, u32 max_voices,
                        u32 i_voice);

} // namespace Libraries::Ngs2
