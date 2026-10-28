// SPDX-License-Identifier: GPL-2.0-or-later
// How voices reach each other and the game (spec 04 §2.1, §3): the grain's event fades, the mix of a
// voice into the inputs its ports lead to (0x265a0) through a matrix and a volume ramped from the
// previous grain, the inputs' delay rings (0x26210), and the conversion into the game's render
// buffers (0x27060).
#include <algorithm>
#include <bit>
#include <cstring>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kFadeSamples = 64;
constexpr u32 kFixedMatrixModes = 4;
constexpr u32 kChannelClasses = 4;
constexpr s32 kFirstFixedMatrix = 0x10;
constexpr s32 kLastFixedMatrix = 0x13;
constexpr u32 kClassChannels[kChannelClasses] = {1, 2, 6, 8};
constexpr float kHalfPower = 0.70710678f;
constexpr float kQuarterPower = 0.35355339f;
static_assert(std::bit_cast<u32>(kHalfPower) == 0x3f3504f3 && std::bit_cast<u32>(kQuarterPower) == 0x3eb504f3);

// Firmware 11.00's port matrices at 0x37d50 (spec 09 §5): a row of 17 levels per source channel, by
// source channel count and mode (matrix id 0x10 to 0x13, mode 0 without a matrix), whose columns are
// the destination blocks of 1, 2, 6 and 8 channels. A destination of 3 to 5 channels reads the first
// columns of the 6-channel block and one of 7 those of the 8-channel block.
enum DestBlock : u8 {
    DEST_1 = 0,
    DEST_2 = 1,
    DEST_6 = 2,
    DEST_8 = 3,
};

struct FixedMatrixCell {
    u8 c_src;
    u8 mode;
    DestBlock dest;
    u8 i_src;
    u8 i_dst;
    float level;
};

constexpr float h = kHalfPower;
constexpr float q = kQuarterPower;

