// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>

#include <condition_variable>
#include <mutex>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};

    /// psport: frame generation's frame between the previous game frame and this one, presented
    /// before it (FrameGenerator); made at the first frame that generates one.
    VmaAllocation generated_allocation{};
    vk::Image generated_image{};
    vk::ImageView generated_view{};
    bool has_generated{false};
    FrameGenSkip frame_gen_skip{FrameGenSkip::None}; ///< why it has none

};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    /// psport: null when the Vulkan instance, the device, the swapchain or another object needed
    /// from the start could not be created (logged); psport_gpu_init then fails.
    static std::unique_ptr<Presenter> Create(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);

    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool,
              std::unique_ptr<Instance> p_instance);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        if (swapchain.GetHDR() != enable) {
            // psport: the mode change recreates the swapchain after waiting for the device idle,
            // which needs the other threads' queue submissions held off.
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            swapchain.SetHDR(enable);
        }
        pp_settings.hdr = enable ? 1 : 0;
    }

    void RegisterVideoOutSurface(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                                 VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        if (const auto image_id = texture_cache.FindImage(desc)) {
            texture_cache.GetImage(image_id).usage.vo_surface = 1u;
        }
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    /// psport: waits until the GPU has finished the commands that render `frame` (any thread).
    void WaitRendered(const Frame* frame);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    bool RecreateFrame(Frame* frame, u32 width, u32 height);
    void DestroyGenerated(Frame* frame);
    /// psport: frame generation for the frame just composed (GPU thread, after its passes).
    /// `view_format` and `image_size`: how the display buffer was read.
    void GenerateFrame(Frame* frame, vk::Format view_format, vk::Extent2D image_size);
    /// psport: blits `image` (General layout) into a swapchain image with the overlay and presents
    /// it. `is_last`: the game's frame itself, whose submission also signals the frame's fence
    /// (covering the generated frame's before it on the same queue). False when the swapchain gave
    /// no image or the fence could not be reset.
    bool PresentImage(Frame* frame, vk::Image image, bool is_last);
    /// psport: present thread, after a game frame: whether the display can show the generated
    /// frames (m_c_frame_gen_limit) and the flip limit with FIFO. `present_ms`: time Present took.
    void UpdateFrameGenLimit(double present_ms);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    std::unique_ptr<Instance> m_p_instance; ///< psport: created first, by Create
    Instance& instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    std::mutex passes_mutex; ///< psport: the passes are recorded on the recording threads
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< psport: PS_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
    /// psport: frame generation, null unless chosen at start (PsSettings::frame_gen). With it the
    /// frames are composed in its RGBA8 (m_frame_format) and blitted into the swapchain's format.
    std::unique_ptr<FrameGenerator> m_p_frame_generator;
    vk::Format m_frame_format{};
    /// The game's picture before its UI, through the same passes as the frame.
    Frame m_hudless_frame{};
    /// Generated frames per game frame the display shows now, 0 or 1 (present thread writes it).
    std::atomic<u32> m_c_frame_gen_limit{1};
    /// Why m_c_frame_gen_limit is 0: paused, or no room on the display.
    std::atomic<FrameGenSkip> m_frame_gen_held{FrameGenSkip::NoRoom};
    // Present thread: the interval of the game's frames the generated ones are paced in, the share
    // of it spent presenting, the pause after presenting fell behind, and the game frames of the
    // current second with the seconds in a row that had room for a generated frame.
    std::chrono::steady_clock::time_point m_last_game_present{};
    double m_game_frame_ms = 16.7;
    double m_present_busy = 0.0;
    int m_frame_gen_hold_seconds = 5;
    std::chrono::steady_clock::time_point m_frame_gen_hold_until{};
    std::chrono::steady_clock::time_point m_limit_window{};
    u32 m_c_limit_frames = 0;
    u32 m_c_limit_up_seconds = 0;
};

} // namespace Vulkan
