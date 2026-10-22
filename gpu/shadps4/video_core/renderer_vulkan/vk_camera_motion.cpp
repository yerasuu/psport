// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_camera_motion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "psport_toggles.h"

#include "video_core/host_shaders/camera_motion_comp.h"
#include "video_core/host_shaders/camera_motion_debug_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct PushConstants {
    std::array<float, 12> reproject;
    std::array<float, 4> proj;
    std::array<float, 4> prev_proj;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> previous_jitter;
    u32 mode;
};

/// a * b for 3x4 affine matrices (rows [R | t]).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

/// The inverse of a 3x4 affine matrix [M | t]: [M^-1 | -M^-1 t].
std::array<float, 12> InverseAffine(const std::array<float, 12>& m) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12f) {
        return m;
    }
    const float s = 1.0f / det;
    const std::array<float, 9> r = {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s,
                                    (f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s,
                                    (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s};
    std::array<float, 12> out{};
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 0] = r[row * 3 + 0];
        out[row * 4 + 1] = r[row * 3 + 1];
        out[row * 4 + 2] = r[row * 3 + 2];
        out[row * 4 + 3] = -(r[row * 3 + 0] * m[3] + r[row * 3 + 1] * m[7] + r[row * 3 + 2] * m[11]);
    }
    return out;
}

} // namespace

CameraMotion::CameraMotion(const Instance& instance_, Scheduler& scheduler_,
                           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : m_instance{instance_}, m_scheduler{scheduler_}, m_texture_cache{texture_cache_}, m_runtime{runtime_},
      m_constants{GetUpscalerPlacement().camera} {
    const char* sz_env = std::getenv("PS_DEBUG_MOTION");
    m_has_debug_overlay = sz_env && sz_env[0] == '1';
    // The upscaler can be switched on from the menu at any time: the camera is always tracked
    // while it is available.
    m_is_for_upscaler = IsUpscalerAvailable();
    const auto device = m_instance.GetDevice();
    if (m_is_for_upscaler) {
        const std::array<vk::DescriptorSetLayoutBinding, 3> motion_bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 2,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        m_motion_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(motion_bindings.size()),
            .pBindings = motion_bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(PushConstants)};
        m_motion_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*m_motion_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto motion_module = CompileSPV(CAMERA_MOTION_COMP, device);
        m_motion_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = motion_module,
                              .pName = "main"},
                    .layout = *m_motion_pipeline_layout,
                }));
        device.destroyShaderModule(motion_module);
    }
    if (!m_has_debug_overlay) {
        return;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    m_desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    m_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*m_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    }));
    const auto module = CompileSPV(CAMERA_MOTION_DEBUG_COMP, device);
    m_overlay_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *m_pipeline_layout,
            }));
    device.destroyShaderModule(module);
    for (auto& frame : m_frames) {
        frame = std::make_unique<VideoCore::Buffer>(m_instance, 0, 3840ull * 2160 * 4,
                                                    VideoCore::MemoryType::DeviceLocal);
    }
    std::printf("GPU: camera motion debug overlay on\n");
}

CameraMotion::~CameraMotion() = default;

float CameraMotion::VerticalFov() const noexcept {
    return 2.0f * std::atan(1.0f / std::abs(m_current.proj[1]));
}

float CameraMotion::Near() const noexcept {
    // depth = zs + zo / z is 0 at the near plane.
    return -m_current.proj[3] / m_current.proj[2];
}

void CameraMotion::PrintState(int frame) const {
    const auto print = [](const char* sz_name, const float* p_values, int c_values) {
        std::printf(" %s", sz_name);
        for (int i = 0; i < c_values; ++i) {
            std::printf(" %.9g", p_values[i]);
        }
    };
    std::printf("Dump camera: frame %d", frame);
    print("view", m_current.view.data(), 12);
    print("prev_view", m_previous.view.data(), 12);
    print("proj", m_current.proj.data(), 4);
    print("prev_proj", m_previous.proj.data(), 4);
    print("jitter", m_jitter.data(), 2);
    print("prev_jitter", m_previous_jitter.data(), 2);
    std::printf("\n");
}