// The non-zero levels; every other cell, and modes 1 to 3 of 5, 6, 7 and 8 channels and mode 3 of
// 3 and 4, are 0.
constexpr FixedMatrixCell kFixedMatrixCells[] = {
    {1, 0, DEST_1, 0, 0, 1.0f}, {1, 0, DEST_2, 0, 0, h}, {1, 0, DEST_2, 0, 1, h},
    {1, 0, DEST_6, 0, 2, 1.0f}, {1, 0, DEST_8, 0, 2, 1.0f},
    {1, 1, DEST_6, 0, 3, 1.0f}, {1, 1, DEST_8, 0, 3, 1.0f},
    {1, 2, DEST_1, 0, 0, 1.0f}, {1, 2, DEST_2, 0, 0, h}, {1, 2, DEST_2, 0, 1, h},
    {1, 2, DEST_6, 0, 0, h}, {1, 2, DEST_6, 0, 1, h}, {1, 2, DEST_8, 0, 0, h}, {1, 2, DEST_8, 0, 1, h},

    {2, 0, DEST_1, 0, 0, h}, {2, 0, DEST_1, 1, 0, h}, {2, 0, DEST_2, 0, 0, 1.0f}, {2, 0, DEST_2, 1, 1, 1.0f},
    {2, 0, DEST_6, 0, 0, 1.0f}, {2, 0, DEST_6, 1, 1, 1.0f}, {2, 0, DEST_8, 0, 0, 1.0f}, {2, 0, DEST_8, 1, 1, 1.0f},
    {2, 1, DEST_1, 0, 0, 1.0f}, {2, 1, DEST_2, 0, 0, h}, {2, 1, DEST_2, 0, 1, h},
    {2, 1, DEST_6, 0, 2, 1.0f}, {2, 1, DEST_6, 1, 3, 1.0f}, {2, 1, DEST_8, 0, 2, 1.0f}, {2, 1, DEST_8, 1, 3, 1.0f},
    {2, 2, DEST_1, 0, 0, 1.0f}, {2, 2, DEST_2, 0, 0, h}, {2, 2, DEST_2, 0, 1, h},
    {2, 2, DEST_6, 0, 0, h}, {2, 2, DEST_6, 0, 1, h}, {2, 2, DEST_6, 1, 3, 1.0f},
    {2, 2, DEST_8, 0, 0, h}, {2, 2, DEST_8, 0, 1, h}, {2, 2, DEST_8, 1, 3, 1.0f},

    {3, 0, DEST_1, 0, 0, h}, {3, 0, DEST_1, 1, 0, h}, {3, 0, DEST_2, 0, 0, 1.0f}, {3, 0, DEST_2, 1, 1, 1.0f},
    {3, 0, DEST_6, 0, 0, 1.0f}, {3, 0, DEST_6, 1, 1, 1.0f}, {3, 0, DEST_6, 2, 3, 1.0f},
    {3, 0, DEST_8, 0, 0, 1.0f}, {3, 0, DEST_8, 1, 1, 1.0f}, {3, 0, DEST_8, 2, 3, 1.0f},
    {3, 1, DEST_1, 0, 0, h}, {3, 1, DEST_1, 1, 0, h}, {3, 1, DEST_1, 2, 0, 1.0f},
    {3, 1, DEST_2, 0, 0, 1.0f}, {3, 1, DEST_2, 1, 1, 1.0f}, {3, 1, DEST_2, 2, 0, h}, {3, 1, DEST_2, 2, 1, h},
    {3, 1, DEST_6, 0, 0, 1.0f}, {3, 1, DEST_6, 1, 1, 1.0f}, {3, 1, DEST_6, 2, 2, 1.0f},
    {3, 1, DEST_8, 0, 0, 1.0f}, {3, 1, DEST_8, 1, 1, 1.0f}, {3, 1, DEST_8, 2, 2, 1.0f},
    {3, 2, DEST_1, 0, 0, h}, {3, 2, DEST_1, 1, 0, h}, {3, 2, DEST_1, 2, 0, q},
    {3, 2, DEST_2, 0, 0, 1.0f}, {3, 2, DEST_2, 1, 1, 1.0f}, {3, 2, DEST_2, 2, 0, q}, {3, 2, DEST_2, 2, 1, q},
    {3, 2, DEST_6, 0, 0, 1.0f}, {3, 2, DEST_6, 1, 1, 1.0f}, {3, 2, DEST_6, 2, 4, h}, {3, 2, DEST_6, 2, 5, h},
    {3, 2, DEST_8, 0, 0, 1.0f}, {3, 2, DEST_8, 1, 1, 1.0f}, {3, 2, DEST_8, 2, 6, h}, {3, 2, DEST_8, 2, 7, h},

    {4, 0, DEST_1, 0, 0, h}, {4, 0, DEST_1, 1, 0, h}, {4, 0, DEST_1, 2, 0, 0.5f}, {4, 0, DEST_1, 3, 0, 0.5f},
    {4, 0, DEST_2, 0, 0, 1.0f}, {4, 0, DEST_2, 1, 1, 1.0f}, {4, 0, DEST_2, 2, 0, h}, {4, 0, DEST_2, 3, 1, h},
    {4, 0, DEST_6, 0, 0, 1.0f}, {4, 0, DEST_6, 1, 1, 1.0f}, {4, 0, DEST_6, 2, 4, 1.0f}, {4, 0, DEST_6, 3, 5, 1.0f},
    {4, 0, DEST_8, 0, 0, 1.0f}, {4, 0, DEST_8, 1, 1, 1.0f}, {4, 0, DEST_8, 2, 4, 1.0f}, {4, 0, DEST_8, 3, 5, 1.0f},
    {4, 1, DEST_1, 0, 0, h}, {4, 1, DEST_1, 1, 0, h}, {4, 1, DEST_1, 2, 0, 1.0f},
    {4, 1, DEST_2, 0, 0, 1.0f}, {4, 1, DEST_2, 1, 1, 1.0f}, {4, 1, DEST_2, 2, 0, h}, {4, 1, DEST_2, 2, 1, h},
    {4, 1, DEST_6, 0, 0, 1.0f}, {4, 1, DEST_6, 1, 1, 1.0f}, {4, 1, DEST_6, 2, 2, 1.0f}, {4, 1, DEST_6, 3, 3, 1.0f},
    {4, 1, DEST_8, 0, 0, 1.0f}, {4, 1, DEST_8, 1, 1, 1.0f}, {4, 1, DEST_8, 2, 2, 1.0f}, {4, 1, DEST_8, 3, 3, 1.0f},
    {4, 2, DEST_1, 0, 0, h}, {4, 2, DEST_1, 1, 0, h}, {4, 2, DEST_1, 2, 0, 1.0f}, {4, 2, DEST_1, 3, 0, q},
    {4, 2, DEST_2, 0, 0, 1.0f}, {4, 2, DEST_2, 1, 1, 1.0f}, {4, 2, DEST_2, 2, 0, h}, {4, 2, DEST_2, 2, 1, h},
    {4, 2, DEST_2, 3, 0, q}, {4, 2, DEST_2, 3, 1, q},
    {4, 2, DEST_6, 0, 0, 1.0f}, {4, 2, DEST_6, 1, 1, 1.0f}, {4, 2, DEST_6, 2, 2, 1.0f},
    {4, 2, DEST_6, 3, 4, h}, {4, 2, DEST_6, 3, 5, h},
    {4, 2, DEST_8, 0, 0, 1.0f}, {4, 2, DEST_8, 1, 1, 1.0f}, {4, 2, DEST_8, 2, 2, 1.0f},
    {4, 2, DEST_8, 3, 6, h}, {4, 2, DEST_8, 3, 7, h},

    {5, 0, DEST_1, 0, 0, h}, {5, 0, DEST_1, 1, 0, h}, {5, 0, DEST_1, 2, 0, 1.0f},
    {5, 0, DEST_1, 3, 0, 0.5f}, {5, 0, DEST_1, 4, 0, 0.5f},
    {5, 0, DEST_2, 0, 0, 1.0f}, {5, 0, DEST_2, 1, 1, 1.0f}, {5, 0, DEST_2, 2, 0, h}, {5, 0, DEST_2, 2, 1, h},
    {5, 0, DEST_6, 0, 0, 1.0f}, {5, 0, DEST_6, 1, 1, 1.0f}, {5, 0, DEST_6, 2, 2, 1.0f},
    {5, 0, DEST_6, 3, 4, 1.0f}, {5, 0, DEST_6, 4, 5, 1.0f},
    {5, 0, DEST_8, 0, 0, 1.0f}, {5, 0, DEST_8, 1, 1, 1.0f}, {5, 0, DEST_8, 2, 2, 1.0f},
    {5, 0, DEST_8, 3, 4, 1.0f}, {5, 0, DEST_8, 4, 5, 1.0f},

    {6, 0, DEST_1, 0, 0, h}, {6, 0, DEST_1, 1, 0, h}, {6, 0, DEST_1, 2, 0, 1.0f},
    {6, 0, DEST_1, 4, 0, 0.5f}, {6, 0, DEST_1, 5, 0, 0.5f},
    {6, 0, DEST_2, 0, 0, 1.0f}, {6, 0, DEST_2, 1, 1, 1.0f}, {6, 0, DEST_2, 2, 0, h}, {6, 0, DEST_2, 2, 1, h},
    {6, 0, DEST_2, 4, 0, 0.5f}, {6, 0, DEST_2, 5, 1, 0.5f},
    {6, 0, DEST_6, 0, 0, 1.0f}, {6, 0, DEST_6, 1, 1, 1.0f}, {6, 0, DEST_6, 2, 2, 1.0f},
    {6, 0, DEST_6, 3, 3, 1.0f}, {6, 0, DEST_6, 4, 4, 1.0f}, {6, 0, DEST_6, 5, 5, 1.0f},
    {6, 0, DEST_8, 0, 0, 1.0f}, {6, 0, DEST_8, 1, 1, 1.0f}, {6, 0, DEST_8, 2, 2, 1.0f},
    {6, 0, DEST_8, 3, 3, 1.0f}, {6, 0, DEST_8, 4, 4, 1.0f}, {6, 0, DEST_8, 5, 5, 1.0f},

    {7, 0, DEST_1, 0, 0, h}, {7, 0, DEST_1, 1, 0, h}, {7, 0, DEST_1, 2, 0, 1.0f},
    {7, 0, DEST_1, 4, 0, 0.5f}, {7, 0, DEST_1, 5, 0, 0.5f}, {7, 0, DEST_1, 6, 0, q},
    {7, 0, DEST_2, 0, 0, 1.0f}, {7, 0, DEST_2, 1, 1, 1.0f}, {7, 0, DEST_2, 2, 0, h}, {7, 0, DEST_2, 2, 1, h},
    {7, 0, DEST_2, 4, 0, 0.5f}, {7, 0, DEST_2, 5, 1, 0.5f}, {7, 0, DEST_2, 6, 0, q}, {7, 0, DEST_2, 6, 1, q},
    {7, 0, DEST_6, 0, 0, 1.0f}, {7, 0, DEST_6, 1, 1, 1.0f}, {7, 0, DEST_6, 2, 2, 1.0f},
    {7, 0, DEST_6, 3, 3, 1.0f}, {7, 0, DEST_6, 4, 4, 1.0f}, {7, 0, DEST_6, 5, 5, 1.0f},
    {7, 0, DEST_6, 6, 4, h}, {7, 0, DEST_6, 6, 5, h},
    {7, 0, DEST_8, 0, 0, 1.0f}, {7, 0, DEST_8, 1, 1, 1.0f}, {7, 0, DEST_8, 2, 2, 1.0f},
    {7, 0, DEST_8, 3, 3, 1.0f}, {7, 0, DEST_8, 4, 4, 1.0f}, {7, 0, DEST_8, 5, 5, 1.0f},
    {7, 0, DEST_8, 6, 6, h}, {7, 0, DEST_8, 6, 7, h},

    {8, 0, DEST_1, 0, 0, h}, {8, 0, DEST_1, 1, 0, h}, {8, 0, DEST_1, 2, 0, 1.0f}, {8, 0, DEST_1, 4, 0, 0.5f},
    {8, 0, DEST_1, 5, 0, 0.5f}, {8, 0, DEST_1, 6, 0, 0.5f}, {8, 0, DEST_1, 7, 0, 0.5f},
    {8, 0, DEST_2, 0, 0, 1.0f}, {8, 0, DEST_2, 1, 1, 1.0f}, {8, 0, DEST_2, 2, 0, h}, {8, 0, DEST_2, 2, 1, h},
    {8, 0, DEST_2, 4, 0, 0.5f}, {8, 0, DEST_2, 5, 1, 0.5f}, {8, 0, DEST_2, 6, 0, 0.5f}, {8, 0, DEST_2, 7, 1, 0.5f},
    {8, 0, DEST_6, 0, 0, 1.0f}, {8, 0, DEST_6, 1, 1, 1.0f}, {8, 0, DEST_6, 2, 2, 1.0f}, {8, 0, DEST_6, 3, 3, 1.0f},
    {8, 0, DEST_6, 4, 4, 1.0f}, {8, 0, DEST_6, 5, 5, 1.0f}, {8, 0, DEST_6, 6, 4, 1.0f}, {8, 0, DEST_6, 7, 5, 1.0f},
    {8, 0, DEST_8, 0, 0, 1.0f}, {8, 0, DEST_8, 1, 1, 1.0f}, {8, 0, DEST_8, 2, 2, 1.0f}, {8, 0, DEST_8, 3, 3, 1.0f},
    {8, 0, DEST_8, 4, 4, 1.0f}, {8, 0, DEST_8, 5, 5, 1.0f}, {8, 0, DEST_8, 6, 6, 1.0f}, {8, 0, DEST_8, 7, 7, 1.0f},
};

