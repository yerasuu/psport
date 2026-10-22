// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "psport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"

#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 micro_tiled_mips;
    u32 num_mips;
    std::array<ImageInfo::MipInfo, 16> mips;
};

TileManager::TileManager(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, StreamBuffer& stream_buffer_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, stream_buffer{stream_buffer_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    desc_layout = Vulkan::Check<"create the tiling descriptor set layout">(
        device.createDescriptorSetLayoutUnique(desc_layout_ci));
    if (!desc_layout) {
        Vulkan::ReportInitFailure();
        return;
    }

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    pl_layout = Vulkan::Check<"create the tiling pipeline layout">(
        device.createPipelineLayoutUnique(layout_info));
    if (!pl_layout) {
        Vulkan::ReportInitFailure();
    }
}

TileManager::~TileManager() = default;

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const TilingKey key{
        .tile_mode = info.tile_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .is_tiler = is_tiler,
    };
    if (const auto it = tiling_pipelines.find(key); it != tiling_pipelines.end()) {
        return *it->second;
    }
    if (!pl_layout) {
        return {};
    }
    // An unknown tile mode was already reported and handled as linear: nothing to detile.
    if (info.array_mode == AmdGpu::ArrayMode::ArrayLinearAligned ||
        info.array_mode == AmdGpu::ArrayMode::ArrayLinearGeneral) {
        return {};
    }

    const auto device = instance.GetDevice();
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool is_macro = AmdGpu::IsMacroTiled(info.array_mode);
    std::array<u32, 12> spec_data{
        info.num_samples,
        u32(micro_tile_mode),
        AmdGpu::GetMicroTileThickness(info.array_mode),
        u32(is_tiler),
    };
    if (is_macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        spec_data[4] = u32(info.array_mode);
        spec_data[5] = u32(AmdGpu::GetPipeConfig(info.tile_mode));
        spec_data[6] = AmdGpu::GetBankWidth(macro_tile_mode);
        spec_data[7] = AmdGpu::GetBankHeight(macro_tile_mode);
        spec_data[8] = AmdGpu::GetNumBanks(macro_tile_mode);
        spec_data[9] = std::bit_width(spec_data[8]) - 1;
        spec_data[10] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                                   info.num_bits);
        spec_data[11] = AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }

    std::span<const u32> code;
    switch (info.num_bits) {
    case 8:
        code = is_macro ? std::span<const u32>{TILING_MACRO_8_COMP}
                        : std::span<const u32>{TILING_MICRO_8_COMP};
        break;
    case 16:
        code = is_macro ? std::span<const u32>{TILING_MACRO_16_COMP}
                        : std::span<const u32>{TILING_MICRO_16_COMP};
        break;
    case 32:
        code = is_macro ? std::span<const u32>{TILING_MACRO_32_COMP}
                        : std::span<const u32>{TILING_MICRO_32_COMP};
        break;
    case 64:
        code = is_macro ? std::span<const u32>{TILING_MACRO_64_COMP}
                        : std::span<const u32>{TILING_MICRO_64_COMP};
        break;
    case 96:
        code = is_macro ? std::span<const u32>{TILING_MACRO_96_COMP}
                        : std::span<const u32>{TILING_MICRO_96_COMP};
        break;
    case 128:
        code = is_macro ? std::span<const u32>{TILING_MACRO_128_COMP}
                        : std::span<const u32>{TILING_MICRO_128_COMP};
        break;
    default:
        LOG_ERROR_COUNTED(Render_Vulkan, "Unsupported tiling pixel width {}, image left as is",
                          info.num_bits);
        tiling_pipelines.emplace(key, vk::UniquePipeline{});
        return {};
    }

    const auto module = Vulkan::CompileSPV(code, device);
    static constexpr auto spec_entries = [] {
        std::array<vk::SpecializationMapEntry, 12> entries{};
        for (u32 i = 0; i < entries.size(); ++i) {
            entries[i] = vk::SpecializationMapEntry{i, i * u32(sizeof(u32)), sizeof(u32)};
        }
        return entries;
    }();

    const u32 spec_count = is_macro ? 12 : 4;
    const vk::SpecializationInfo specialization{
        .mapEntryCount = spec_count,
        .pMapEntries = spec_entries.data(),
        .dataSize = spec_count * sizeof(u32),
        .pData = spec_data.data(),
    };

    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Creating tiling pipeline {}", module_name);
    Vulkan::SetObjectName(device, module, module_name);

    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
        .pSpecializationInfo = &specialization,
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };

    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    device.destroyShaderModule(module);
    if (result != vk::Result::eSuccess) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Tiling pipeline {} creation failed, image left as is: {}",
                          module_name, vk::to_string(result));
        tiling_pipelines.emplace(key, vk::UniquePipeline{});
        return {};
    }
    const auto handle = *pipeline;
    tiling_pipelines.emplace(key, std::move(pipeline));
    return handle;
}