std::array<std::array<float, 4>, 3> CameraMotion::TaaDepthParameters() const noexcept {
    const auto transform = Multiply(m_previous.view, m_current.inv_view);
    return {m_current.proj, m_previous.proj,
            {transform[8], transform[9], transform[10], transform[11]}};
}

vk::ImageView CameraMotion::ObjectMotionView() const noexcept {
    return m_p_object_motion && m_p_object_motion->Enabled() ? m_p_object_motion->View() : vk::ImageView{};
}

vk::Image CameraMotion::ObjectMotionImage(u32 width, u32 height) const noexcept {
    return m_p_object_motion && m_p_object_motion->Enabled() ? m_p_object_motion->Image(width, height)
                                                      : vk::Image{};
}

void CameraMotion::RecordMotion(vk::ImageView depth_view, vk::ImageView motion_view, u32 width,
                                u32 height) {
    CommitFrameCamera();
    bool is_object_valid = false;
    const vk::ImageView object_view = m_p_object_motion && m_p_object_motion->Enabled()
        ? m_p_object_motion->PrepareRead(width, height, is_object_valid) : depth_view;
    const PushConstants push{
        .reproject = Multiply(m_previous.view, m_current.inv_view),
        .proj = m_current.proj,
        .prev_proj = m_previous.proj,
        .size = {float(width), float(height)},
        .jitter = m_jitter,
        .previous_jitter = m_previous_jitter,
        .mode = is_object_valid ? 1u : 0u,
    };
    // psport: everything the pass reads is captured: it may be recorded on a recording thread
    // while this thread goes on with the next frame's camera.
    m_scheduler.RecordCrumb({.sz_name = "camera motion"}, [push, depth_view, motion_view, object_view, width, height,
                      pipeline = *m_motion_pipeline,
                      layout = *m_motion_pipeline_layout](vk::CommandBuffer cmdbuf) {
        const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo object_info{.imageView = object_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &depth_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &motion_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &object_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
    });
}

void CameraMotion::OnConstants(const float* p_data) {
    const float* p_far = p_data + m_constants.off_dw_far;
    const float* p_render_size = p_data + m_constants.off_dw_render_size;
    if (p_far[0] != m_constants.far_plane || p_render_size[0] < 64.0f || p_render_size[1] < 64.0f ||
        std::abs(p_far[1] * p_far[0] - 1.0f) > 1e-3f) {
        return;
    }
    // psport: the first constants of each G-buffer pass are its camera. A frame may have more
    // than one such pass (another camera before the main scene): the camera goes with the pass
    // whose depth the motion vectors use (CommitFrameCamera), not with the frame's first pass.
    if (m_has_pass_camera) {
        return;
    }
    std::memcpy(m_pass_camera.view.data(), p_data + m_constants.off_dw_view, 12 * sizeof(float));
    std::memcpy(m_pass_camera.inv_view.data(), p_data + m_constants.off_dw_inverse_view, 12 * sizeof(float));
    // psport: PS_CAMERA_INVERSE=own: the inverse of the view matrix itself instead of the one the
    // game stores beside it (A/B; they agree to float precision). PS_CAMERA_LOG prints the gap.
    static const bool s_is_own_inverse = [] {
        const char* sz_env = std::getenv("PS_CAMERA_INVERSE");
        return sz_env && std::strcmp(sz_env, "own") == 0;
    }();
    const auto computed_inverse = InverseAffine(m_pass_camera.view);
    float inverse_gap = 0.0f;
    for (int i = 0; i < 12; ++i) {
        inverse_gap = std::max(inverse_gap, std::abs(computed_inverse[i] - m_pass_camera.inv_view[i]));
    }
    const auto game_inverse = m_pass_camera.inv_view;
    if (s_is_own_inverse != PsToggle::Disabled(PsToggle::CameraOwnInverse)) {
        m_pass_camera.inv_view = computed_inverse;
    }
    // psport: PS_CAMERA_LOG=1 prints the camera position every 100 ms (scripted tests of how far
    // the player moves in a given time at different frame rates).
    static const bool s_should_log_camera = std::getenv("PS_CAMERA_LOG") != nullptr;
    if (s_should_log_camera) {
        using Clock = std::chrono::steady_clock;
        static const auto s_start = Clock::now();
        static auto s_next = s_start;
        if (const auto now = Clock::now(); now >= s_next) {
            s_next = now + std::chrono::milliseconds(100);
            std::printf("Camera: %.3f s at %.3f %.3f %.3f (inverse of view: %.3f %.3f %.3f, largest "
                        "difference %.4f)\n",
                        std::chrono::duration<double>(now - s_start).count(), game_inverse[3],
                        game_inverse[7], game_inverse[11], computed_inverse[3], computed_inverse[7],
                        computed_inverse[11], inverse_gap);
        }
    }
    // psport: the projection as the motion shaders use it: ndc +y down the screen. 0.2 assumed the
    // viewport flips y (ndc +y up, as a G-buffer pass with yscale -540), 0.3 that it does not; the
    // viewport of the frame's G-buffer pass decides (PS_CAMERA_Y=up/down forces one, for tests).
    static const float s_forced_y = [] {
        const char* sz_env = std::getenv("PS_CAMERA_Y");
        return !sz_env ? 0.0f : std::strcmp(sz_env, "up") == 0 ? -1.0f : std::strcmp(sz_env, "down") == 0 ? 1.0f : 0.0f;
    }();
    const float y_sign = (s_forced_y != 0.0f ? s_forced_y : m_gbuffer_y_sign) *
                         (PsToggle::Disabled(PsToggle::CameraYFlip) ? -1.0f : 1.0f);
    const float* p_projection = p_data + m_constants.off_dw_projection;
    m_pass_camera.proj = {p_projection[0] * m_gbuffer_x_sign, p_projection[5] * y_sign, p_projection[10],
                        p_projection[11]};
    {
        // psport: the projection's y scale sign, printed when it changes (see the G-buffer
        // viewport line): which way view +y goes on the screen.
        static int s_last_sign = 0;
        const int sign = (p_projection[5] < 0.0f ? -2 : 2) + (m_gbuffer_y_sign < 0.0f ? -1 : 1);
        if (sign != s_last_sign) {
            s_last_sign = sign;
            std::printf("Camera motion: projection x %.4f y %.4f z %.6f %.6f, G-buffer viewport y "
                        "%s: screen y follows view %s\n",
                        p_projection[0], p_projection[5], p_projection[10], p_projection[11],
                        m_gbuffer_y_sign < 0.0f ? "flipped" : "not flipped",
                        m_pass_camera.proj[1] < 0.0f ? "-y" : "+y");
        }
    }
    m_pass_camera.is_valid = m_pass_camera.proj[0] != 0.0f && m_pass_camera.proj[1] != 0.0f;
    const std::array<u32, 2> size{u32(p_render_size[0]), u32(p_render_size[1])};
    if (size != m_render_size) {
        std::printf("Camera motion: scene render size %ux%u\n", size[0], size[1]);
        m_render_size = size;
    }
    m_has_pass_camera = true;
    m_has_frame_camera = true;
    if (m_c_frame_gbuffer_passes <= m_pass_positions.size() && m_c_frame_gbuffer_passes > 0) {
        // World position of this pass's camera (diagnostics: frames with several passes).
        const auto& v = m_pass_camera.view;
        m_pass_positions[m_c_frame_gbuffer_passes - 1] = {
            -(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]),
            -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
            -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11])};
    }
}

