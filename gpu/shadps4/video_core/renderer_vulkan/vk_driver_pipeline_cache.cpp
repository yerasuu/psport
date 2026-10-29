// SPDX-License-Identifier: GPL-2.0-or-later
// psport: the saved driver pipeline cache follows Supermedo/bloodborne_pc e28a412 and
// Mrsuss60/bloodborne_pc_windows_port b972798 (GPL-2.0-or-later), written anew.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <system_error>
#include <vector>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_driver_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {

namespace {

constexpr u32 FILE_MAGIC = 0x43565350u; // "PSVC"
constexpr u32 FILE_VERSION = 1;
constexpr auto SAVE_INTERVAL = std::chrono::seconds(20);

// The driver checks its own header too, but only after the blob reached it: one written by
// another device or driver (an update keeps the device id) is dropped here, before the driver
// sees it. driverVersion is in the key because some drivers keep pipelineCacheUUID across updates.
struct FileHeader {
    u32 magic;
    u32 version;
    u32 vendor_id;
    u32 device_id;
    u32 driver_version;
    std::array<u8, VK_UUID_SIZE> pipeline_cache_uuid;
    u32 reserved;
    u64 cb_data;
};

FileHeader MakeHeader(const Instance& instance, size_t cb_data) {
    FileHeader header{
        .magic = FILE_MAGIC,
        .version = FILE_VERSION,
        .vendor_id = instance.GetVendorID(),
        .device_id = instance.GetDeviceID(),
        .driver_version = instance.GetDriverVersion(),
        .pipeline_cache_uuid = {},
        .reserved = 0,
        .cb_data = cb_data,
    };
    const auto uuid = instance.GetPipelineCacheUUID();
    std::memcpy(header.pipeline_cache_uuid.data(), uuid.data(), VK_UUID_SIZE);
    return header;
}

// Why a blob is not offered to the driver, or nullptr when it can be.
const char* RejectReason(const Instance& instance, std::span<const u8> file) {
    FileHeader header{};
    if (file.size() < sizeof(header)) {
        return "shorter than its header";
    }
    std::memcpy(&header, file.data(), sizeof(header));
    const FileHeader expected = MakeHeader(instance, file.size() - sizeof(header));
    if (header.magic != FILE_MAGIC || header.version != FILE_VERSION) {
        return "not a psport driver cache of this version";
    }
    if (header.cb_data != expected.cb_data) {
        return "cut short or padded";
    }
    if (header.vendor_id != expected.vendor_id || header.device_id != expected.device_id ||
        header.driver_version != expected.driver_version ||
        header.pipeline_cache_uuid != expected.pipeline_cache_uuid) {
        return "written by another device or driver";
    }
    VkPipelineCacheHeaderVersionOne blob_header{};
    if (header.cb_data < sizeof(blob_header)) {
        return "driver blob shorter than its header";
    }
    std::memcpy(&blob_header, file.data() + sizeof(header), sizeof(blob_header));
    if (blob_header.headerSize < sizeof(blob_header) || blob_header.headerSize > header.cb_data ||
        blob_header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        blob_header.vendorID != expected.vendor_id || blob_header.deviceID != expected.device_id ||
        std::memcmp(blob_header.pipelineCacheUUID, expected.pipeline_cache_uuid.data(),
                    VK_UUID_SIZE) != 0) {
        return "driver blob header does not match this device";
    }
    return nullptr;
}

// The live cache for SaveForExit, guarded with every save by s_save_mutex.
std::mutex s_save_mutex;
DriverPipelineCache* s_p_live{};

} // namespace

DriverPipelineCache::DriverPipelineCache(const Instance& instance) : m_instance{instance} {
    auto& storage = Storage::DataBase::Instance();
    if (EmulatorSettings.IsDriverPipelineCacheEnabled() && storage.IsOpened()) {
        m_path = storage.DriverCachePath();
    }
    std::vector<u8> data;
    Load(data);

    const auto device = m_instance.GetDevice();
    auto [result, cache] = device.createPipelineCacheUnique({
        .initialDataSize = data.size(),
        .pInitialData = data.empty() ? nullptr : data.data(),
    });
    if (result != vk::Result::eSuccess && !data.empty()) {
        LOG_ERROR_COUNTED(Render_Vulkan,
                          "Driver pipeline cache: the driver refused the saved one ({}), starting "
                          "empty",
                          vk::to_string(result));
        data.clear();
        auto retry = device.createPipelineCacheUnique({});
        result = retry.result;
        cache = std::move(retry.value);
    }
    if (result != vk::Result::eSuccess) {
        // Without one (VK_NULL_HANDLE) pipelines are created all the same.
        LOG_ERROR_COUNTED(Render_Vulkan, "Failed to create the pipeline cache: {}",
                          vk::to_string(result));
        std::printf("Driver pipeline cache: none (the driver could not make one)\n");
        return;
    }
    m_cache = std::move(cache);

    if (m_path.empty()) {
        std::printf("Driver pipeline cache: in memory only (%s)\n",
                    EmulatorSettings.IsDriverPipelineCacheEnabled()
                        ? "the shader cache is off, PS_PIPELINE_CACHE=0"
                        : "PS_DRIVER_PIPELINE_CACHE=0");
        return;
    }
    m_cb_saved = data.size();
    std::printf("Driver pipeline cache: %s %s (%zu KiB; PS_DRIVER_PIPELINE_CACHE=0 turns it "
                "off)\n",
                data.empty() ? "new," : "loaded from", m_path.string().c_str(), data.size() >> 10);
    {
        std::scoped_lock lock{s_save_mutex};
        s_p_live = this;
    }
    m_saver = std::jthread{[this](std::stop_token token) { RunSaver(token); }};
}

