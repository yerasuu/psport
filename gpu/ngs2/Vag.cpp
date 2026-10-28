// SPDX-License-Identifier: GPL-2.0-or-later
// The sampler's VAG and HEVAG decoder, waveform type 0x1c (0x1e6a0; spec 05 §4, 05-vag-coefs): 16-byte
// blocks per channel of 28 4-bit samples through a 4th-order predictor chosen among the firmware's 128,
// and the loop flags of the last channel's block when configData bit 0 asks for them.
#include <bit>

#include "Source.h"

namespace Libraries::Ngs2 {
namespace {

// The predictors of libSceNgs2.sprx 11.00's table at 0x45dc0, as IEEE-754 words: c1 to c4 of
// y[n] = s[n] + c1 y[n-1] + c2 y[n-2] + c3 y[n-3] + c4 y[n-4]. The firmware stores each one as the
// IIR's response to blocks of four samples; these are the coefficients that response comes from.
constexpr u32 kPredictors = 128;
constexpr u32 kPredictorWords[kPredictors][4] = {
    {0x80000000, 0x80000000, 0x80000000, 0x80000000}, {0x3f700000, 0x80000000, 0x80000000, 0x80000000},
    {0x3fe60000, 0xbf500000, 0x80000000, 0x80000000}, {0x3fc40000, 0xbf5c0000, 0x80000000, 0x80000000},
    {0x3ff40000, 0xbf700000, 0x80000000, 0x80000000}, {0x3fe62c00, 0xbf5c9800, 0x80000000, 0x80000000},
    {0x3fe2ac00, 0xbf663000, 0x80000000, 0x80000000}, {0x3fd98000, 0xbf6b1000, 0x80000000, 0x80000000},
    {0x3fcd3400, 0xbf700000, 0x80000000, 0x80000000}, {0x3fbbf000, 0xbf700000, 0x80000000, 0x80000000},
    {0x3fa83000, 0xbf700000, 0x80000000, 0x80000000}, {0x3f923c00, 0xbf700000, 0x80000000, 0x80000000},
    {0x3f74c000, 0xbf700000, 0x80000000, 0x80000000}, {0x3f41c800, 0xbf700000, 0x80000000, 0x80000000},
    {0x3f0c4000, 0xbf700000, 0x80000000, 0x80000000}, {0x3ea9d000, 0xbf700000, 0x80000000, 0x80000000},
    {0x3de38000, 0xbf700000, 0x80000000, 0x80000000}, {0xbde38000, 0xbf700000, 0x80000000, 0x80000000},
    {0xbea9d000, 0xbf700000, 0x80000000, 0x80000000}, {0xbf0c4000, 0xbf700000, 0x80000000, 0x80000000},
    {0xbf41c800, 0xbf700000, 0x80000000, 0x80000000}, {0xbf74c000, 0xbf700000, 0x80000000, 0x80000000},
    {0xbf923c00, 0xbf700000, 0x80000000, 0x80000000}, {0xbfa83000, 0xbf700000, 0x80000000, 0x80000000},
    {0xbfbbf000, 0xbf700000, 0x80000000, 0x80000000}, {0xbfcd3400, 0xbf700000, 0x80000000, 0x80000000},
    {0xbfd98000, 0xbf6b1000, 0x80000000, 0x80000000}, {0xbfe2ac00, 0xbf663000, 0x80000000, 0x80000000},
    {0xbfe62c00, 0xbf5c9800, 0x80000000, 0x80000000}, {0x3f280000, 0xbf900000, 0x3ed00000, 0xbec00000},
    {0xbf480000, 0xbf600000, 0xbed00000, 0xbe900000}, {0xbfa40000, 0xbf680000, 0xbee00000, 0xbe000000},
    {0xbca70000, 0xbeaa2000, 0xbd770000, 0xbd874000}, {0xbf683000, 0xbe8ad000, 0xbe8fa000, 0x3d540000},
    {0xbf7a0800, 0xbec5e000, 0xbeafe000, 0x3d108000}, {0x3f3c1000, 0xbf147000, 0x3ea59000, 0xbe224000},
    {0x3eed6000, 0xbf591000, 0x3ef23000, 0xbe180000}, {0xbf80b400, 0xbea24000, 0xbe817000, 0x3ce30000},
    {0x3f8fbc00, 0x3e77c000, 0xbe2cc000, 0xbe90c000}, {0x3fcb7400, 0xbebe4000, 0xbeed0000, 0x3e1e6000},
    {0x3fcce000, 0xbf0c3800, 0xbe8ca000, 0x3e502000}, {0xbe508000, 0xbeea0000, 0xbf49c000, 0x3dd20000},
    {0x3f745800, 0xbf074000, 0x3e83f000, 0xbd7d0000}, {0x3f958c00, 0xbe270000, 0xbdbd4000, 0x3d738000},
    {0x3f9cc000, 0xbea02000, 0x3d160000, 0x3cc70000}, {0xbf144800, 0xbf00d000, 0xbf2b7000, 0xbe3aa000},
    {0xbf384000, 0x3e94a000, 0xbf159800, 0xbf591800}, {0xbe152000, 0xbf8e7400, 0xbe1aa000, 0xbec2a000},
    {0x3eadf000, 0xbf5e2000, 0xbe44e000, 0xbe34e000}, {0xbf653000, 0xbe80e000, 0xbe8a4000, 0x3d5f0000},
    {0x3f3f7800, 0xbea11000, 0xbd1d8000, 0xbb100000}, {0x3f93c400, 0xbe684000, 0x3c4c0000, 0x3d010000},
    {0x3f762000, 0xbf0c1800, 0x3e84e000, 0xbd868000}, {0xbf602000, 0xbe606000, 0xbe815000, 0x3d710000},
    {0xbf65f000, 0xbe836000, 0xbe8b9000, 0x3d5c0000}, {0xbf8f4800, 0xbedb5000, 0xbea72000, 0xbd438000},
    {0xbea4e000, 0xbea57000, 0xbe712000, 0xbe4ca000}, {0x3e6a2000, 0x3f8f7c00, 0x3e688000, 0xbf339000},
    {0x3f8ff800, 0x3e686000, 0xbe0c8000, 0xbe97b000}, {0x3fce5000, 0xbebc4000, 0xbf015800, 0x3e2b4000},
    {0x3fc25400, 0xbf15c000, 0xbd000000, 0x3d9b8000}, {0xbea5d000, 0xbe0f0000, 0xbec6e000, 0xbf56f000},
    {0x3f91cc00, 0xbe030000, 0xbdcf0000, 0x3d7d8000}, {0x3e4d4000, 0xbd9b0000, 0xbdec8000, 0xbf042800},
    {0x3f04b000, 0xbf6d0800, 0xbd854000, 0xbe8d3000}, {0xbf8c7000, 0xbef33000, 0xbeaf7000, 0x3bb00000},
    {0xbea02000, 0xbeaaf000, 0xbe584000, 0xbe6d6000}, {0x3ec6e000, 0xbd718000, 0xbdb28000, 0xbe31a000},
    {0x3f780800, 0xbef04000, 0x3eb05000, 0xbdfec000}, {0x3f9d5400, 0xbea31000, 0x3d1d0000, 0x3cc30000},
    {0x3f834000, 0xbece1000, 0x3e41e000, 0xbc9b0000}, {0xbf854400, 0xbead7000, 0xbe980000, 0xbd280000},
    {0x3f941400, 0xbe6b4000, 0x3c580000, 0x3d000000}, {0x3c160000, 0xbe8c1000, 0xbd150000, 0xbf58c800},
    {0xbf8edc00, 0xbf020000, 0xbeb93000, 0x3a200000}, {0xbf633000, 0xbe74c000, 0xbe86c000, 0x3d660000},
    {0xbeab4000, 0x3eea1000, 0x3f398000, 0xbe082000}, {0x3f8c8400, 0x3e738000, 0xbdaac000, 0xbea90000},
    {0x3fccb400, 0xbeb13000, 0xbef0e000, 0x3e03e000}, {0x3f951c00, 0xbe752000, 0x3c820000, 0x3cfa0000},
    {0x3f24c000, 0xbf057000, 0x3ec34000, 0xbec55000}, {0xbf708000, 0xbed37000, 0xbeb5b000, 0xbd640000},
    {0x3f646800, 0x3e9db000, 0x3d590000, 0xbe99d000}, {0x3fa08c00, 0xbeb33000, 0x3d438000, 0x3cac0000},
    {0x3faaf000, 0xbee89000, 0x3da64000, 0x3c420000}, {0x3b400000, 0xbd1b0000, 0xbe1dc000, 0x3d200000},
    {0xbf7dd000, 0xbe96b000, 0xbe907000, 0xbd078000}, {0xbf84fc00, 0xbebfb000, 0xbe921000, 0x3ca30000},
    {0x3d230000, 0xbef06000, 0x3d518000, 0xbde94000}, {0x3f8afc00, 0x3e892000, 0xbd874000, 0xbeb40000},
    {0x3fbca400, 0xbe6a8000, 0xbe7c2000, 0xbd964000}, {0x3f886400, 0xbed5f000, 0x3e514000, 0xbca90000},
    {0x3fca5800, 0xbef07000, 0xbebbf000, 0x3e734000}, {0x3f9cd800, 0xbea0a000, 0x3d170000, 0x3cc60000},
    {0x3f92a400, 0xbeac5000, 0x3dfc0000, 0x3ba40000}, {0xbf138000, 0xbf1c7000, 0xbeb24000, 0xbe112000},
    {0x3f75e800, 0xbf073000, 0x3e857000, 0xbd7a8000}, {0x3e992000, 0xbf865400, 0x3e226000, 0xbeadc000},
    {0x3f9f4000, 0xbeacb000, 0x3d340000, 0x3cb50000}, {0x3fb0c400, 0xbf03a800, 0x3dd04000, 0x3bd40000},
    {0x3f9ea800, 0xbea9c000, 0x3d2c8000, 0x3cb90000}, {0x3f969800, 0xbe312000, 0xbdb84000, 0x3d710000},
    {0x3ef0e000, 0xbf0e5000, 0x3eb1b000, 0xbed45000}, {0x3f515800, 0xbf30d800, 0x3e8c9000, 0xbe064000},
    {0x3fad2800, 0xbef44000, 0x3db60000, 0x3c200000}, {0xbe004000, 0xbf994800, 0xbdc94000, 0xbed86000},
    {0x3fa27000, 0xbeea2000, 0x3e2ae000, 0xbc400000}, {0x3fa0bc00, 0x3df70000, 0xbe6f6000, 0xbe33a000},
    {0x3f788800, 0x3ef26000, 0xbdbf0000, 0xbecbf000}, {0x3fc64400, 0xbed2f000, 0xbed1a000, 0x3e81f000},
    {0x3f4f8800, 0x3ec5e000, 0x3ce70000, 0xbe829000}, {0xbe92c000, 0xbf65d000, 0xbe730000, 0xbf00d000},
    {0x3f912800, 0xbefc7000, 0x3e6ba000, 0xbcf60000}, {0x3f10c800, 0xbf481800, 0x3e5a8000, 0xbe4a6000},
    {0x3fafbc00, 0xbf00e800, 0x3dc88000, 0x3bf40000}, {0x3f993c00, 0xbe8ec000, 0x3cdb0000, 0x3cde0000},
    {0x3f982000, 0xbe400000, 0xbdb08000, 0x3d6c8000}, {0x3f83e000, 0xbed6c000, 0x3e434000, 0xbcb00000},
    {0x3f916c00, 0xbdff4000, 0xbdd0c000, 0x3d7e8000}, {0x3e544000, 0xbf8d0400, 0x3dac0000, 0xbebdd000},
    {0x3f9f9800, 0xbeae7000, 0x3d380000, 0x3cb30000}, {0x3f831800, 0x3ecb0000, 0xbdbd8000, 0xbebad000},
    {0x3f610800, 0x3ecf4000, 0x3bb80000, 0xbea36000}, {0xbc300000, 0xbf740800, 0xbd868000, 0xbf0ed800},
    {0x3f418800, 0xbf222800, 0x3eac8000, 0xbe1ba000}, {0x3fc09400, 0xbe214000, 0xbecd4000, 0x3cfd0000},
    {0x3fcc2000, 0xbefeb000, 0xbeb08000, 0x3e6aa000}, {0x3f26a800, 0x3ebb7000, 0x3dc1c000, 0xbe0d8000},
};

enum VagFlag : u32 {
    VAG_FLAG_END = 1,
    VAG_FLAG_LOOP_END = 3,
    VAG_FLAG_LOOP_START = 6,
};

// b0 = predictor low nibble << 4 | shift, b1 = predictor high nibble << 4 | flags, then 28 signed
// nibbles, low nibble first, each worth nibble / 8 / 2^shift (shift 0 to 15, no clamp). p_history
// holds y[n-1] to y[n-4].
void DecodeBlock(const u8* p_block, float* p_history, float* p_out) {
    const u32 i_predictor = (p_block[1] & 0xf0) | (p_block[0] >> 4);
    if (i_predictor >= kPredictors) {
        // The firmware reads past its table here (spec 05 §4); the predictor that adds nothing stands
        // in for whatever it finds there.
        static std::uint64_t s_c_uses;
        if (__atomic_fetch_add(&s_c_uses, 1, __ATOMIC_RELAXED) == 0) {
            Common::Log::Stubbed("Ngs2 VAG block with a predictor past the firmware's 128", &s_c_uses);
            LOG_WARNING(Lib_Ngs2, "VAG block with predictor {} past the firmware's 128: decoded without one",
                        i_predictor);
        }
    }
    const u32* p_words = kPredictorWords[i_predictor < kPredictors ? i_predictor : 0];
    const float c1 = std::bit_cast<float>(p_words[0]);
    const float c2 = std::bit_cast<float>(p_words[1]);
    const float c3 = std::bit_cast<float>(p_words[2]);
    const float c4 = std::bit_cast<float>(p_words[3]);
    const float scale = 1.0f / float(8u << (p_block[0] & 0xf));
    float y1 = p_history[0], y2 = p_history[1], y3 = p_history[2], y4 = p_history[3];
    for (u32 i = 0; i < kVagFrameSamples; ++i) {
        const u32 byte = p_block[2 + i / 2];
        const s32 nibble = s32((i & 1 ? byte >> 4 : byte & 0xf) << 28) >> 28;
        const float y = float(nibble) * scale + c1 * y1 + c2 * y2 + c3 * y3 + c4 * y4;
        y4 = y3, y3 = y2, y2 = y1, y1 = y;
        p_out[i] = y;
    }
    p_history[0] = y1, p_history[1] = y2, p_history[2] = y3, p_history[3] = y4;
}

// Flag 3 ends the loop as a forced repeat (0x21a30 mode 1): unless an exit request waits, the block
// counts a repeat and goes on from the frame flag 6 marked, its history untouched.
void RepeatLoop(Voice* p_voice, SourceState* p_source, u32 cb_stride) {
    const u32 c_popped = p_voice->active_queue.c_popped;
    AdvanceBlock(p_voice, p_source, cb_stride, kVagFrameSamples, ADVANCE_REPEAT);
    if (p_voice->active_queue.c_popped == c_popped) {
        const u32 i_frame = p_source->i_vag_loop_frame;
        SeekBlock(p_voice, p_source, i_frame * kVagFrameSamples, u64(i_frame) * cb_stride);
    }
}

} // namespace

u32 DecodeVag(Voice* p_voice, SourceState* p_source, const SourceFormat& format, float* const* pp_in, u32 i_out,
              u32 c_wanted) {
    SourceState& source = *p_source;
    const u32 cb_blocks = format.c_channels * kVagBlockBytes;
    const u32 cb_stride = format.cb_frame_margin + cb_blocks;
    const bool has_loop_flags = format.config_data & 1;
    u8 scratch[kMaxChannels * kVagBlockBytes];
    u32 c_done = 0;
    while (c_done < c_wanted) {
        Spans spans;
        if (!GatherSpans(p_voice, p_source, cb_stride, &spans)) {
            break;
        }
        const u8* p_frame = SampleBytes(spans, format.off_frame, cb_blocks, scratch);
        for (u32 i_channel = 0; i_channel < format.c_channels; ++i_channel) {
            DecodeBlock(p_frame + i_channel * kVagBlockBytes, source.vag_history[i_channel],
                        pp_in[i_channel] + i_out + c_done);
        }
        c_done += kVagFrameSamples;
        const u32 flags = has_loop_flags ? p_frame[cb_blocks - kVagBlockBytes + 1] & 0xf : 0;
        if (flags == VAG_FLAG_LOOP_END) {
            RepeatLoop(p_voice, p_source, cb_stride);
            continue;
        }
        if (flags == VAG_FLAG_LOOP_START) {
            source.i_vag_loop_frame = source.i_sample / kVagFrameSamples;
        }
        AdvanceBlock(p_voice, p_source, cb_stride, kVagFrameSamples, ADVANCE_STEP);
        // The voice ends once its history drains (spec 05 §3.4).
        if (flags == VAG_FLAG_END) {
            source.is_end = true;
            break;
        }
    }
    return c_done;
}

} // namespace Libraries::Ngs2