void CameraMotion::OnGBufferPass(VideoCore::ImageId depth, float x_sign, float y_sign) {
    if (depth != m_depth_id) {
        // Another depth target: another G-buffer pass, with its own camera.
        m_depth_id = depth;
        m_has_pass_camera = false;
        ++m_c_frame_gbuffer_passes;
    }
    m_gbuffer_x_sign = x_sign;
    m_gbuffer_y_sign = y_sign;
}

void CameraMotion::CommitFrameCamera() {
    if (m_has_committed || !m_has_pass_camera) {
        return;
    }
    m_previous = m_current;
    m_current = m_pass_camera;
    m_has_committed = true;
    // psport: a cut (a cutscene's next shot, a respawn) turns or moves the camera further in one
    // frame than it can move, so the previous frame's image is another shot: the upscaler starts its
    // history over (TakeCut) instead of reprojecting it, which flashed the old shot for a frame or
    // two. Scale-free thresholds, the same for every game: 30 degrees, or 64 near-plane distances.
    // PS_CAMERA_CUTS=0 (diagnostics): no cuts detected.
    static const bool s_is_on = [] {
        const char* sz_env = std::getenv("PS_CAMERA_CUTS");
        return !(sz_env && sz_env[0] == '0');
    }();
    if (!s_is_on || !m_previous.is_valid || !m_current.is_valid) {
        return;
    }
    const auto& p = m_previous.view;
    const auto& q = m_current.view;
    float cos_sum = 0.0f;
    for (int row = 0; row < 3; ++row) {
        const float* a = &p[row * 4];
        const float* b = &q[row * 4];
        const float len =
            std::sqrt((a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) * (b[0] * b[0] + b[1] * b[1] + b[2] * b[2]));
        cos_sum += len > 0.0f ? (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / len : 1.0f;
    }
    const float turned = std::acos(std::clamp((cos_sum - 1.0f) * 0.5f, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
    const auto& pi = m_previous.inv_view;
    const auto& qi = m_current.inv_view;
    const float dx = qi[3] - pi[3], dy = qi[7] - pi[7], dz = qi[11] - pi[11];
    const float near_plane = Near();
    const float moved = near_plane > 0.0f ? std::sqrt(dx * dx + dy * dy + dz * dz) / near_plane : 0.0f;
    if (turned < 30.0f && moved < 64.0f) {
        return;
    }
    m_is_cut = true;
    ++m_c_cuts;
    // The wall clock, so that a cut can be matched with what the player saw.
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::printf("Camera motion: cut at %02d:%02d:%02d (turned %.0f degrees, moved %.0f near planes): "
                "upscaler history reset (%llu so far; PS_CAMERA_CUTS=0: off)\n",
                local.tm_hour, local.tm_min, local.tm_sec, turned, moved,
                static_cast<unsigned long long>(m_c_cuts));
}

void CameraMotion::OnDisplayPass(VideoCore::ImageId frame) {
    CommitFrameCamera();
    if (m_has_debug_overlay && frame && m_depth_id && m_current.is_valid && m_previous.is_valid) {
        Overlay(frame);
    }
    if (m_c_frame_gbuffer_passes > 1) {
        static auto s_next = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now >= s_next) {
            s_next = now + std::chrono::seconds(2);
            std::printf("Camera motion: %u G-buffer passes this frame, cameras at", m_c_frame_gbuffer_passes);
            for (u32 i = 0; i < std::min<u32>(m_c_frame_gbuffer_passes, m_pass_positions.size()); ++i) {
                std::printf(" (%.2f %.2f %.2f)", m_pass_positions[i][0], m_pass_positions[i][1],
                            m_pass_positions[i][2]);
            }
            std::printf("; the last pass's camera goes with its depth\n");
        }
    }
    if (!m_has_frame_camera) InvalidateHistory();
    m_has_frame_camera = false;
    m_has_pass_camera = false;
    m_has_committed = false;
    m_c_frame_gbuffer_passes = 0;
    m_depth_id = {};
}

void CameraMotion::Overlay(VideoCore::ImageId frame) {
    if (!m_frames[0]->IsAllocated() || !m_frames[1]->IsAllocated()) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Camera motion overlay skipped: its buffers have no memory");
        return;
    }
    auto& depth = m_texture_cache.GetImage(m_depth_id);
    auto& color = m_texture_cache.GetImage(frame);
    const auto depth_format = depth.info.pixel_format;
    if ((depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD32SfloatS8Uint) ||
        color.info.pixel_format != vk::Format::eR8G8B8A8Unorm ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
        color.info.size.width != depth.info.size.width ||
        color.info.size.height != depth.info.size.height) {
        static bool s_has_warned = false;
        if (!s_has_warned) {
            s_has_warned = true;
            std::printf("Camera motion: overlay skipped (depth %s, frame %s %ux%u)\n",
                        vk::to_string(depth_format).c_str(),
                        vk::to_string(color.info.pixel_format).c_str(), color.info.size.width,
                        color.info.size.height);
        }
        return;
    }
    const auto device = m_instance.GetDevice();
    const auto depth_view = Check(device.createImageView({
        .image = vk::Image(depth.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));

    m_scheduler.EndRendering();
    m_runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    m_runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    m_runtime.FlushBarriers();

    const PushConstants push{
        .reproject = Multiply(m_previous.view, m_current.inv_view),
        .proj = m_current.proj,
        .prev_proj = m_previous.proj,
        .size = {float(color.info.size.width), float(color.info.size.height)},
        .jitter = m_jitter,
        .previous_jitter = m_previous_jitter,
        .mode = PsToggle::Disabled(1u << 20)   ? 1u
                : PsToggle::Disabled(1u << 21) ? 2u
                : PsToggle::Disabled(1u << 22) ? 3u
                : PsToggle::Disabled(1u << 23) ? 4u
                                               : 0u,
    };
    static u32 s_c_logs = 0;
    if (++s_c_logs % 200 == 0) {
        const auto& m = push.reproject;
        std::printf("Camera motion: proj %g %g %g %g prev %g %g %g %g\n"
                    "  view  %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f\n"
                    "  reproj %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f\n"
                    "  depth %s %ux%u, frame %ux%u\n",
                    push.proj[0], push.proj[1], push.proj[2], push.proj[3], push.prev_proj[0],
                    push.prev_proj[1], push.prev_proj[2], push.prev_proj[3], m_current.view[0],
                    m_current.view[1], m_current.view[2], m_current.view[3], m_current.view[4],
                    m_current.view[5], m_current.view[6], m_current.view[7], m_current.view[8],
                    m_current.view[9], m_current.view[10], m_current.view[11], m[0], m[1], m[2], m[3],
                    m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                    vk::to_string(depth_format).c_str(), depth.info.size.width,
                    depth.info.size.height, color.info.size.width, color.info.size.height);
    }
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{.imageView = color_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const auto& prev_frame = *m_frames[m_i_frame ^ 1];
    const auto& next_frame = *m_frames[m_i_frame];
    m_i_frame ^= 1;
    const vk::DescriptorBufferInfo prev_info{prev_frame.Handle(), 0, prev_frame.SizeBytes()};
    const vk::DescriptorBufferInfo next_info{next_frame.Handle(), 0, next_frame.SizeBytes()};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &color_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &prev_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &next_info},
    }};
    const auto cmdbuf = m_scheduler.CommandBuffer();
    const Breadcrumbs::Scope crumb{cmdbuf, m_scheduler.CrumbStream(), "motion overlay"};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *m_overlay_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *m_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    // The previous frame buffer was written by the last dispatch.
    const vk::MemoryBarrier2 frame_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &frame_barrier});
    cmdbuf.dispatch((color.info.size.width + 7) / 8, (color.info.size.height + 7) / 8, 1);

    m_scheduler.DeferOperation([device, depth_view, color_view] {
        device.destroyImageView(depth_view);
        device.destroyImageView(color_view);
    });
}

} // namespace Vulkan