DriverPipelineCache::~DriverPipelineCache() {
    if (m_saver.joinable()) {
        m_saver.request_stop();
        m_saver.join();
    }
    std::scoped_lock lock{s_save_mutex};
    if (s_p_live == this) {
        s_p_live = nullptr;
        SaveIfChanged();
    }
}

void DriverPipelineCache::SaveForExit() {
    std::scoped_lock lock{s_save_mutex};
    if (s_p_live) {
        s_p_live->SaveIfChanged();
    }
}

void DriverPipelineCache::RunSaver(std::stop_token token) {
    Common::SetCurrentThreadName("psport:DriverCache");
    std::unique_lock lock{m_saver_mutex};
    while (!token.stop_requested()) {
        m_saver_cv.wait_for(lock, token, SAVE_INTERVAL, [] { return false; });
        if (token.stop_requested()) {
            return;
        }
        std::scoped_lock save_lock{s_save_mutex};
        SaveIfChanged();
    }
}

void DriverPipelineCache::Load(std::vector<u8>& out_data) const {
    if (m_path.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(m_path, ec)) {
        return;
    }
    std::vector<u8> file;
    {
        const auto io = Common::FS::IOFile{m_path, Common::FS::FileAccessMode::Read};
        if (!io.IsOpen()) {
            LOG_ERROR_COUNTED(Render_Vulkan,
                              "Driver pipeline cache: {} could not be opened, starting empty",
                              m_path.string());
            return;
        }
        file.resize(io.GetSize());
        if (io.Read(file) != file.size()) {
            LOG_ERROR_COUNTED(Render_Vulkan, "Driver pipeline cache: {} read short, starting empty",
                              m_path.string());
            return;
        }
    }
    if (const char* sz_reason = RejectReason(m_instance, file)) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Driver pipeline cache: {} rejected ({}), starting empty",
                          m_path.string(), sz_reason);
        std::printf("Driver pipeline cache: saved one rejected (%s), starting empty\n", sz_reason);
        return;
    }
    out_data.assign(file.begin() + sizeof(FileHeader), file.end());
}

// The caller holds s_save_mutex. The driver's blob only grows while pipelines are made, so its
// size tells whether there is anything new to write.
void DriverPipelineCache::SaveIfChanged() {
    const auto device = m_instance.GetDevice();
    size_t cb_data = 0;
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPipelineCacheData(device, *m_cache, &cb_data, nullptr) !=
            VK_SUCCESS ||
        cb_data == m_cb_saved) {
        return;
    }
    std::vector<u8> file(sizeof(FileHeader) + cb_data);
    const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPipelineCacheData(
        device, *m_cache, &cb_data, file.data() + sizeof(FileHeader));
    if (result == VK_INCOMPLETE) {
        return; // pipelines made between the two calls: taken at the next save
    }
    if (result != VK_SUCCESS) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Driver pipeline cache: could not be read for saving ({})",
                          vk::to_string(vk::Result(result)));
        return;
    }
    file.resize(sizeof(FileHeader) + cb_data);
    const FileHeader header = MakeHeader(m_instance, cb_data);
    std::memcpy(file.data(), &header, sizeof(header));

    // Written beside it and renamed into place, so that an exit or a crash mid-write leaves the
    // previous file, never one cut short.
    auto temp = m_path;
    temp += ".tmp";
    bool is_written = false;
    {
        const auto io = Common::FS::IOFile{temp, Common::FS::FileAccessMode::Create};
        is_written = io.IsOpen() && io.Write(file) == file.size();
    }
    std::error_code ec;
    if (is_written) {
        std::filesystem::rename(temp, m_path, ec);
    }
    if (!is_written || ec) {
        LOG_ERROR_COUNTED(Render_Vulkan, "Driver pipeline cache: could not write {}",
                          m_path.string());
        std::filesystem::remove(temp, ec);
        return;
    }
    m_cb_saved = cb_data;
    LOG_INFO(Render_Vulkan, "Driver pipeline cache saved ({} KiB)", cb_data >> 10);
}

} // namespace Vulkan