DestBlock DestBlockOf(u32 c_dst) {
    return c_dst == 1 ? DEST_1 : c_dst == 2 ? DEST_2 : c_dst <= 6 ? DEST_6 : DEST_8;
}

// The table expanded to one kMaxChannels x kMaxChannels matrix per source count, mode and
// destination count, rows by source channel.
struct FixedMatrices {
    float levels[kMaxChannels][kFixedMatrixModes][kMaxChannels][kMatrixCells];
};

const FixedMatrices& GetFixedMatrices() {
    static const FixedMatrices s_matrices = [] {
        FixedMatrices matrices{};
        for (const FixedMatrixCell& cell : kFixedMatrixCells) {
            for (u32 c_dst = 1; c_dst <= kMaxChannels; ++c_dst) {
                if (DestBlockOf(c_dst) == cell.dest && cell.i_dst < c_dst) {
                    matrices.levels[cell.c_src - 1][cell.mode][c_dst - 1][cell.i_src * kMaxChannels + cell.i_dst] =
                        cell.level;
                }
            }
        }
        return matrices;
    }();
    return s_matrices;
}

// c_src and c_dst are 1 to kMaxChannels: setups refuse other counts.
const float* FixedMatrix(u32 c_src, u32 mode, u32 c_dst) {
    return GetFixedMatrices().levels[c_src - 1][mode][c_dst - 1];
}

