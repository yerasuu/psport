// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>

namespace {

// psport: one queue drained by the writer thread, which finishes it before stopping. The
// upstream worker dropped what was still queued when stopped and polled a counter both threads
// changed without a lock.
struct BlobWrite {
    std::filesystem::path path;
    std::vector<u8> bytes;
};
// Timed so that a flush at exit stays bounded even if the exiting thread could not get it.
std::timed_mutex s_queue_mutex{};
std::condition_variable_any s_queue_cv{};
std::condition_variable_any s_idle_cv{};
std::deque<BlobWrite> s_queue{};
std::atomic<u32> s_c_pending{};
bool s_is_closed_for_exit{};
bool s_has_reported_refusal{};

// psport: the process ends through _exit (runtime_stop, the window thread), which never runs
// Close: what the writer has not finished by then is lost and recompiled at the next start.
constexpr std::chrono::milliseconds EXIT_FLUSH_LIMIT{2000};

// psport: the archive is read from <serial>.zip and written to <serial>.zip.tmp (the entries
// kept copied over), which replaces it when the cache is closed, so a crash or an exit before
// that leaves the old archive, never one cut short without its central directory.
mz_zip_archive s_zip_reader{};
bool s_has_zip_reader{};
mz_zip_archive s_zip_writer{};
bool s_is_zip_writable{};
std::filesystem::path s_zip_temp_path{};

} // namespace

namespace Storage {

void WriteBlob(const BlobWrite& write, bool is_zip_writable) {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        if (!is_zip_writable) {
            LOG_ERROR(Render, "Pipeline cache: {} saved before the archive was writable, dropped",
                      write.path.string());
            return;
        }
        if (!mz_zip_writer_add_mem(&s_zip_writer, write.path.string().c_str(), write.bytes.data(),
                                   write.bytes.size(), MZ_BEST_COMPRESSION)) {
            LOG_ERROR(Render, "Failed to add {} to the archive", write.path.string().c_str());
        }
        return;
    }
    // psport: written under a temporary name and renamed into place, so a crash or a power loss
    // mid-write leaves the old file or none, never a cut-short one that the next start reads as
    // a damaged entry (issue #28).
    using namespace Common::FS;
    auto temp = write.path;
    temp += ".tmp";
    bool is_written = false;
    {
        const auto file = IOFile{temp, FileAccessMode::Create};
        is_written = file.IsOpen() && file.Write(write.bytes) == write.bytes.size();
    }
    std::error_code ec;
    if (is_written) {
        std::filesystem::rename(temp, write.path, ec);
    }
    if (!is_written || ec) {
        LOG_ERROR(Render, "Failed to write {}", write.path.string());
        std::filesystem::remove(temp, ec);
    }
}

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    std::unique_lock lk{s_queue_mutex};
    for (;;) {
        Common::CondvarWait(s_queue_cv, lk, stoken, [] { return !s_queue.empty(); });
        if (s_queue.empty()) {
            return;
        }
        const BlobWrite write = std::move(s_queue.front());
        s_queue.pop_front();
        const bool is_zip_writable = s_is_zip_writable;
        lk.unlock();
        WriteBlob(write, is_zip_writable);
        lk.lock();
        if (s_c_pending.fetch_sub(1, std::memory_order_relaxed) == 1) {
            s_idle_cv.notify_all();
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&s_zip_reader);
        mz_zip_zero_struct(&s_zip_writer);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");
        s_zip_temp_path = cache_path;
        s_zip_temp_path += ".tmp";

        s_has_zip_reader = mz_zip_reader_init_file(&s_zip_reader, cache_path.string().c_str(), 0) &&
            mz_zip_validate_archive(&s_zip_reader, 0);
        if (!s_has_zip_reader) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&s_zip_reader);
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        // psport: temporary files of writes a crash or an exit cut short (never read as blobs).
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
            if (entry.path().extension() == ".tmp") {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void FinishArchive(const std::filesystem::path& cache_path) {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        if (s_has_zip_reader) {
            mz_zip_reader_end(&s_zip_reader);
            s_has_zip_reader = false;
        }
        if (s_is_zip_writable) {
            const bool is_finalized = mz_zip_writer_finalize_archive(&s_zip_writer);
            mz_zip_writer_end(&s_zip_writer);
            s_is_zip_writable = false;
            std::error_code ec;
            if (is_finalized) {
                std::filesystem::rename(s_zip_temp_path, cache_path, ec);
            }
            if (!is_finalized || ec) {
                LOG_ERROR(Render, "Failed to write the cache archive {}", cache_path.string());
                std::filesystem::remove(s_zip_temp_path, ec);
            }
        }
    }
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    // FlushForExit may have stopped the writer already.
    if (io_worker.joinable()) {
        io_worker.request_stop();
        io_worker.join();
    }
    FinishArchive(cache_path);

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path, std::vector<T>&& v) {
    path.replace_extension(GetBlobFileExtension(type));
    BlobWrite write{std::move(path), {}};
    if constexpr (std::is_same_v<T, u8>) {
        write.bytes = std::move(v);
    } else {
        const auto* p_bytes = reinterpret_cast<const u8*>(v.data());
        write.bytes.assign(p_bytes, p_bytes + v.size() * sizeof(T));
    }
    {
        std::scoped_lock lk{s_queue_mutex};
        if (s_is_closed_for_exit) {
            if (!std::exchange(s_has_reported_refusal, true)) {
                LOG_WARNING(Render, "Pipeline cache: closed for exit, later blobs are not saved");
            }
            return false;
        }
        s_queue.push_back(std::move(write));
        s_c_pending.fetch_add(1, std::memory_order_relaxed);
    }
    s_queue_cv.notify_one();
    return true;
}

