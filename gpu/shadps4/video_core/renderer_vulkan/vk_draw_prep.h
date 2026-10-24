// SPDX-License-Identifier: GPL-2.0-or-later
// psport: speculative draw preparation on worker threads (docs/en/parallel_gpu.md, steps 1-2).
//
// Every submitted graphics command buffer is copied. A scanner thread replays the register
// writes of the whole stream in order (ApplyGraphicsRegisterPacket, the same code the GPU
// thread runs, nested indirect buffers included) and records, per buffer, its starting checksum
// and a delta of the register blocks it wrote. Where the state stops being knowable ahead of the
// GPU thread (a nested buffer it cannot read, a COND_EXEC over state packets) the scanner stops
// and waits for the GPU thread's own registers at the next buffer; the GPU thread also hands its
// registers over whenever its checksum at a buffer's start differs from the scanned one. Any
// number of workers (one per spare hardware thread) claim the nearest buffers ahead of the GPU
// thread, reach their starting state by applying deltas, and select the graphics pipeline and
// resource sharps of each direct draw. All helpers run as SCHED_IDLE: they only use idle cores.
// The GPU thread uses a prepared draw only when the running register checksums match and the
// flattened user data it computes itself equals the worker's; otherwise it takes the regular
// path. Workers never create or compile anything.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "common/types.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

