// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the driver's own pipeline cache (VkPipelineCache, the GPU code it compiled) kept on disk
// beside the shader cache, so that a pipeline the driver built once is not compiled again at the
// next start. The blob is read before the preload builds its pipelines through it, saved while the
// game runs (the process usually ends through _exit, which runs no destructor) and at exit, and
// only reused by the device and driver that wrote it. PS_DRIVER_PIPELINE_CACHE=0 keeps it in
// memory only, as before.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;

class DriverPipelineCache {
public:
    explicit DriverPipelineCache(const Instance& instance);
    ~DriverPipelineCache();

    DriverPipelineCache(const DriverPipelineCache&) = delete;
    DriverPipelineCache& operator=(const DriverPipelineCache&) = delete;

    /// VK_NULL_HANDLE when the driver could not make one: pipelines are then created without.
    [[nodiscard]] vk::PipelineCache Handle() const {
        return *m_cache;
    }

    /// The process is about to end through _exit: the live cache, if any, is written now.
    static void SaveForExit();

private:
    void Load(std::vector<u8>& out_data) const;
    void SaveIfChanged();
    void RunSaver(std::stop_token token);

    const Instance& m_instance;
    vk::UniquePipelineCache m_cache;
    std::filesystem::path m_path;
    size_t m_cb_saved{};
    std::condition_variable_any m_saver_cv;
    std::mutex m_saver_mutex;
    std::jthread m_saver;
};

} // namespace Vulkan
