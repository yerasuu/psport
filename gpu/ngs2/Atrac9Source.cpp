// SPDX-License-Identifier: GPL-2.0-or-later
// The sampler's ATRAC9 waveforms, type 0x40 (0x29ac0, 0x29c70, 0x16c60 to 0x17160; spec 05 §3.3, §6,
// 10 §3). Each frame of the superframe being read is decoded by LibAtrac9 into float planes, with the
// decoder PLAY reserved for the voice. The decoder restarts, overlap included, only on its first use by
// the voice and at the start of a pass through a block that skips samples, whose first numSkipSamples
// are decoded and dropped; a block that skips none goes on from the decoder's state, so that streamed
// blocks join without a gap. The firmware's AJM path (setup flags bit 0) queues superframes to libSceAjm
// a grain ahead; decoding them here at once gives the same samples a grain sooner.
#include <algorithm>
#include <utility>

#include "Atrac9Glue.h"
#include "Source.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kInfinite = 0xffffffffu;
// A config's frames take up to 2048 bytes, and a superframe holds up to 8 of them.
constexpr u32 kMaxSuperframeBytes = 2048 * 8;

// A failure the voice reports as its state (spec 05 §3.4): logged with its count, the first time and
// on every power of two after it, so that a broken stream stays visible without flooding the log.
s32 FailCodec(std::uint64_t* p_c_failures, s32 code, const char* sz_what, s32 detail) {
    const std::uint64_t c_failures = __atomic_add_fetch(p_c_failures, 1, __ATOMIC_RELAXED);
    if (c_failures == 1) {
        Common::Log::Stubbed(sz_what, p_c_failures);
    }
    if ((c_failures & (c_failures - 1)) == 0) {
        LOG_ERROR(Lib_Ngs2, "{} ({:#x}, error {:#x}), {} so far", sz_what, static_cast<u32>(detail),
                  static_cast<u32>(code), c_failures);
    }
    return code;
}

} // namespace

std::size_t GetAtrac9StateSize() {
    return (ngs2_atrac9_get_decoder_size() + 15) & ~std::size_t(15);
}

void ReleaseAtrac9Decoder(Voice* p_voice) {
    if (p_voice->p_atrac9) {
        Voice* p_owner = p_voice;
        p_voice->p_atrac9->p_owner.compare_exchange_strong(p_owner, nullptr, std::memory_order_release);
    }
    const u32 c_works = std::exchange(p_voice->c_atrac9_channel_works, 0);
    p_voice->p_rack->c_atrac9_channel_works_used.fetch_sub(c_works, std::memory_order_relaxed);
}

u32 DecodeAtrac9(Voice* p_voice, SourceState* p_source, const SourceFormat& format, float* const* pp_in, u32 i_out,
                 u32 c_wanted) {
    SourceState& source = *p_source;
    Atrac9Decoder* p_decoder = p_voice->p_atrac9;
    // PLAY reserves the decoder a setup asks for, so a voice reaches here without one only when its
    // setup changed after its PLAY; that is psport's own failure, not a code of the SDK.
    if (!p_decoder || p_decoder->is_ajm != bool(format.flags & 1)) {
        static std::uint64_t s_c_uses;
        NoteDropped(&s_c_uses, "Ngs2 ATRAC9: a voice decodes without the decoder PLAY reserves");
        return 0;
    }
    // The firmware ignores what 0x29ac0 answers and fails at the first frame instead (spec 10 §3, §5).
    Atrac9Config config;
    if (!ParseAtrac9Config(format.config_data, &config) || config.c_frame_samples == 0) {
        static std::uint64_t s_c_failures;
        source.codec_error = FailCodec(&s_c_failures, ORBIS_NGS2_ERROR_CODEC_DECODE_FAIL,
                                       "Ngs2 ATRAC9: invalid config data", s32(format.config_data));
        return 0;
    }
    const u32 cb_superframe = config.cb_frame * config.c_frames_per_superframe;
    const u32 cb_stride = format.cb_frame_margin + cb_superframe;
    const u8 header[4] = {u8(format.config_data >> 24), u8(format.config_data >> 16), u8(format.config_data >> 8),
                          u8(format.config_data)};
    float* pp_frame[kMaxChannels];
    for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
        pp_frame[i_channel] = p_decoder->p_frame + std::size_t(i_channel) * kMaxAtrac9FrameSamples;
    }
    u8 scratch[kMaxSuperframeBytes];
    u32 c_done = 0;
    while (c_done < c_wanted) {
        if (source.i_codec_sample < source.c_codec_samples) {
            const u32 c_copy = std::min(source.c_codec_samples - source.i_codec_sample, c_wanted - c_done);
            for (u32 i_channel = 0; i_channel < format.c_channels; ++i_channel) {
                std::copy_n(pp_frame[i_channel] + source.i_codec_sample, c_copy, pp_in[i_channel] + i_out + c_done);
            }
            source.i_codec_sample += c_copy;
            c_done += c_copy;
            continue;
        }
        Spans spans;
        if (!GatherSpans(p_voice, p_source, cb_stride, &spans)) {
            break;
        }
        const bool is_skipping_pass = source.i_sample == 0 && source.i_codec_frame == 0 && source.num_skip_samples > 0;
        if (!source.is_atrac9_ready || is_skipping_pass) {
            if (const int result = ngs2_atrac9_reset(p_decoder->p_state, header); result != 0) {
                static std::uint64_t s_c_failures;
                source.codec_error = FailCodec(&s_c_failures, ORBIS_NGS2_ERROR_CODEC_DECODE_FAIL,
                                               "Ngs2 ATRAC9: failed to initialize the decoder", result);
                return c_done;
            }
            source.is_atrac9_ready = true;
        }
        const u8* p_superframe = SampleBytes(spans, format.off_frame, cb_superframe, scratch);
        int cb_used = 0;
        const int result = ngs2_atrac9_decode_frame(p_decoder->p_state, p_superframe + source.off_codec_frame,
                                                    int(cb_superframe - source.off_codec_frame), pp_frame, &cb_used);
        if (result != 0 || cb_used <= 0 || u32(cb_used) > cb_superframe - source.off_codec_frame) {
            static std::uint64_t s_c_failures;
            source.codec_error = FailCodec(&s_c_failures, ORBIS_NGS2_ERROR_CODEC_DECODE_FAIL,
                                           "Ngs2 ATRAC9: failed to decode codec data", result);
            return c_done;
        }
        source.off_codec_frame += u32(cb_used);
        ++source.i_codec_frame;
        const u32 end_region = RegionEnd(source);
        const u32 c_left = end_region == kInfinite ? config.c_frame_samples
                                                   : std::min(config.c_frame_samples, end_region - source.i_sample);
        source.c_codec_samples = c_left;
        source.i_codec_sample =
            source.num_skip_samples > source.i_sample ? std::min(source.num_skip_samples - source.i_sample, c_left) : 0;
        u32 cb_advance = 0;
        if (source.i_codec_frame == config.c_frames_per_superframe) {
            cb_advance = cb_stride;
            source.i_codec_frame = 0;
            source.off_codec_frame = 0;
        }
        // A pass that ends inside a superframe, looping back or moving to the next block, starts the
        // next one from that block's first superframe.
        const u32 i_next = source.i_sample + c_left;
        AdvanceBlock(p_voice, p_source, cb_advance, c_left, ADVANCE_STEP);
        if (source.i_sample != i_next) {
            source.i_codec_frame = 0;
            source.off_codec_frame = 0;
        }
    }
    return c_done;
}

} // namespace Libraries::Ngs2