namespace Vulkan {

struct PreparedStage {
    const Program* p_program;
    u64 hash;
    Shader::HwStage hw_stage;
    VAddr pgm_base;
    const u32* p_flat; ///< flattened user data the worker computed, owned by the submission
    u32 c_flat;
    // Resource sharps read from `p_flat` in the order of the program's resource lists, and the
    // texture description hash of each image (docs/en/parallel_gpu.md, step 2). Valid for the
    // GPU thread exactly when `p_flat` matched: sharps depend on nothing else.
    const AmdGpu::Image* p_image_sharps;
    const u64* p_image_hashes;
    const AmdGpu::Sampler* p_sampler_sharps;
    const AmdGpu::Buffer* p_buffer_sharps;
    u32 c_images, c_samplers, c_buffers;
};

/// Key of Rasterizer's texture description cache for a T# bound through `res`.
inline u64 ImageDescHash(const AmdGpu::Image& sharp, const Shader::ImageResource& res) {
    std::array<u64, 4> key;
    std::memcpy(key.data(), &sharp, sizeof(key));
    const u32 flags = u32(res.is_written) | u32(res.is_depth) << 1 | u32(res.is_array) << 2;
    u64 hash = flags * 0x9E3779B97F4A7C15ull;
    for (const u64 word : key) {
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    return hash;
}

/// Vertex inputs of a draw (the dynamic vertex input path): the attribute and binding
/// descriptions, the V# of each stream, and the stream memory merged into ranges.
struct PreparedVertexInputs {
    struct Range {
        VAddr base, end;
    };
    const vk::VertexInputAttributeDescription2EXT* p_attributes;
    const vk::VertexInputBindingDescription2EXT* p_bindings;
    const AmdGpu::Buffer* p_buffers;
    const u8* p_range_index; ///< per stream with memory: its entry in `p_ranges`
    const Range* p_ranges;
    u32 c_streams, c_ranges;
    u64 buffers_hash; ///< XXH3 of `p_buffers` (object motion stream identity)
    bool is_valid;
};

struct PreparedDraw {
    enum : u32 { Pending = 0, Ready = 1, Unavailable = 2 };
    std::atomic<u32> state{Pending};
    u64 reg_checksum{};
    GraphicsPipelineKey key{};
    u32 c_stages{};
    std::array<PreparedStage, MaxShaderStages> stages{};
    PreparedVertexInputs vertex{};
};

class DrawPreparation {
public:
    explicit DrawPreparation(PipelineCache& pipeline_cache);
    ~DrawPreparation();

    DrawPreparation(const DrawPreparation&) = delete;
    DrawPreparation& operator=(const DrawPreparation&) = delete;

    [[nodiscard]] bool Enabled() const noexcept {
        return m_c_workers != 0;
    }

    struct Submission;
    /// Game submit thread, outside the queue lock: copies and scans a top-level graphics
    /// command buffer. Null when disabled.
    std::shared_ptr<Submission> Build(std::span<const u32> commands);
    /// Game submit thread, under the queue lock: hands it to the workers in submission order.
    void Enqueue(u64 seq, std::shared_ptr<Submission> submission);

    /// GPU thread: brackets the processing of submission `seq`. The first call hands the
    /// workers the exact register state and checksum they start replaying from.
    void BeginSubmission(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum);
    void EndSubmission();

    /// GPU thread: the prepared state for the next direct draw of the current submission.
    const PreparedDraw* NextDraw();

    /// GPU thread: counts whether a prepared draw was used; prints every 5 s (PS_FRAME_STATS).
    void Count(bool is_used);

    struct Submission {
        u64 seq{};
        /// Shared with the fresh copy a rescan puts in its place, which workers may still read.
        std::shared_ptr<const std::vector<u32>> commands;
        std::unique_ptr<PreparedDraw[]> draws;
        u32 c_draws{};
        std::vector<std::unique_ptr<u32[]>> flat_chunks; ///< from the claiming worker
        // Set by the scanner before `is_scanned`.
        u64 start_checksum{}, end_checksum{};
        AmdGpu::RegDelta delta; ///< registers written, values at the end
        /// State packets of the nested indirect buffers the scanner followed, in order, and where
        /// each buffer's packets end: workers replay these instead of reading guest memory again,
        /// so they reach the scanner's checksums even if the game rewrites a nested buffer.
        std::vector<u32> nested_words;
        std::vector<u32> nested_ends;
        /// Direct draws before the first packet whose register effects are unknown ahead of the
        /// GPU thread; when there is one, `is_end_known` is false and the delta is not recorded.
        u32 c_draws_known{};
        bool is_end_known{};
        std::atomic<bool> is_scanned{false};
        std::atomic<bool> is_claimed{false};
        std::atomic<bool> is_gpu_done{false};
    };

private:
    void ScannerLoop(std::stop_token stop);
    void WorkerLoop(std::stop_token stop, u32 i_worker);
    void Collect();
    /// Under `m_mutex`: the tail becomes the GPU thread's state at the start of `seq`, and the
    /// scanner and workers restart from it.
    void RebaseTail(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum);

    PipelineCache& m_pipeline_cache;
    u64 m_baseline_seq{};
    /// State at the start of the oldest queued buffer (`m_tail_seq`): a worker whose position
    /// was collected restarts from here. Guarded by `m_mutex`.
    std::unique_ptr<AmdGpu::Regs> m_tail_regs;
    u64 m_tail_seq{};
    u64 m_tail_checksum{};
    /// False once a buffer whose end state the scanner could not know is collected, until the
    /// GPU thread hands over its registers at the next buffer. Guarded by `m_mutex`.
    bool m_is_tail_known{};
    /// Bumped by every rebase: scans and worker positions from an older epoch are discarded.
    /// Guarded by `m_mutex`.
    u64 m_tail_epoch{};
    // GPU thread only (PS_FRAME_STATS).
    u64 m_c_rebases = 0;          ///< scanner restarts after lagging
    u64 m_c_unknown_resyncs = 0;  ///< restarts after a buffer whose end state was unknown
    u64 m_c_checksum_resyncs = 0; ///< restarts after a start checksum that differed
    std::atomic<u64> m_c_nested_followed{0}; ///< nested indirect buffers the scanner replayed
    std::atomic<u64> m_c_unknown_ends{0};    ///< buffers the scanner stopped in
    bool m_is_baseline_ready{}; ///< guarded by `m_mutex`
    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::deque<std::shared_ptr<Submission>> m_submissions; ///< ordered by seq
    std::atomic<u64> m_gpu_seq{0};
    std::shared_ptr<Submission> m_current;

public:
    /// psport: the current submission (prepared draws), for the draw recording thread to keep
    /// it alive while it still records its draws.
    [[nodiscard]] std::shared_ptr<const void> KeepCurrent() const {
        return m_current;
    }

private:
    u32 m_current_draw = 0;
    u64 m_c_used = 0, m_c_unused = 0;
    u32 m_c_workers = 0; ///< fixed before the worker threads start
    std::jthread m_scanner;
    std::vector<std::jthread> m_workers;
};

} // namespace Vulkan
