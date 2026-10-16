// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "shader_recompiler/ir/type.h"
#include "video_core/amdgpu/resource.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <boost/container/static_vector.hpp>

namespace Shader {

static constexpr u32 NUM_USER_DATA_REGS = 16;
static constexpr u32 NUM_IMAGES = 64;
static constexpr u32 NUM_BUFFERS = 40;
static constexpr u32 NUM_SAMPLERS = 16;
static constexpr u32 NUM_FMASKS = 8;

using SharpLocation = u16;

constexpr SharpLocation UNKNOWN_LOCATION = std::numeric_limits<u16>::max();

template <typename T>
struct SharpFetch {
    static constexpr std::size_t N = sizeof(T) / sizeof(u32);
    static_assert(N <= 8);

    std::array<u32, N> immediates;
    std::array<SharpLocation, N> offsets;
    u8 load_mask;

    bool operator==(const SharpFetch&) const = default;

    template <u32 num_dwords = N>
        requires(num_dwords <= N)
    constexpr bool Fetch(const u32* flatbuf, T* out) const {
        u8 mask = load_mask;
        for (u32 i = 0; i < num_dwords; i++) {
            if (offsets[i] == UNKNOWN_LOCATION) {
                return false;
            }
        }
        std::array<u32, num_dwords> out_dw;
        for (u32 i = 0; i < num_dwords; i++) {
            out_dw[i] = (mask & 1) ? flatbuf[offsets[i]] : immediates[i];
            mask >>= 1;
        }
        std::memcpy(out, out_dw.data(), sizeof(out_dw));
        return true;
    }
};

enum class SharpFetchPostOp : u8 {
    None,
    // For buffers
    BitwiseOrDw1WithImm,
    OffsetByProgramBase,
    // For images,
    ConvertCubeTo2DArray,
    // For samplers
    DisableAnisoIfSingleLod,
    ForceRepeatXyzClamp,
    ForceLastTexelXyClamp,
    ClearAnisoRatioAndThreshold,
};

enum class BufferType : u8 {
    Guest,
    Flatbuf,
    BdaPagetable,
    FaultBuffer,
    GdsBuffer,
    SharedMemory,
    ClipPlanes,
    /// psport: a guest buffer whose V# the shader picks at run time (DynamicSharpSources).
    Dynamic,
};

/// psport: the V# dwords a Dynamic binding's shader computes at run time, with the values each
/// can take that the CPU reads at bind time (flat user data locations or immediates). The
/// shader reaches the game's memory through the page table; the bind prepares every candidate.
struct DynamicSharpSources {
    static constexpr u32 MaxSources = 4;
    static constexpr u32 MaxCandidates = 16;

    std::array<std::array<u32, MaxSources>, 4> values{};
    std::array<u8, 4> c_sources{};
    std::array<u8, 4> location_masks{};
    u8 dynamic_mask{};
    /// Some source is not readable by the CPU: what the shader computes from it reaches only
    /// memory other bindings made resident, and reads zero elsewhere.
    bool is_unbounded{};

    bool operator==(const DynamicSharpSources&) const = default;
};
using DynamicCandidateList =
    boost::container::static_vector<AmdGpu::Buffer, DynamicSharpSources::MaxCandidates>;

struct BufferResource {
    SharpFetch<AmdGpu::Buffer> sharp_fetch{};
    IR::Type used_types{};
    BufferType buffer_type{};
    bool is_written{};
    bool is_formatted{};
    SharpFetchPostOp post_op{};
    u32 post_op_dw1_mask{};
    DynamicSharpSources dynamic{};

    bool IsSpecial() const noexcept {
        return buffer_type != BufferType::Guest;
    }

