// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture_draw.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

#include "core/memory.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan::CaptureDraw {

namespace {

constexpr u32 C_INDICES_SCANNED_MAX = 65536;
constexpr u64 CB_VERTICES_READ_MAX = 4ull << 20;
constexpr u32 C_ATTRIBUTES_MAX = 4;
constexpr u32 C_VERTICES_DUMPED = 8;
constexpr u32 C_INDICES_SHOWN = 6;
constexpr u32 C_BUFFERS_DUMPED = 4;
constexpr u64 CB_BUFFER_DUMPED = 256;

/// Guest bytes the GPU would read, through the backing view: a plain read of a page the GPU
/// wrote faults into a readback that waits for this very thread (PS_READBACKS=2).
bool ReadGuest(VAddr va_source, u64 cb_read, std::vector<u8>& out_bytes) {
    auto* p_memory = Core::Memory::Instance();
    if (!va_source || !cb_read || !p_memory->IsValidGpuMapping(va_source, cb_read) ||
        p_memory->ClampRangeSize(va_source, cb_read) < cb_read) {
        return false;
    }
    out_bytes.resize(cb_read);
    p_memory->CopySparseMemory(va_source, out_bytes.data(), cb_read);
    return true;
}

struct VertexFormat {
    u32 c_components{};
    u32 cb_component{};
};

VertexFormat VertexFormatOf(AmdGpu::DataFormat format) {
    using D = AmdGpu::DataFormat;
    switch (format) {
    case D::Format8:
        return {1, 1};
    case D::Format8_8:
        return {2, 1};
    case D::Format8_8_8_8:
        return {4, 1};
    case D::Format16:
        return {1, 2};
    case D::Format16_16:
        return {2, 2};
    case D::Format16_16_16_16:
        return {4, 2};
    case D::Format32:
        return {1, 4};
    case D::Format32_32:
        return {2, 4};
    case D::Format32_32_32:
        return {3, 4};
    case D::Format32_32_32_32:
        return {4, 4};
    default:
        return {};
    }
}

float DecodeComponent(const u8* p_component, u32 cb_component, AmdGpu::NumberFormat number) {
    using N = AmdGpu::NumberFormat;
    u32 bits = 0;
    std::memcpy(&bits, p_component, cb_component);
    const u32 c_bits = cb_component * 8;
    const bool is_signed = number == N::Snorm || number == N::Sint || number == N::Sscaled ||
                           number == N::SnormNz;
    const s64 value_signed =
        c_bits == 32 ? s64(s32(bits)) : s64(s32(bits << (32 - c_bits)) >> (32 - c_bits));
    const double value = is_signed ? double(value_signed) : double(bits);
    switch (number) {
    case N::Float:
        return cb_component == 4   ? std::bit_cast<float>(bits)
               : cb_component == 2 ? HalfToFloat(u16(bits))
                                   : float(bits);
    case N::Unorm:
        return float(value / double((1ull << c_bits) - 1));
    case N::Snorm:
    case N::SnormNz:
        return float(std::max(value / double((1ull << (c_bits - 1)) - 1), -1.0));
    default:
        return float(value);
    }
}

std::string FormatVector(const std::array<float, 4>& values, u32 c_components) {
    std::string text = "(";
    for (u32 c = 0; c < c_components; ++c) {
        text += std::format("{}{:.6g}", c ? " " : "", values[c]);
    }
    return text + ")";
}

const char* PrimitiveName(AmdGpu::PrimitiveType type) {
    using P = AmdGpu::PrimitiveType;
    switch (type) {
    case P::None:
        return "None";
    case P::PointList:
        return "PointList";
    case P::LineList:
        return "LineList";
    case P::LineStrip:
        return "LineStrip";
    case P::TriangleList:
        return "TriangleList";
    case P::TriangleFan:
        return "TriangleFan";
    case P::TriangleStrip:
        return "TriangleStrip";
    case P::PatchPrimitive:
        return "Patch";
    case P::RectList:
        return "RectList";
    case P::QuadList:
        return "QuadList";
    case P::QuadStrip:
        return "QuadStrip";
    case P::Polygon:
        return "Polygon";
    default:
        return "other";
    }
}

/// Index statistics and the indices (restart values left out) into `out_indices`.
std::string ScanIndices(const u8* p_bytes, u32 c_scanned, u32 c_total, u32 cb_index,
                        std::vector<u32>& out_indices) {
    out_indices.clear();
    const u32 restart = cb_index == 2 ? 0xffffu : 0xffffffffu;
    u32 min_index = ~0u, max_index = 0, c_restarts = 0;
    for (u32 i = 0; i < c_scanned; ++i) {
        u32 index = 0;
        std::memcpy(&index, p_bytes + u64(i) * cb_index, cb_index);
        if (index == restart) {
            ++c_restarts;
            continue;
        }
        min_index = std::min(min_index, index);
        max_index = std::max(max_index, index);
        out_indices.push_back(index);
    }
    std::string text = " first";
    for (u32 i = 0; i < std::min<size_t>(out_indices.size(), C_INDICES_SHOWN); ++i) {
        text += std::format(" {}", out_indices[i]);
    }
    text += " last";
    for (size_t i = out_indices.size() > C_INDICES_SHOWN ? out_indices.size() - C_INDICES_SHOWN : 0;
         i < out_indices.size(); ++i) {
        text += std::format(" {}", out_indices[i]);
    }
    if (out_indices.empty()) {
        return text + std::format(" | all {} restart\n", c_restarts);
    }
    return text + std::format(" | range {}..{} restart values {}{}\n", min_index, max_index,
                              c_restarts,
                              c_scanned < c_total ? std::format(" (first {} scanned)", c_scanned)
                                                  : std::string{});
}

std::string DescribeIndices(const FrameCapture::DrawInputs& inputs, std::vector<u32>& out_indices) {
    out_indices.clear();
    if (!inputs.cb_index) {
        const u32 c_vertices = std::min(inputs.c_indices, C_INDICES_SCANNED_MAX);
        for (u32 i = 0; i < c_vertices; ++i) {
            out_indices.push_back(i);
        }
        return std::format("      vertices {}..{} (no indices)\n", inputs.vertex_offset,
                           u64(inputs.vertex_offset) + inputs.c_indices);
    }
    const u32 c_scanned = std::min(inputs.c_indices, C_INDICES_SCANNED_MAX);
    std::vector<u8> bytes;
    if (!ReadGuest(inputs.va_indices, u64(c_scanned) * inputs.cb_index, bytes)) {
        return std::format("      indices guest at {:#x} not readable\n", inputs.va_indices);
    }
    return std::format("      indices guest u{} at {:#x}:", inputs.cb_index * 8,
                       inputs.va_indices) +
           ScanIndices(bytes.data(), c_scanned, inputs.c_indices, inputs.cb_index, out_indices);
}

/// Range, non-finite and out-of-records counts of one stream over the draw's vertices, from
/// `p_bytes`, which holds cb_bytes of the stream from vertex `first_vertex` on.
std::string ScanVertices(const u8* p_bytes, u64 cb_bytes, u64 first_vertex, u32 cb_stride,
                         u32 c_records, AmdGpu::DataFormat data_format,
                         AmdGpu::NumberFormat number_format, const std::vector<u32>& indices,
                         u32 vertex_offset, bool is_vertex_dump) {
    const VertexFormat format = VertexFormatOf(data_format);
    const u32 cb_element = format.c_components * format.cb_component;
    std::array<float, 4> min_values, max_values;
    min_values.fill(INFINITY);
    max_values.fill(-INFINITY);
    u32 c_outside_records = 0, c_nonfinite = 0, c_outside_copy = 0;
    std::string dump;
    for (size_t i = 0; i < indices.size(); ++i) {
        const u64 vertex = u64(indices[i]) + vertex_offset;
        c_outside_records += vertex >= c_records ? 1 : 0;
        const u64 off_element = (vertex - first_vertex) * cb_stride;
        if (vertex < first_vertex || off_element + cb_element > cb_bytes) {
            ++c_outside_copy;
            continue;
        }
        std::array<float, 4> values{};
        for (u32 c = 0; c < format.c_components; ++c) {
            values[c] = DecodeComponent(p_bytes + off_element + c * format.cb_component,
                                        format.cb_component, number_format);
            if (!std::isfinite(values[c])) {
                ++c_nonfinite;
                continue;
            }
            min_values[c] = std::min(min_values[c], values[c]);
            max_values[c] = std::max(max_values[c], values[c]);
        }
        if (is_vertex_dump && i < C_VERTICES_DUMPED) {
            dump += std::format(" v{} {}", vertex, FormatVector(values, format.c_components));
        }
    }
    std::string text = std::format(" | min {} max {} nonfinite {} beyond records {}",
                                   FormatVector(min_values, format.c_components),
                                   FormatVector(max_values, format.c_components), c_nonfinite,
                                   c_outside_records);
    if (c_outside_copy) {
        text += std::format(" beyond copy {}", c_outside_copy);
    }
    text += "\n";
    if (!dump.empty()) {
        text += std::format("        first:{}\n", dump);
    }
    return text;
}

std::string DescribeAttribute(const Shader::Gcn::VertexAttribute& attribute,
                              const Shader::Info& vs_info, const FrameCapture::DrawInputs& inputs,
                              const std::vector<u32>& indices, bool is_vertex_dump) {
    const auto sharp = attribute.GetSharp(vs_info);
    const auto data_format = sharp.GetDataFmt();
    const auto number_format = sharp.GetNumberFmt();
    const u32 cb_stride = sharp.GetStride();
    std::string text = std::format(
        "      attr guest semantic {} vgpr {} x{} step {} V# {:#x} stride {} records {} format "
        "{}/{}",
        attribute.semantic, attribute.dest_vgpr, attribute.num_elements,
        u32(attribute.GetStepRate()), u64(sharp.base_address), cb_stride, sharp.num_records,
        u32(data_format), u32(number_format));
    const VertexFormat format = VertexFormatOf(data_format);
    if (!format.c_components) {
        return text + " (format not decoded)\n";
    }
    if (attribute.GetStepRate() != Shader::Gcn::VertexAttribute::None) {
        return text + " (per instance, not scanned)\n";
    }
    if (indices.empty()) {
        return text + "\n";
    }
    const u32 cb_element = format.c_components * format.cb_component;
    const auto [it_min, it_max] = std::ranges::minmax_element(indices);
    const u64 first_vertex = u64(*it_min) + inputs.vertex_offset;
    const u64 last_vertex = u64(*it_max) + inputs.vertex_offset;
    const u64 cb_span = (last_vertex - first_vertex) * cb_stride + cb_element;
    std::vector<u8> bytes;
    if (cb_span > CB_VERTICES_READ_MAX ||
        !ReadGuest(sharp.base_address + first_vertex * cb_stride, cb_span, bytes)) {
        return text + std::format(" | vertices {}..{} not readable\n", first_vertex, last_vertex);
    }
    return text + ScanVertices(bytes.data(), bytes.size(), first_vertex, cb_stride,
                               sharp.num_records, data_format, number_format, indices,
                               inputs.vertex_offset, is_vertex_dump);
}

std::string DescribeBuffers(const Shader::Info& vs_info) {
    std::string text;
    u32 c_dumped = 0;
    for (u32 i_slot = 0; i_slot < vs_info.buffers.size() && c_dumped < C_BUFFERS_DUMPED;
         ++i_slot) {
        const auto& resource = vs_info.buffers[i_slot];
        if (resource.IsSpecial() || resource.is_written) {
            continue;
        }
        const auto sharp = resource.GetSharp(vs_info);
        std::vector<u8> bytes;
        const u64 cb_read = std::min<u64>(sharp.GetSize(), CB_BUFFER_DUMPED) & ~u64(3);
        if (!sharp.Valid() || !ReadGuest(sharp.base_address, cb_read, bytes)) {
            continue;
        }
        ++c_dumped;
        text += std::format("      vs buffer slot {} at {:#x} size {}:", i_slot,
                            u64(sharp.base_address), sharp.GetSize());
        for (u64 i = 0; i < cb_read / 4; ++i) {
            float value;
            std::memcpy(&value, bytes.data() + i * 4, 4);
            text += std::format("{}{:12.6g}", i % 8 == 0 ? std::format("\n        [{:3}]", i) : "",
                                value);
        }
        text += "\n";
    }
    return text;
}

} // namespace

