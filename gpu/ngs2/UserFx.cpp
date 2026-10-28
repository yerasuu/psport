// SPDX-License-Identifier: GPL-2.0-or-later
// The game's own effects (phase F6, kb #1098): UserFx, module 0x19 (0x23f50 parameter, 0x24020 process; spec
// 03 §6.6, 04 §2.2, §4), in the sampler, the submixer and custom racks, and UserFx2, module 0x1f (0x24140,
// 0x243c0, 0x24470, 0x245c0; spec 03 §6.9), in custom racks. Their handlers get the SDK's contexts (shadPS4's
// ngs2.h); which context field carries what is read from those names.
#include "common/logging/log.h"

#include "Custom.h"

namespace Libraries::Ngs2 {
namespace {

// The game's handler may call back into Ngs2 for its own voice (VoiceControl, GetState), whose busy bit would
// then spin forever, so the bit is released for the call and taken back after it. What the render uses
// meanwhile is its own copy of the voice, which only the flush writes, under the process lock the render
// keeps throughout as the firmware does around the game's voice callbacks (spec 08 §1.4).
class GuestCall {
public:
    explicit GuestCall(Voice* p_voice) : m_p_voice{p_voice} {
        UnlockVoice(p_voice);
    }
    ~GuestCall() {
        LockVoice(m_p_voice);
    }
    GuestCall(const GuestCall&) = delete;
    GuestCall& operator=(const GuestCall&) = delete;

private:
    Voice* m_p_voice;
};

void NoteHandlerFailure(std::uint64_t* p_c_uses, const char* sz_what, s32 result) {
    if (__atomic_fetch_add(p_c_uses, 1, __ATOMIC_RELAXED) == 0) {
        Common::Log::Stubbed(sz_what, p_c_uses);
        LOG_WARNING(Lib_Ngs2, "{}: the game's handler answered {:#x}", sz_what, static_cast<u32>(result));
    }
}

void MarkError(Voice* p_voice) {
    p_voice->flags = (p_voice->flags | STATE_ERROR) & ~u32(STATE_PLAYING);
}

} // namespace

void RunVoiceUserFx(const RenderContext& context, Voice* p_voice, const UserFxParams& user_fx, const Planes& planes,
                    u32 c_channels, u32 module) {
    const u32 flags = ((p_voice->first_active_modules & module) ? 1u : 0u) | (p_voice->are_params_changed ? 2u : 0u);
    CallUserFx(context, p_voice, user_fx, planes, c_channels, flags);
}

// NOTE: inferred: the context's flags are the module's grain flags (process m+0x70, spec 04 §4.2), the only
// flags rea saw a module receive.
void CallUserFx(const RenderContext& context, Voice* p_voice, const UserFxParams& user_fx, const Planes& planes,
                u32 c_channels, u32 flags) {
    if (!user_fx.pfn_handler) {
        return;
    }
    // A copy, so that the game writing the array cannot move the render's planes.
    Planes channels = planes;
    OrbisNgs2UserFxProcessContext process{
        channels.p_channel, user_fx.user_data[0],    user_fx.user_data[1],  user_fx.user_data[2], flags,
        c_channels,         context.c_grain_samples, context.sample_rate_hz};
    s32 result;
    {
        GuestCall call{p_voice};
        result = user_fx.pfn_handler(&process);
    }
    if (result < 0) {
        static std::uint64_t s_c_failures;
        NoteHandlerFailure(&s_c_failures, "Ngs2 UserFx process handler failed, voice reports ERROR", result);
        MarkError(p_voice);
    }
}

s32 CallUserFx2Control(Voice* p_voice, const CustomModuleOption& option, void* p_common, void* p_param,
                       const void* p_data, std::size_t cb_data) {
    OrbisNgs2UserFx2ControlContext control{p_data, cb_data, p_common, p_param, option.user_data, {}};
    GuestCall call{p_voice};
    return option.pfn_control(&control);
}

s32 CallUserFx2Process(Voice* p_voice, const CustomModuleOption& option, const CustomModuleRun& run,
                       const Planes& planes, u32 c_samples, u32 rate_hz) {
    if (!option.pfn_process) {
        return ORBIS_OK;
    }
    Planes channels = planes;
    OrbisNgs2UserFx2ProcessContext process{};
    process.aChannelData = channels.p_channel;
    process.common = run.p_common;
    process.param = run.p_params;
    process.work = run.p_extra;
    process.state = run.p_state;
    process.userData = option.user_data;
    process.flags = run.flags;
    process.numInputChannels = run.c_in_channels;
    process.numOutputChannels = run.c_out_channels;
    process.numGrainSamples = c_samples;
    process.sampleRate = rate_hz;
    s32 result;
    {
        GuestCall call{p_voice};
        result = option.pfn_process(&process);
    }
    if (result < 0) {
        static std::uint64_t s_c_failures;
        NoteHandlerFailure(&s_c_failures, "Ngs2 UserFx2 process handler failed, voice reports ERROR", result);
        MarkError(p_voice);
    }
    return result;
}

void CallUserFx2Setup(const CustomModuleOption& option, void* p_common, void* p_param, void* p_work, u32 max_voices,
                      u32 i_voice) {
    if (!option.pfn_setup) {
        return;
    }
    OrbisNgs2UserFx2SetupContext setup{p_common, p_param, p_work, option.user_data, max_voices, i_voice, {}};
    if (const s32 result = option.pfn_setup(&setup); result < 0) {
        static std::uint64_t s_c_failures;
        NoteHandlerFailure(&s_c_failures, "Ngs2 UserFx2 setup handler failed (ignored)", result);
    }
}

void CallUserFx2Cleanup(const CustomModuleOption& option, void* p_common, void* p_param, void* p_work, u32 max_voices,
                        u32 i_voice) {
    if (!option.pfn_cleanup) {
        return;
    }
    OrbisNgs2UserFx2CleanupContext cleanup{p_common, p_param, p_work, option.user_data, max_voices, i_voice, {}};
    if (const s32 result = option.pfn_cleanup(&cleanup); result < 0) {
        static std::uint64_t s_c_failures;
        NoteHandlerFailure(&s_c_failures, "Ngs2 UserFx2 cleanup handler failed (ignored)", result);
    }
}

} // namespace Libraries::Ngs2
