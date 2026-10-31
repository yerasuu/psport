// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the per-draw text of frame captures (vk_frame_capture.h): rasterizer state and the
// draw's geometry as the guest memory holds it.

#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"

namespace Vulkan::CaptureDraw {

constexpr u64 NOT_COPIED = ~0ull;

/// A buffer region the GPU reads or writes, copied into the capture's readback buffer in stream
/// order (FrameCapture), so that it holds what the GPU had there and not the CPU's view.
struct GpuRange {
    vk::Buffer h_buffer{};
    u64 off_buffer{};
    u64 cb_copy{};          ///< bytes copied (at most the budget per range)
    u64 cb_full{};          ///< bytes the binding covers
    u64 off_data{NOT_COPIED}; ///< where the copy landed, NOT_COPIED until it did
    std::string skipped;    ///< why it was not copied
};

/// A fetched vertex stream as the draw's bound vertex buffer holds it.
struct GpuAttribute {
    u32 semantic{};
    VAddr va_base{};
    u32 cb_stride{};
    u32 c_records{};
    u32 data_format{};
    u32 number_format{};
    bool is_instanced{};
    GpuRange range;
};

/// What a draw fetches, as the GPU buffers it reads hold it.
struct GpuGeometry {
    std::vector<GpuAttribute> attributes;
    GpuRange indices;
    VAddr va_indices{};
    u32 cb_index{}; ///< 0 for a draw without indices
    u32 c_indices{};
    u32 vertex_offset{};
};

float HalfToFloat(u16 bits);

/// The streams and index buffer of a draw, from the Vulkan buffers it was bound to.
GpuGeometry GpuSources(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline,
                       const FrameCapture::DrawInputs& inputs, u64 cb_range_max);

/// `p_data`: the readback buffer GpuGeometry's ranges were copied into.
std::string DescribeGpuGeometry(const GpuGeometry& geometry, const u8* p_data);

/// Viewport, scissor, clipping and rasterizer state, Vulkan next to the guest registers; the
/// same for consecutive draws that share it.
std::string DescribeRasterState(const AmdGpu::Regs& regs, const DynamicState& dynamic,
                                const GraphicsPipeline& pipeline);

/// Topology, index range and the fetched vertex attributes read from the guest memory (the
/// first vertices when `is_vertex_dump`), and the vertex shader's buffers when
/// `is_constant_dump`. Memory the GPU wrote and the CPU has not seen reads stale.
std::string DescribeGeometry(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline,
                             const FrameCapture::DrawInputs& inputs, bool is_vertex_dump,
                             bool is_constant_dump);

} // namespace Vulkan::CaptureDraw