    /// psport: every V# a Dynamic binding's shader can pick among the sources the CPU reads
    /// (the static dwords as GetSharp reads them, each run-time dword from one of its sources).
    /// False when there are more than DynamicSharpSources::MaxCandidates; `out` is then empty.
    bool DynamicCandidates(const auto& info, DynamicCandidateList& out) const noexcept {
        out.clear();
        std::array<u32, 4> shape;
        if (!sharp_fetch.Fetch(info.FlatUserData().data(),
                               reinterpret_cast<AmdGpu::Buffer*>(shape.data()))) {
            return true;
        }
        u32 c_candidates = 1;
        for (const u8 c_sources : dynamic.c_sources) {
            c_candidates *= std::max<u32>(c_sources, 1);
        }
        if (c_candidates > DynamicSharpSources::MaxCandidates) {
            return false;
        }
        const auto flat = info.FlatUserData();
        for (u32 i_candidate = 0; i_candidate < c_candidates; ++i_candidate) {
            auto dwords = shape;
            bool is_readable = true;
            u32 digits = i_candidate;
            for (u32 i = 0; i < 4 && is_readable; ++i) {
                const u32 c_sources = dynamic.c_sources[i];
                if (c_sources == 0) {
                    continue;
                }
                const u32 i_source = digits % c_sources;
                digits /= c_sources;
                const u32 value = dynamic.values[i][i_source];
                if (((dynamic.location_masks[i] >> i_source) & 1) == 0) {
                    dwords[i] = value;
                } else if (value < flat.size()) {
                    dwords[i] = flat[value];
                } else {
                    is_readable = false;
                }
            }
            if (!is_readable) {
                continue;
            }
            if (post_op == SharpFetchPostOp::BitwiseOrDw1WithImm) {
                dwords[1] |= post_op_dw1_mask;
            }
            AmdGpu::Buffer buffer;
            std::memcpy(&buffer, dwords.data(), sizeof(buffer));
            if (buffer.Valid() && buffer.base_address != 0 && buffer.GetSize() != 0) {
                out.push_back(buffer);
            }
        }
        return true;
    }

    constexpr AmdGpu::Buffer GetSharp(const auto& info) const noexcept {
        AmdGpu::Buffer buffer;
        if (!sharp_fetch.Fetch(info.FlatUserData().data(), &buffer)) {
            return AmdGpu::Buffer::Null();
        }
        if (post_op == SharpFetchPostOp::BitwiseOrDw1WithImm) {
            reinterpret_cast<u32*>(&buffer)[1] |= post_op_dw1_mask;
        } else if (post_op == SharpFetchPostOp::OffsetByProgramBase) {
            buffer.base_address += info.ProgramBase();
        }
        if (!buffer.Valid()) {
            return AmdGpu::Buffer::Null();
        }
        return buffer;
    }
};
using BufferResourceList = boost::container::static_vector<BufferResource, NUM_BUFFERS>;

/// psport PS_LAYER_MEMORY: a guest buffer of 64 MiB or more (a shader given nearly all memory as
/// one buffer) is reached through the page table, not as one bound range: the game's memory is
/// in several buffers (its chunks, VRAM mirrors). docs/en/history/MEMORY_MODULE_PLAN.md.
constexpr u64 PagedBufferMinSize = u64{64} << 20;
inline bool IsPagedBuffer(bool enabled, const AmdGpu::Buffer& sharp) {
    return enabled && sharp.base_address != 0 && u64(sharp.GetSize()) >= PagedBufferMinSize;
}

enum class MipStorageFallbackMode : u16 {
    None,
    DynamicIndex,
    ConstantIndex,
};

struct ImageResource {
    SharpFetch<AmdGpu::Image> sharp_fetch{};
    bool is_depth{};
    bool is_atomic{};
    bool is_array{};
    bool is_written{};
    bool is_r128{};
    /// psport: read other than by normalized sampling without offsets (texel loads, size
    /// queries, offsets): needs the native-size image, not a reduced scene proxy.
    bool needs_native{};
    u8 constant_mip_index{};
    MipStorageFallbackMode mip_fallback_mode{};
    SharpFetchPostOp post_op{};

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        AmdGpu::Image image{};
        if (!Fetch(info.FlatUserData().data(), &image)) {
            return AmdGpu::Image::Null(is_depth);
        }
        if (post_op == SharpFetchPostOp::ConvertCubeTo2DArray) {
            image.type = u64(AmdGpu::ImageType::Color2DArray);
            image.depth = (image.depth + 1) * 6 - 1;
        }
        if (!image.Valid()) {
            image = AmdGpu::Image::Null(is_depth);
        } else if (is_depth) {
            const auto data_fmt = image.GetDataFmt();
            if (data_fmt != AmdGpu::DataFormat::Format16 &&
                data_fmt != AmdGpu::DataFormat::Format32) {
                image = AmdGpu::Image::Null(true);
            }
        }
        return image;
    }