// The matrix a port mixes through: the voice's own when it has levels, the fixed one otherwise.
const float* PortMatrix(const Voice& voice, s32 matrix_id, const float* p_levels, u32 c_src, u32 c_dst) {
    if (matrix_id >= 0 && u32(matrix_id) < voice.p_rack->limits.max_matrices && voice.p_matrix_is_set[matrix_id]) {
        return p_levels + std::size_t(matrix_id) * kMatrixCells;
    }
    const bool is_fixed = matrix_id >= kFirstFixedMatrix && matrix_id <= kLastFixedMatrix;
    return FixedMatrix(c_src, is_fixed ? u32(matrix_id - kFirstFixedMatrix) : 0, c_dst);
}

float* InputBlock(const Voice& voice, u32 i_input, u32 i_block) {
    const Rack& rack = *voice.p_rack;
    const std::size_t i_slot = std::size_t(i_input) * rack.c_input_blocks + i_block;
    return voice.p_input_ring + i_slot * rack.limits.max_channels * rack.limits.max_grain_samples;
}

// The first writer of a block in a grain owns it: it starts from silence, and the ones after add.
float* ClaimInputBlock(const Voice& voice, u32 i_input, u32 i_block, u32 c_samples) {
    const Rack& rack = *voice.p_rack;
    float* p_block = InputBlock(voice, i_input, i_block);
    bool& is_started = voice.p_input_started[i_input * rack.c_input_blocks + i_block];
    if (!is_started) {
        for (u32 i_channel = 0; i_channel < rack.limits.max_channels; ++i_channel) {
            std::memset(p_block + std::size_t(i_channel) * rack.limits.max_grain_samples, 0,
                        c_samples * sizeof(float));
        }
        is_started = true;
    }
    return p_block;
}