u32 PendingBlobs() {
    return s_c_pending.load(std::memory_order_relaxed);
}

void DataBase::FlushForExit() {
    if (!opened) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + EXIT_FLUSH_LIMIT;
    std::unique_lock lk{s_queue_mutex, deadline};
    if (!lk.owns_lock()) {
        std::printf("Pipeline cache: %u blobs still queued at exit, dropped after the time limit "
                    "(%lld ms, queue lock not taken)\n",
                    PendingBlobs(), static_cast<long long>(EXIT_FLUSH_LIMIT.count()));
        return;
    }
    const u32 c_queued = PendingBlobs();
    const bool is_idle = s_idle_cv.wait_until(lk, deadline, [] { return PendingBlobs() == 0; });
    s_is_closed_for_exit = true;
    lk.unlock();
    if (!is_idle) {
        std::printf("Pipeline cache: %u blobs still queued at exit, dropped after the time limit (%lld ms)\n",
                    PendingBlobs(), static_cast<long long>(EXIT_FLUSH_LIMIT.count()));
        return;
    }
    io_worker.request_stop();
    io_worker.join();
    FinishArchive(cache_path);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::printf("Pipeline cache: flushed %u blobs in %lld ms\n", c_queued,
                static_cast<long long>(elapsed.count()));
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        if (!s_has_zip_reader) {
            return;
        }
        int index{-1};
        index = mz_zip_reader_locate_file(&s_zip_reader, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&s_zip_reader, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&s_zip_reader, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = s_has_zip_reader ? mz_zip_reader_get_num_files(&s_zip_reader) : 0;
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&s_zip_reader, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&s_zip_reader, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&s_zip_reader, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(std::move(data));
                }
            }
        }
    }
}

void DataBase::Clear() {
    if (!opened) {
        return;
    }
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        // The archive is replaced at Close by one without the old entries.
        if (s_has_zip_reader) {
            mz_zip_reader_end(&s_zip_reader);
            s_has_zip_reader = false;
        }
        LOG_WARNING(Render, "Pipeline cache cleared: it is rebuilt for this build");
        return;
    }
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

void DataBase::FinishPreload() {
    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    if (!mz_zip_writer_init_file(&s_zip_writer, s_zip_temp_path.string().c_str(), 0)) {
        LOG_ERROR(Render, "Failed to create the cache archive {}: nothing is saved this session",
                  s_zip_temp_path.string());
        return;
    }
    if (s_has_zip_reader) {
        u32 c_lost = 0;
        const u32 c_files = mz_zip_reader_get_num_files(&s_zip_reader);
        for (u32 i_file = 0; i_file < c_files; ++i_file) {
            c_lost += !mz_zip_writer_add_from_zip_reader(&s_zip_writer, &s_zip_reader, i_file);
        }
        if (c_lost != 0) {
            LOG_ERROR(Render, "Cache archive: {} of {} entries not carried over", c_lost, c_files);
        }
        mz_zip_reader_end(&s_zip_reader);
        s_has_zip_reader = false;
    }
    std::scoped_lock lk{s_queue_mutex};
    s_is_zip_writable = true;
}

} // namespace Storage