float HalfToFloat(u16 bits) {
    const u32 exponent = (bits >> 10) & 0x1f;
    const u32 mantissa = bits & 0x3ff;
    float value;
    if (exponent == 0) {
        value = std::ldexp(float(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? NAN : INFINITY;
    } else {
        value = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    }
    return (bits & 0x8000) ? -value : value;
}

std::string DescribeRasterState(const AmdGpu::Regs& regs, const DynamicState& dynamic,
                                const GraphicsPipeline& pipeline) {
    std::string text = std::format("      viewport vk count {}", dynamic.viewports.size());
    if (!dynamic.viewports.empty()) {
        const auto& v = dynamic.viewports[0];
        text += std::format(" [0] x {} y {} w {} h {} depth {}..{}", v.x, v.y, v.width, v.height,
                            v.minDepth, v.maxDepth);
    }
    if (!dynamic.scissors.empty()) {
        const auto& s = dynamic.scissors[0];
        text += std::format(" | scissor vk [0] {},{} {}x{}", s.offset.x, s.offset.y,
                            s.extent.width, s.extent.height);
    }
    const auto& vp = regs.viewports[0];
    const auto& vp_depth = regs.viewport_depths[0];
    const auto& vp_scissor = regs.viewport_scissors[0];
    const auto scissor = regs.GetCombinedScissor();
    const auto& vc = regs.viewport_control;
    const auto& cc = regs.clipper_control;
    text += std::format(
        "\n      viewport guest [0] xscale {} xoffset {} yscale {} yoffset {} zscale {} zoffset {} "
        "enables x {}{} y {}{} z {}{} transformed {}{}{} depth range {}..{} | scissor combined "
        "{},{}-{},{} viewport scissor {} [0] {},{}-{},{}\n",
        vp.xscale, vp.xoffset, vp.yscale, vp.yoffset, vp.zscale, vp.zoffset, u32(vc.xscale_enable),
        u32(vc.xoffset_enable), u32(vc.yscale_enable), u32(vc.yoffset_enable),
        u32(vc.zscale_enable), u32(vc.zoffset_enable), u32(vc.xy_transformed),
        u32(vc.z_transformed), u32(vc.w_transformed), vp_depth.zmin, vp_depth.zmax,
        scissor.top_left_x, scissor.top_left_y, scissor.bottom_right_x, scissor.bottom_right_y,
        u32(regs.mode_control.vport_scissor_enable), u32(vp_scissor.top_left_x),
        u32(vp_scissor.top_left_y), u32(vp_scissor.bottom_right_x),
        u32(vp_scissor.bottom_right_y));
    text += std::format(
        "      clip guest disable {} (effective {}) space {} zclip near off {} far off {} user "
        "planes {:#x} dx raster kill {} vtx kill or {}\n",
        u32(cc.clip_disable), u32(regs.IsClipDisabled()),
        cc.clip_space == AmdGpu::ClipSpace::MinusWToW ? "-w..w" : "0..w",
        u32(cc.zclip_near_disable), u32(cc.zclip_far_disable), u32(cc.user_clip_plane_enable),
        u32(cc.dx_rasterization_kill), u32(cc.vtx_kill_or));
    const auto& key = pipeline.GetGraphicsKey();
    const auto& pc = regs.polygon_control;
    text += std::format(
        "      raster vk cull {} front {} discard {} restart {} polygon mode {} depth clip {} "
        "clamp {} bias {} ({} clamp {} slope {}) | guest cull front {} back {} front face {} "
        "polygon mode enable {} {}/{} offset enable front {} back {} para {} | primitive restart "
        "{} index {:#x}\n",
        vk::to_string(dynamic.cull_mode), vk::to_string(dynamic.front_face),
        u32(dynamic.rasterizer_discard_enable), u32(dynamic.primitive_restart_enable),
        u32(key.polygon_mode), u32(key.depth_clip_enable), u32(key.depth_clamp_enable),
        u32(dynamic.depth_bias_enabled), dynamic.depth_bias_constant, dynamic.depth_bias_clamp,
        dynamic.depth_bias_slope, u32(pc.cull_front), u32(pc.cull_back), u32(pc.front_face),
        u32(pc.enable_polygon_mode), u32(pc.polygon_mode_front), u32(pc.polygon_mode_back),
        u32(pc.enable_polygon_offset_front), u32(pc.enable_polygon_offset_back),
        u32(pc.enable_polygon_offset_para), regs.enable_primitive_restart,
        regs.primitive_restart_index);
    return text;
}

std::string DescribeGeometry(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline,
                             const FrameCapture::DrawInputs& inputs, bool is_vertex_dump,
                             bool is_constant_dump) {
    std::string text = std::format(
        "      {} ({}) count {} instances {} base vertex {} first instance {}\n",
        PrimitiveName(regs.primitive_type),
        vk::to_string(LiverpoolToVK::PrimitiveType(regs.primitive_type)), inputs.c_indices,
        inputs.c_instances, inputs.vertex_offset, inputs.first_instance);
    std::vector<u32> indices;
    text += DescribeIndices(inputs, indices);
    const auto& vs_info = pipeline.GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline.GetFetchShader();
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        text += "      no fetch shader attributes (the vertex shader reads its own buffers)\n";
    } else {
        u32 c_described = 0;
        for (const auto& attribute : fetch_shader->attributes) {
            if (c_described++ == C_ATTRIBUTES_MAX) {
                text += std::format("      {} more attributes\n",
                                    fetch_shader->attributes.size() - C_ATTRIBUTES_MAX);
                break;
            }
            text += DescribeAttribute(attribute, vs_info, inputs, indices, is_vertex_dump);
        }
    }
    if (is_constant_dump) {
        text += DescribeBuffers(vs_info);
    }
    return text;
}

GpuGeometry GpuSources(const AmdGpu::Regs& regs, const GraphicsPipeline& pipeline,
                       const FrameCapture::DrawInputs& inputs, u64 cb_range_max) {
    GpuGeometry geometry{.va_indices = inputs.va_indices,
                         .cb_index = inputs.cb_index,
                         .c_indices = inputs.c_indices,
                         .vertex_offset = inputs.vertex_offset};
    const auto bound = [cb_range_max](vk::Buffer h_buffer, u64 off_buffer, u64 cb_full) {
        GpuRange range{.h_buffer = h_buffer,
                       .off_buffer = off_buffer,
                       .cb_copy = std::min(cb_full, cb_range_max) & ~u64(3),
                       .cb_full = cb_full};
        if (!h_buffer) {
            range.skipped = "no buffer bound";
        } else if (!range.cb_copy) {
            range.skipped = "empty binding";
        }
        return range;
    };
    if (inputs.cb_index) {
        geometry.indices = bound(inputs.index_buffer, inputs.off_index_buffer,
                                 std::min(inputs.cb_index_buffer,
                                          u64(inputs.c_indices) * inputs.cb_index));
    }
    const auto& fetch_shader = pipeline.GetFetchShader();
    if (!fetch_shader) {
        return geometry;
    }
    const auto& vs_info = pipeline.GetStage(Shader::SwStage::Vertex);
    for (size_t i = 0; i < fetch_shader->attributes.size() && i < C_ATTRIBUTES_MAX; ++i) {
        const auto& attribute = fetch_shader->attributes[i];
        const auto sharp = attribute.GetSharp(vs_info);
        GpuAttribute stream{
            .semantic = attribute.semantic,
            .va_base = sharp.base_address,
            .cb_stride = sharp.GetStride(),
            .c_records = sharp.num_records,
            .data_format = u32(sharp.GetDataFmt()),
            .number_format = u32(sharp.GetNumberFmt()),
            .is_instanced = attribute.GetStepRate() != Shader::Gcn::VertexAttribute::None,
        };
        if (i < inputs.vertex_buffers.size() && i < inputs.vertex_offsets.size() &&
            i < inputs.vertex_sizes.size()) {
            // The bound buffer covers the stream only as far as the game's memory is mapped
            // (Rasterizer::ResolveVertexBuffers); a copy past that reads outside it.
            const u64 cb_mapped = sharp.base_address
                                      ? Core::Memory::Instance()->ClampRangeSize(
                                            sharp.base_address, inputs.vertex_sizes[i])
                                      : 0;
            stream.range = bound(inputs.vertex_buffers[i], inputs.vertex_offsets[i],
                                 std::min<u64>(inputs.vertex_sizes[i], cb_mapped));
        } else {
            stream.range.skipped = "no vertex buffer bound for the stream";
        }
        geometry.attributes.push_back(std::move(stream));
    }
    return geometry;
}

std::string DescribeGpuGeometry(const GpuGeometry& geometry, const u8* p_data) {
    std::string text;
    std::vector<u32> indices;
    const auto copied = [](const GpuRange& range) {
        return range.off_data == NOT_COPIED
                   ? std::format("not copied ({})",
                                 range.skipped.empty() ? "the pass was not summarized"
                                                       : range.skipped)
                   : std::format("{} of {} B", range.cb_copy, range.cb_full);
    };
    if (!geometry.cb_index) {
        const u32 c_vertices = std::min(geometry.c_indices, C_INDICES_SCANNED_MAX);
        for (u32 i = 0; i < c_vertices; ++i) {
            indices.push_back(i);
        }
    } else if (geometry.indices.off_data == NOT_COPIED || !p_data) {
        text += std::format("      indices gpu {}\n", copied(geometry.indices));
    } else {
        const u32 c_scanned = u32(std::min<u64>(geometry.indices.cb_copy / geometry.cb_index,
                                                C_INDICES_SCANNED_MAX));
        text += std::format("      indices gpu u{} ({}):", geometry.cb_index * 8,
                            copied(geometry.indices)) +
                ScanIndices(p_data + geometry.indices.off_data, c_scanned, geometry.c_indices,
                            geometry.cb_index, indices);
    }
    for (const auto& stream : geometry.attributes) {
        text += std::format("      attr gpu semantic {} V# {:#x} stride {} ({})", stream.semantic,
                            stream.va_base, stream.cb_stride, copied(stream.range));
        const auto data_format = AmdGpu::DataFormat(stream.data_format);
        if (stream.range.off_data == NOT_COPIED || !p_data) {
            text += "\n";
        } else if (!VertexFormatOf(data_format).c_components) {
            text += " (format not decoded)\n";
        } else if (stream.is_instanced) {
            text += " (per instance, not scanned)\n";
        } else {
            text += ScanVertices(p_data + stream.range.off_data, stream.range.cb_copy, 0,
                                 stream.cb_stride, stream.c_records, data_format,
                                 AmdGpu::NumberFormat(stream.number_format), indices,
                                 geometry.vertex_offset, true);
        }
    }
    return text;
}

} // namespace Vulkan::CaptureDraw