void MixPort(RenderContext& context, const Voice& voice, const ActivePort& port, const Planes& src, u32 c_src) {
    const Voice& dest = *port.p_dest;
    const Rack& dest_rack = *dest.p_rack;
    const u32 c_dst = dest.c_in_channels;
    if (c_dst == 0 || port.i_input >= dest_rack.limits.max_inputs || dest_rack.c_input_blocks == 0) {
        return;
    }
    const u32 c_samples = context.c_grain_samples;
    const u32 c_blocks = dest_rack.c_input_blocks;
    const u32 max_delay = c_blocks >= 2 ? (c_blocks - 2) * c_samples : 0;
    const u32 delay = std::min(port.delay_samples, max_delay);
    const float* p_current = PortMatrix(voice, port.matrix_id, voice.p_matrix_levels, c_src, c_dst);
    const float* p_previous =
        port.is_fresh ? p_current
                      : PortMatrix(voice, port.previous_matrix_id, voice.p_previous_matrix_levels, c_src, c_dst);
    const float volume_start = port.is_fresh ? port.volume : port.previous_volume;
    const u32 i_block = (dest_rack.i_input_block + delay / c_samples) % c_blocks;
    const u32 off_block = delay % c_samples;
    const std::size_t cb_plane = dest_rack.limits.max_grain_samples;
    float* p_first = ClaimInputBlock(dest, port.i_input, i_block, c_samples);
    float* p_second = off_block ? ClaimInputBlock(dest, port.i_input, (i_block + 1) % c_blocks, c_samples) : nullptr;
    for (u32 i_src = 0; i_src < c_src; ++i_src) {
        for (u32 i_dst = 0; i_dst < c_dst; ++i_dst) {
            const u32 i_cell = i_src * kMaxChannels + i_dst;
            const float gain_start = volume_start * p_previous[i_cell];
            const float gain_end = port.volume * p_current[i_cell];
            if (gain_start == 0.0f && gain_end == 0.0f) {
                continue;
            }
            const float* p_in = src.p_channel[i_src];
            RampAdd(p_first + i_dst * cb_plane + off_block, p_in, c_samples - off_block, gain_start, gain_end, 0,
                    c_samples);
            if (p_second) {
                RampAdd(p_second + i_dst * cb_plane, p_in + c_samples - off_block, off_block, gain_start, gain_end,
                        c_samples - off_block, c_samples);
            }
        }
    }
}