void TileManager::RecordTilingDispatch(vk::Pipeline pipeline, const vk::DescriptorBufferInfo& tiled,
                                       const vk::DescriptorBufferInfo& linear,
                                       const vk::DescriptorBufferInfo& params, u32 dim_x) {
    scheduler.RecordCrumb({.sz_name = "tiling"}, [pipeline, layout = *pl_layout, tiled, linear,
                                              params, dim_x](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &tiled,
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &linear,
            },
            {
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo = &params,
            },
        }};
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, set_writes);
        cmdbuf.dispatch(dim_x, 1, 1);
    });
}

std::pair<const Buffer*, u64> TileManager::DetileImage(const VideoCore::Buffer* in_buffer,
                                                       u64 in_offset, const ImageInfo& info) {
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }
    const vk::Pipeline pipeline = GetTilingPipeline(info, false);
    if (!pipeline) {
        return {in_buffer, in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_tiled_mips = info.micro_tiled_mips;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const auto off_params = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment());
    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);
    if (!off_params || !staging.buffer) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Image at {:#x} not uploaded: no memory to detile it",
                          info.guest_address);
        return {nullptr, 0};
    }
    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = *off_params,
        .range = sizeof(params),
    };

    scheduler.EndRendering();
    runtime.FlushBarriers();

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer->Handle(),
        .offset = in_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
        .range = info.guest_size,
    };
    const auto dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    // psport: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(pipeline, tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, dim_x);
    // psport: the source may be the arena, which later uploads write: a copy into
    // it must wait for this read.
    runtime.AccessBuffer(in_buffer, in_offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);

    runtime.AccessBuffer(staging.buffer, staging.offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);

    return {staging.buffer, staging.offset};
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            const VideoCore::Buffer* out_buffer, u64 out_offset,
                            bool is_host_destination) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        runtime.DownloadImage(&in_image, out_buffer, buffer_copies);
        return;
    }
    const vk::Pipeline pipeline = GetTilingPipeline(info, true);
    if (!pipeline) {
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_tiled_mips = info.micro_tiled_mips;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const auto off_params = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment());
    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);
    if (!off_params || !staging.buffer) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Image at {:#x} not written back: no memory to tile it",
                          info.guest_address);
        return;
    }
    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = *off_params,
        .range = sizeof(params),
    };
    for (auto& copy : buffer_copies) {
        copy.bufferOffset += staging.offset;
    }

    runtime.DownloadImage(&in_image, staging.buffer, buffer_copies);
    runtime.FlushBarriers();

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer->Handle(),
        .offset = out_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
        .range = info.guest_size,
    };
    const u64 c_texels = info.guest_size / (info.num_bits / 8);
    const auto dim_x = static_cast<u32>(c_texels / 64);
    // psport: the tiler writes a texel at a time at swizzled addresses; into system memory each
    // write crossed the bus on its own (bbport 8185b88, PR #210: 17.0 to 10.4 ms of GPU a frame on
    // an RTX 5090). Tiled into VRAM and copied over in one transfer instead, only where the tiler
    // writes every byte of the copy: all levels back to back over guest_size and whole workgroups,
    // otherwise the bytes it leaves alone would be overwritten.
    u64 cb_covered = 0;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        const auto& mip_info = info.mips_layout[mip];
        cb_covered = mip_info.offset == cb_covered ? cb_covered + mip_info.size : ~0ULL;
    }
    const auto local = is_host_destination && cb_covered == info.guest_size && c_texels % 64 == 0 &&
                               !PsToggle::Disabled(PsToggle::LocalTiling)
                           ? runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal)
                           : Vulkan::StagingBufferRef{};
    if (local.buffer) {
        const vk::DescriptorBufferInfo local_buffer_info{
            .buffer = local.buffer->Handle(),
            .offset = local.offset,
            .range = info.guest_size,
        };
        RecordTilingDispatch(pipeline, local_buffer_info, linear_buffer_info, params_buffer_info,
                             dim_x);
        runtime.AccessBuffer(local.buffer, local.offset, info.guest_size,
                             vk::PipelineStageFlagBits2::eComputeShader,
                             vk::AccessFlagBits2::eShaderWrite);
        const vk::BufferCopy copy{
            .srcOffset = local.offset, .dstOffset = out_offset, .size = info.guest_size};
        runtime.CopyBuffer(local.buffer, out_buffer, {&copy, 1});
        PsStats::g_c_local_tilings.fetch_add(1, std::memory_order_relaxed);
        PsStats::g_cb_local_tiling.fetch_add(info.guest_size, std::memory_order_relaxed);
        return;
    }
    // psport: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(pipeline, tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, dim_x);

    runtime.AccessBuffer(out_buffer, out_offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
}

} // namespace VideoCore
