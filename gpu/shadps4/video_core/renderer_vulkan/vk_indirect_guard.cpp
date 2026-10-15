// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_indirect_guard.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/indirect_guard_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
struct Push {
    u32 max_count[3];
    u32 first;
    u32 max_total;
    u32 slot;
};

struct Skipped {
    u32 c_skipped;
    u32 pad[3];
    u32 last[3];
};

s64 NowNs() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
} // namespace

bool IndirectGuard::Enabled() {
    static const bool s_is_enabled = [] {
        const char* sz_v = std::getenv("PS_INDIRECT_GUARD");
        return !(sz_v && sz_v[0] == '0');
    }();
    return s_is_enabled;
}

IndirectGuard::IndirectGuard(const Instance& instance_, Scheduler& scheduler_)
    : m_instance{instance_}, m_scheduler{scheduler_} {
    const auto device = m_instance.GetDevice();
    m_checked = std::make_unique<VideoCore::Buffer>(m_instance, 0, NumSlots * 16,
                                                  VideoCore::MemoryType::DeviceLocal,
                                                  "Indirect dispatch counts");
    m_skipped = std::make_unique<VideoCore::Buffer>(m_instance, 0, sizeof(Skipped),
                                                  VideoCore::MemoryType::HostCached,
                                                  "Indirect dispatches skipped");
    std::memset(m_skipped->mapped_data.data(), 0, sizeof(Skipped));
    m_skipped->Flush(0, sizeof(Skipped));

    const auto limits = m_instance.GetPhysicalDevice().getProperties().limits;
    for (u32 i = 0; i < 3; ++i) {
        m_max_count[i] = limits.maxComputeWorkGroupCount[i];
    }
    const char* sz_max = std::getenv("PS_INDIRECT_GUARD_MAX");
    m_max_total = sz_max && *sz_max ? static_cast<u32>(std::strtoul(sz_max, nullptr, 0)) : 1u << 22;

    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    m_set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(Push)};
    m_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*m_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(INDIRECT_GUARD_COMP, device);
    m_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *m_pipeline_layout,
            }));
    device.destroyShaderModule(module);
    m_last_report_ns = NowNs();
}

IndirectGuard::~IndirectGuard() = default;

std::pair<vk::Buffer, u64> IndirectGuard::CheckArgs(const VideoCore::Buffer& args, u64 off_args) {
    // A storage binding starts at a multiple of the device's alignment: the arguments are
    // `first` dwords into it.
    const u64 alignment = m_instance.StorageMinAlignment();
    const u64 off_bind = off_args / alignment * alignment;
    const u32 first = static_cast<u32>((off_args - off_bind) / sizeof(u32));
    const u64 cb_bind = (off_args - off_bind) + 3 * sizeof(u32);
    const u32 slot = m_next_slot.fetch_add(1, std::memory_order_relaxed) % NumSlots;
    const Push push{{m_max_count[0], m_max_count[1], m_max_count[2]}, first, m_max_total, slot * 4};
    m_scheduler.Record([this, source = args.Handle(), off_bind, cb_bind,
                      push](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {source, off_bind, cb_bind},
            {m_checked->Handle(), 0, NumSlots * 16},
            {m_skipped->Handle(), 0, sizeof(Skipped)},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *m_pipeline_layout, 0, writes);
        cmdbuf.pushConstants(*m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch(1, 1, 1);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect |
                            vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead |
                             vk::AccessFlagBits2::eShaderStorageRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
    Report();
    return {m_checked->Handle(), u64(slot) * 16};
}

void IndirectGuard::Report() {
    const s64 now = NowNs();
    s64 last = m_last_report_ns.load(std::memory_order_relaxed);
    if (now - last < 5'000'000'000 ||
        !m_last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    m_skipped->Invalidate(0, sizeof(Skipped));
    const auto* p_skipped = reinterpret_cast<const volatile Skipped*>(m_skipped->mapped_data.data());
    const u32 c_skipped = p_skipped->c_skipped;
    const u32 before = m_c_reported.exchange(c_skipped, std::memory_order_relaxed);
    if (c_skipped != before) {
        std::printf("Indirect dispatches: %u skipped in all (garbage group counts, the last "
                    "%ux%ux%u; PS_INDIRECT_GUARD)\n",
                    c_skipped, p_skipped->last[0], p_skipped->last[1], p_skipped->last[2]);
    }
}

} // namespace Vulkan