void StoreSample(s16* p_out, float value, bool is_first) {
    const s32 sample = s32(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
    *p_out = s16(is_first ? sample : std::clamp(s32(*p_out) + sample, -32768, 32767));
}

// The next grain's ramps start from this grain's volumes and matrices.
void EndPortsGrain(Voice* p_voice) {
    const RackLimits& limits = p_voice->p_rack->limits;
    for (u32 i_port = 0; i_port < limits.max_ports; ++i_port) {
        ActivePort& port = p_voice->p_active_ports[i_port];
        port.previous_volume = port.volume;
        port.previous_matrix_id = port.matrix_id;
        port.is_fresh = false;
    }
    std::copy_n(p_voice->p_matrix_levels, std::size_t(limits.max_matrices) * kMatrixCells,
                p_voice->p_previous_matrix_levels);
}

} // namespace

u32 ChannelClass(u32 c_channels) {
    switch (c_channels) {
    case 1:
        return 0;
    case 2:
        return 1;
    case 8:
        return 3;
    default:
        return 2;
    }
}

Planes WorkPlanes(const RenderContext& context, WorkSet set) {
    Planes planes;
    for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
        planes.p_channel[i_channel] =
            context.p_work + (std::size_t(set) * kMaxChannels + i_channel) * context.c_grain_samples;
    }
    return planes;
}

void ApplyEventCode(const Planes& planes, u32 c_channels, u32 c_samples, u32 event_code) {
    for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
        float* p_samples = planes.p_channel[i_channel];
        switch (event_code) {
        case EVENT_CODE_FADE_OUT_TAIL: {
            float* p_tail = p_samples + c_samples - kFadeSamples;
            RampWrite(p_tail, p_tail, kFadeSamples, 1.0f, 0.0f);
            break;
        }
        case EVENT_CODE_FADE_IN_HEAD:
            RampWrite(p_samples, p_samples, kFadeSamples, 0.0f, 1.0f);
            break;
        case EVENT_CODE_SILENT:
            std::memset(p_samples, 0, c_samples * sizeof(float));
            break;
        case EVENT_CODE_STOP:
        case EVENT_CODE_CUT:
            RampWrite(p_samples, p_samples, kFadeSamples, 1.0f, 0.0f);
            std::memset(p_samples + kFadeSamples, 0, (c_samples - kFadeSamples) * sizeof(float));
            break;
        default:
            break;
        }
    }
}

void MixPorts(RenderContext& context, Voice* p_voice, const Planes& planes, u32 c_channels) {
    const RackLimits& limits = p_voice->p_rack->limits;
    for (u32 i_port = 0; i_port < limits.max_ports; ++i_port) {
        const ActivePort& port = p_voice->p_active_ports[i_port];
        if (!port.p_dest || !port.p_dest->is_active) {
            continue;
        }
        Planes src = planes;
        const u32 location = 2 + i_port;
        if (HasFilterAt(*p_voice, location)) {
            src = WorkPlanes(context, WORK_PORT);
            for (u32 i_channel = 0; i_channel < c_channels; ++i_channel) {
                std::memcpy(src.p_channel[i_channel], planes.p_channel[i_channel],
                            context.c_grain_samples * sizeof(float));
            }
            RunFilters(*p_voice, location, src, c_channels, context.c_grain_samples);
        }
        MixPort(context, *p_voice, port, src, c_channels);
    }
    EndPortsGrain(p_voice);
}

void MixCustomPorts(RenderContext& context, Voice* p_voice, const Planes* p_buffers, u32 valid_mask, u32 c_channels) {
    const RackLimits& limits = p_voice->p_rack->limits;
    for (u32 i_port = 0; i_port < limits.max_ports; ++i_port) {
        const ActivePort& port = p_voice->p_active_ports[i_port];
        const u32 i_buffer = limits.custom_port_sources[i_port];
        if (port.p_dest && port.p_dest->is_active && (valid_mask & (1u << i_buffer))) {
            MixPort(context, *p_voice, port, p_buffers[i_buffer], c_channels);
        }
    }
    EndPortsGrain(p_voice);
}