    constexpr bool Fetch(const u32* flatbuf, AmdGpu::Image* out) const {
        if (!is_r128) {
            // Fetch full 8 byte T#
            return sharp_fetch.Fetch(flatbuf, out);
        }
        // Fetch r128 T# and fix pitch
        if (!sharp_fetch.Fetch<4>(flatbuf, out)) {
            return false;
        }
        out->pitch = out->width;
        return true;
    }

    u32 NumBindings(const auto& info) const {
        const AmdGpu::Image tsharp = GetSharp(info);
        return (mip_fallback_mode == MipStorageFallbackMode::DynamicIndex)
                   ? (tsharp.last_level - tsharp.base_level + 1)
                   : 1;
    }
};
using ImageResourceList = boost::container::static_vector<ImageResource, NUM_IMAGES>;

struct SamplerResource {
    SharpFetch<AmdGpu::Sampler> sharp_fetch{};
    SharpFetchPostOp post_op{};
    SharpLocation post_op_tsharp_dw3_off{};
    bool is_depth{};

    constexpr AmdGpu::Sampler GetSharp(const auto& info) const noexcept {
        AmdGpu::Sampler sampler{};
        sharp_fetch.Fetch(info.FlatUserData().data(), &sampler);
        if (post_op == SharpFetchPostOp::DisableAnisoIfSingleLod) {
            const u32 tsharp_dw3 = info.FlatUserData()[post_op_tsharp_dw3_off];
            if (((tsharp_dw3 >> 12) & 0xff) == 0) {
                sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        } else if (post_op == SharpFetchPostOp::ForceRepeatXyzClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ForceLastTexelXyClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ClearAnisoRatioAndThreshold) {
            sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            sampler.aniso_threshold.Assign(0);
        }
        return sampler;
    }
};
using SamplerResourceList = boost::container::static_vector<SamplerResource, NUM_SAMPLERS>;

struct FMaskResource {
    SharpLocation sharp_idx;

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        return info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
    }
};
using FMaskResourceList = boost::container::static_vector<FMaskResource, NUM_FMASKS>;

struct PushData {
    static constexpr u32 XOffsetIndex = 0;
    static constexpr u32 YOffsetIndex = 1;
    static constexpr u32 XScaleIndex = 2;
    static constexpr u32 YScaleIndex = 3;
    static constexpr u32 UdRegsIndex = 4;
    static constexpr u32 BufOffsetIndex = UdRegsIndex + NUM_USER_DATA_REGS / 4;
    static constexpr u32 MotionParamIndex = BufOffsetIndex + NUM_BUFFERS / 16 + 1;
    static constexpr u32 SceneSizeIndex = MotionParamIndex + 1;

    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    std::array<u32, NUM_USER_DATA_REGS> ud_regs;
    std::array<u8, NUM_BUFFERS> buf_offsets;
    u32 motion_param; ///< psport: object motion parameter index (0: off)
    u32 scene_size; ///< reduced width | height << 16; zero means native pixel coordinates

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};
static_assert(sizeof(PushData) <= 128,
              "PushData size is greater than minimum size guaranteed by Vulkan spec");

} // namespace Shader