Planes ReadInput(RenderContext& context, Voice* p_voice, u32 i_input) {
    const Rack& rack = *p_voice->p_rack;
    bool& is_started = p_voice->p_input_started[i_input * rack.c_input_blocks + rack.i_input_block];
    if (p_voice->is_in_cycle && p_voice->is_first_grain) {
        is_started = false;
    }
    float* p_block = ClaimInputBlock(*p_voice, i_input, rack.i_input_block, context.c_grain_samples);
    Planes planes;
    for (u32 i_channel = 0; i_channel < kMaxChannels; ++i_channel) {
        const u32 i_plane = std::min(i_channel, rack.limits.max_channels - 1);
        planes.p_channel[i_channel] = p_block + std::size_t(i_plane) * rack.limits.max_grain_samples;
    }
    return planes;
}

void WriteRenderBuffer(RenderContext& context, u32 i_buffer, const Planes& planes, u32 c_channels) {
    if (i_buffer >= context.c_buffers) {
        static std::uint64_t s_c_uses;
        NoteDropped(&s_c_uses, "Ngs2 mastering output to a render buffer index past the render's buffers");
        return;
    }
    const OrbisNgs2RenderBufferInfo& buffer = context.p_buffers[i_buffer];
    const u32 c_buffer_channels = buffer.numChannels;
    const bool is_first = !(context.written_mask & (1u << i_buffer));
    context.written_mask |= 1u << i_buffer;
    const u32 c_samples = context.c_grain_samples;
    const bool is_float = buffer.waveformType == 0x18 || buffer.waveformType == 0x19;
    for (u32 i = 0; i < c_samples; ++i) {
        for (u32 i_channel = 0; i_channel < c_buffer_channels; ++i_channel) {
            const std::size_t i_out = std::size_t(i) * c_buffer_channels + i_channel;
            const bool has_channel = i_channel < c_channels;
            if (!has_channel && !is_first) {
                continue;
            }
            const float value = has_channel ? planes.p_channel[i_channel][i] : 0.0f;
            if (is_float) {
                float* p_out = static_cast<float*>(buffer.buffer) + i_out;
                *p_out = is_first ? value : *p_out + value;
            } else {
                StoreSample(static_cast<s16*>(buffer.buffer) + i_out, value, is_first);
            }
        }
    }
}

void ExpandVoiceMatrices(Voice* p_voice) {
    const u32 c_src = p_voice->c_out_channels;
    for (u32 i_matrix = 0; i_matrix < p_voice->p_rack->limits.max_matrices; ++i_matrix) {
        const VoiceMatrix& matrix = p_voice->p_matrices[i_matrix];
        float* p_levels = p_voice->p_matrix_levels + std::size_t(i_matrix) * kMatrixCells;
        std::fill_n(p_levels, kMatrixCells, 0.0f);
        p_voice->p_matrix_is_set[i_matrix] = matrix.c_levels > 0;
        // The game orders the levels by destination: level(s -> d) = aLevel[d * c_src + s] (spec 03 §4).
        const u32 c_used = std::min(matrix.c_levels, kMaxChannels * c_src);
        for (u32 i_src = 0; i_src < c_src; ++i_src) {
            for (u32 i_dst = 0; i_dst < kMaxChannels; ++i_dst) {
                const u32 i_level = i_src + i_dst * c_src;
                if (i_level < c_used) {
                    p_levels[i_src * kMaxChannels + i_dst] = matrix.levels[i_level];
                }
            }
        }
    }
}

// The setup's defaults are the port matrices of mode 0 (spec 09 §4.1); the four types whose input and
// output classes match get none, so their input passes as it is.
void ResetMasteringMatrices(MasteringMatrices* p_matrices) {
    constexpr u32 kSameClassTypes = 0x8421;
    for (u32 i_type = 0; i_type < kMasteringMatrixTypes; ++i_type) {
        const float* p_default =
            FixedMatrix(kClassChannels[i_type / kChannelClasses], 0, kClassChannels[i_type % kChannelClasses]);
        std::copy_n(p_default, kMatrixCells, p_matrices->levels[i_type]);
        p_matrices->is_set[i_type] = !((kSameClassTypes >> i_type) & 1);
    }
}

} // namespace Libraries::Ngs2
