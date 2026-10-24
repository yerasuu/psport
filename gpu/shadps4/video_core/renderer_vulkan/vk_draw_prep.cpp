// SPDX-License-Identifier: GPL-2.0-or-later
// psport: speculative draw preparation on worker threads (see vk_draw_prep.h).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "psport_srt_faults.h"
#include "psport_threads.h"
#include "psport_toggles.h"
#include "common/debug.h"
#include "common/thread.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include <xxhash.h>

namespace Vulkan {

namespace {

bool IsDirectDraw(AmdGpu::PM4ItOpcode opcode) {
    using AmdGpu::PM4ItOpcode;
    return opcode == PM4ItOpcode::DrawIndex2 || opcode == PM4ItOpcode::DrawIndexOffset2 ||
           opcode == PM4ItOpcode::DrawIndexAuto;
}

/// Walks the type-3 packets of a command buffer (type-2 padding skipped) as the GPU thread does:
/// packets shorter than their fields are skipped, which the GPU thread counts, so that the
/// register checksums and the draw ordinals stay equal to its own. A `func` returning bool
/// stops the walk by returning false.
template <typename Func>
void ForEachPacket(std::span<const u32> commands, Func&& func) {
    for (size_t at = 0; at < commands.size();) {
        const auto* p_header = reinterpret_cast<const AmdGpu::PM4Header*>(commands.data() + at);
        if (p_header->type == 2) {
            ++at;
            continue;
        }
        if (p_header->type != 3) {
            return;
        }
        const u32 c_body_words = p_header->type3.NumWords();
        const size_t words = c_body_words + 1;
        if (at + words > commands.size()) {
            return;
        }
        if (c_body_words >= AmdGpu::Liverpool::MinBodyWords(p_header->type3.opcode, false)) {
            if constexpr (std::is_same_v<std::invoke_result_t<Func&, const AmdGpu::PM4Header*>,
                                         bool>) {
                if (!func(p_header)) {
                    return;
                }
            } else {
                func(p_header);
            }
        }
        at += words;
    }
}

/// The GPU thread runs nested buffers recursively; chains deeper than this are not replayed.
constexpr u32 MaxNestedDepth = 4;

/// The scanner's register replay of one top-level buffer.
struct ScanReplay {
    AmdGpu::Regs& regs;
    u64& checksum;
    AmdGpu::RegDirty& dirty;
    std::vector<u32>& nested_words;
    std::atomic<u64>& c_nested_followed;
};

/// A nested buffer is guest memory read ahead of the GPU thread: it may be unmapped by now, so a
/// fault returns here (g_p_runtime_fault_recover) instead of stopping the port.
bool CopyGuestWords(const u32* p_guest, u32 c_words, std::vector<u32>& out_words) {
    out_words.resize(c_words);
    PsHostRecoveryPoint recover;
    if (PS_HOST_RECOVERY_SET(recover)) {
        g_p_runtime_fault_recover = nullptr;
        return false;
    }
    g_p_runtime_fault_recover = &recover;
    std::memcpy(out_words.data(), p_guest, c_words * sizeof(u32));
    g_p_runtime_fault_recover = nullptr;
    return true;
}

bool ScanPacket(const AmdGpu::PM4Header* p_header, const u32* p_end, u32 depth,
                ScanReplay& replay);

/// False when the replay had to stop: the registers after this buffer are unknown.
bool ScanCommands(std::span<const u32> commands, u32 depth, ScanReplay& replay) {
    bool is_known = true;
    ForEachPacket(commands, [&](const AmdGpu::PM4Header* p_header) {
        is_known = ScanPacket(p_header, commands.data() + commands.size(), depth, replay);
        return is_known;
    });
    return is_known;
}

/// Applies one packet as the GPU thread would (nested indirect buffers followed, their state
/// packets recorded for the workers). False when the registers after it cannot be known ahead
/// of the GPU thread: a nested buffer that cannot be read or is nested too deep, or a COND_EXEC
/// whose skip would leave other registers (or draw ordinals) than running its packets, since
/// its condition is only known when the GPU thread gets there.
bool ScanPacket(const AmdGpu::PM4Header* p_header, const u32* p_end, u32 depth,
                ScanReplay& replay) {
    using AmdGpu::PM4ItOpcode;
    switch (p_header->type3.opcode) {
    case PM4ItOpcode::IndirectBuffer: {
        if (depth >= MaxNestedDepth) {
            return false;
        }
        const auto* p_ib = reinterpret_cast<const AmdGpu::PM4CmdIndirectBuffer*>(p_header);
        thread_local std::array<std::vector<u32>, MaxNestedDepth> s_nested;
        auto& words = s_nested[depth];
        if (!CopyGuestWords(p_ib->Address<const u32>(), p_ib->ib_size, words)) {
            return false;
        }
        replay.c_nested_followed.fetch_add(1, std::memory_order_relaxed);
        return ScanCommands(words, depth + 1, replay);
    }
    case PM4ItOpcode::CondExec: {
        const auto* p_cond = reinterpret_cast<const AmdGpu::PM4CmdCondExec*>(p_header);
        const u32* p_covered =
            reinterpret_cast<const u32*>(p_header) + p_header->type3.NumWords() + 1;
        const size_t c_covered =
            std::min<size_t>(p_cond->exec_count.Value(), static_cast<size_t>(p_end - p_covered));
        // The covered packets are applied here as if run: when that changes nothing, running or
        // skipping them is the same; otherwise the replay stops and the registers no longer matter.
        const u64 checksum_before = replay.checksum;
        bool has_effect = false;
        ForEachPacket(std::span<const u32>{p_covered, c_covered},
                      [&](const AmdGpu::PM4Header* p_covered_header) {
                          const auto opcode = p_covered_header->type3.opcode;
                          if (opcode == PM4ItOpcode::IndirectBuffer ||
                              opcode == PM4ItOpcode::CondExec) {
                              has_effect = true;
                              return false;
                          }
                          AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(
                              replay.regs, p_covered_header, replay.checksum, &replay.dirty);
                          return true;
                      });
        return !has_effect && replay.checksum == checksum_before;
    }
    default: {
        const u64 checksum_before = replay.checksum;
        AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(replay.regs, p_header, replay.checksum,
                                                       &replay.dirty);
        if (depth > 0 && replay.checksum != checksum_before) {
            const auto* p_words = reinterpret_cast<const u32*>(p_header);
            replay.nested_words.insert(replay.nested_words.end(), p_words,
                                       p_words + p_header->type3.NumWords() + 1);
        }
        return true;
    }
    }
}

u32 DefaultWorkerCount() {
    if (const char* sz_env = std::getenv("PS_PREP_WORKERS")) {
        return static_cast<u32>(std::clamp(std::atoi(sz_env), 0, 16));
    }
    // Workers are SCHED_IDLE and claim whole buffers, so more of them only use more idle
    // cores: half the hardware threads (Steam Deck 4, 16-thread desktop 8).
    return std::clamp<u32>(PsThreads::Available() / 2, 1, 8);
}

/// Flattened user data of one submission: chunks never move, so the GPU thread can read a
/// prepared draw's data while the worker keeps appending.
struct FlatArena {
    static constexpr size_t ChunkWords = 16 * 1024;
    std::vector<std::unique_ptr<u32[]>> m_chunks;
    size_t m_c_words_used = ChunkWords;

    const u32* Append(const std::vector<u32>& data) {
        return AppendArray(data.data(), data.size());
    }

    /// Trivially copyable elements, 8-byte aligned (sharps hold u64 bit fields).
    template <typename T>
    const T* AppendArray(const T* p_data, size_t c_elements) {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
        const size_t words = (c_elements * sizeof(T) + sizeof(u32) - 1) / sizeof(u32);
        m_c_words_used = (m_c_words_used + 1) & ~size_t(1);
        if (m_chunks.empty() || words > ChunkWords - std::min(m_c_words_used, ChunkWords)) {
            m_chunks.push_back(std::make_unique<u32[]>(std::max(ChunkWords, words)));
            m_c_words_used = 0;
        }
        u32* p_dst = m_chunks.back().get() + m_c_words_used;
        if (c_elements) {
            std::memcpy(p_dst, p_data, c_elements * sizeof(T));
        }
        m_c_words_used += words;
        return reinterpret_cast<const T*>(p_dst);
    }
};

/// Reads the stage's resource sharps from the worker's flattened user data, so the GPU thread
/// only binds them (the sharp fetches and texture-description hashing were ~7% of it).
void PrepareResources(const Shader::Info& info, PreparedStage& stage, FlatArena& arena) {
    thread_local std::vector<AmdGpu::Image> images;
    thread_local std::vector<u64> hashes;
    thread_local std::vector<AmdGpu::Sampler> samplers;
    thread_local std::vector<AmdGpu::Buffer> buffers;
    images.clear();
    hashes.clear();
    samplers.clear();
    buffers.clear();
    for (const auto& res : info.images) {
        images.push_back(res.GetSharp(info));
        hashes.push_back(ImageDescHash(images.back(), res));
    }
    for (const auto& res : info.samplers) {
        samplers.push_back(res.GetSharp(info));
    }
    for (const auto& res : info.buffers) {
        buffers.push_back(res.IsSpecial() ? AmdGpu::Buffer::Null() : res.GetSharp(info));
    }
    stage.p_image_sharps = arena.AppendArray(images.data(), images.size());
    stage.p_image_hashes = arena.AppendArray(hashes.data(), hashes.size());
    stage.p_sampler_sharps = arena.AppendArray(samplers.data(), samplers.size());
    stage.p_buffer_sharps = arena.AppendArray(buffers.data(), buffers.size());
    stage.c_images = static_cast<u32>(images.size());
    stage.c_samplers = static_cast<u32>(samplers.size());
    stage.c_buffers = static_cast<u32>(buffers.size());
}

/// Rasterizer::BindVertexBuffers' pure part (GraphicsPipeline::GetVertexInputs for the dynamic
/// vertex input path, and the merge of the stream memory into ranges).
void PrepareVertexInputs(const Shader::Info& vs, const Shader::Gcn::FetchShaderData& fetch,
                         const AmdGpu::Regs& regs, PreparedVertexInputs& out, FlatArena& arena) {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    thread_local std::vector<vk::VertexInputAttributeDescription2EXT> attributes;
    thread_local std::vector<vk::VertexInputBindingDescription2EXT> bindings;
    thread_local std::vector<AmdGpu::Buffer> buffers;
    thread_local std::vector<PreparedVertexInputs::Range> ranges, merged;
    thread_local std::vector<u8> range_index;
    attributes.clear();
    bindings.clear();
    buffers.clear();
    ranges.clear();
    merged.clear();
    range_index.clear();
    for (const auto& attrib : fetch.attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs);
        // The V# may come from user data the guest is still writing: an unknown format leaves
        // the inputs to the GPU thread, which reads them once the draw is submitted (the
        // assertion in SurfaceFormat stopped the process here).
        const vk::Format format =
            LiverpoolToVK::TrySurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        if (format == vk::Format::eUndefined &&
            buffer.GetDataFmt() != AmdGpu::DataFormat::FormatInvalid) {
            out.is_valid = false;
            return;
        }
        attributes.push_back({
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = format,
            .offset = 0,
        });
        bindings.push_back({
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
            .divisor = step_rate == InstanceIdType::OverStepRate0 ? regs.vgt_instance_step_rate_0
                       : step_rate == InstanceIdType::OverStepRate1
                           ? regs.vgt_instance_step_rate_1
                           : 1u,
        });
        buffers.push_back(buffer);
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.push_back({buffer.base_address, buffer.base_address + buffer.GetSize()});
        }
    }
    std::ranges::sort(ranges, {}, &PreparedVertexInputs::Range::base);
    for (const auto& range : ranges) {
        if (merged.empty() || merged.back().end < range.base) {
            merged.push_back(range);
        } else {
            merged.back().end = std::max(merged.back().end, range.end);
        }
    }
    for (const auto& buffer : buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto it = std::ranges::find_if(merged, [&](const auto& range) {
                return buffer.base_address >= range.base && buffer.base_address < range.end;
            });
            range_index.push_back(static_cast<u8>(it - merged.begin()));
        }
    }
    out.p_attributes = arena.AppendArray(attributes.data(), attributes.size());
    out.p_bindings = arena.AppendArray(bindings.data(), bindings.size());
    out.p_buffers = arena.AppendArray(buffers.data(), buffers.size());
    out.p_range_index = arena.AppendArray(range_index.data(), range_index.size());
    out.p_ranges = arena.AppendArray(merged.data(), merged.size());
    out.c_streams = static_cast<u32>(attributes.size());
    out.c_ranges = static_cast<u32>(merged.size());
    out.buffers_hash =
        buffers.empty() ? 0 : XXH3_64bits(buffers.data(), buffers.size() * sizeof(AmdGpu::Buffer));
    out.is_valid = true;
}

} // namespace

DrawPreparation::DrawPreparation(PipelineCache& pipeline_cache_)
    : m_pipeline_cache{pipeline_cache_} {
    m_c_workers = DefaultWorkerCount();
    if (m_c_workers) {
        m_scanner = std::jthread([this](std::stop_token stop) { ScannerLoop(stop); });
    }
    for (u32 i = 0; i < m_c_workers; ++i) {
        m_workers.emplace_back([this, i](std::stop_token stop) { WorkerLoop(stop, i); });
    }
    std::printf("GPU: draw preparation workers: %u (+1 scanner, %u hardware threads)\n",
                m_c_workers, PsThreads::Available());
}

DrawPreparation::~DrawPreparation() {
    for (auto& worker : m_workers) {
        worker.request_stop();
    }
    if (m_scanner.joinable()) {
        m_scanner.request_stop();
    }
    m_cv.notify_all();
    m_workers.clear();
    m_scanner = {};
}

std::shared_ptr<DrawPreparation::Submission> DrawPreparation::Build(
    std::span<const u32> commands) {
    if (!Enabled()) {
        return {};
    }
    auto submission = std::make_shared<Submission>();
    submission->commands =
        std::make_shared<const std::vector<u32>>(commands.begin(), commands.end());
    ForEachPacket(*submission->commands, [&](const AmdGpu::PM4Header* p_header) {
        submission->c_draws += IsDirectDraw(p_header->type3.opcode);
    });
    submission->draws = std::make_unique<PreparedDraw[]>(submission->c_draws);
    return submission;
}

void DrawPreparation::Enqueue(u64 seq, std::shared_ptr<Submission> submission) {
    if (!submission) {
        return;
    }
    submission->seq = seq;
    {
        std::scoped_lock lk{m_mutex};
        m_submissions.push_back(std::move(submission));
    }
    m_cv.notify_all();
}

void DrawPreparation::BeginSubmission(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum) {
    m_current_draw = 0;
    m_current.reset();
    if (!Enabled()) {
        return;
    }
    m_gpu_seq.store(seq, std::memory_order_release);
    std::scoped_lock lk{m_mutex};
    if (!m_is_baseline_ready) {
        m_tail_regs = std::make_unique<AmdGpu::Regs>(regs);
        m_baseline_seq = seq;
        m_is_baseline_ready = true;
        RebaseTail(seq, regs, reg_checksum);
    }
    // Idle helpers can starve on a busy CPU. Rather than queueing without bound, restart
    // them from the GPU thread's own state (one register copy) once the scanner lags.
    constexpr u64 MaxScanLag = 64;
    if (!m_submissions.empty() && seq > m_tail_seq + MaxScanLag &&
        m_submissions.front()->seq < seq && !m_submissions.front()->is_scanned.load()) {
        RebaseTail(seq, regs, reg_checksum);
        ++m_c_rebases;
    } else if (!m_is_tail_known && seq >= m_tail_seq) {
        RebaseTail(seq, regs, reg_checksum);
        ++m_c_unknown_resyncs;
    }
    const auto find_current = [&]() -> std::shared_ptr<Submission> {
        if (!m_submissions.empty() && seq >= m_submissions.front()->seq &&
            seq - m_submissions.front()->seq < m_submissions.size()) {
            return m_submissions[seq - m_submissions.front()->seq];
        }
        return {};
    };
    m_current = find_current();
    // The scanner's state drifted from the command processor's (a compute queue writing SH
    // registers in between, or anything else it cannot replay): every buffer it scanned from
    // here on was prepared with registers this thread will never have, and walking their
    // resource tables reads other draws' data as pointers. Fresh copies are scanned again from
    // this thread's registers; workers still holding the old ones write only to those.
    if (m_current && m_current->is_scanned.load(std::memory_order_acquire) &&
        m_current->start_checksum != reg_checksum) {
        for (auto& submission : m_submissions) {
            if (submission->seq < seq) {
                continue;
            }
            auto fresh = std::make_shared<Submission>();
            fresh->seq = submission->seq;
            fresh->commands = submission->commands;
            fresh->c_draws = submission->c_draws;
            fresh->draws = std::make_unique<PreparedDraw[]>(fresh->c_draws);
            submission = std::move(fresh);
        }
        RebaseTail(seq, regs, reg_checksum);
        ++m_c_checksum_resyncs;
        m_current = find_current();
    }
}

void DrawPreparation::RebaseTail(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum) {
    *m_tail_regs = regs;
    m_tail_seq = seq;
    m_tail_checksum = reg_checksum;
    m_is_tail_known = true;
    ++m_tail_epoch;
    while (!m_submissions.empty() && m_submissions.front()->seq < seq) {
        m_submissions.pop_front();
    }
    m_cv.notify_all();
}

void DrawPreparation::EndSubmission() {
    if (m_current) {
        m_current->is_gpu_done.store(true, std::memory_order_release);
        m_current.reset();
    }
    if (Enabled()) {
        Collect();
    }
}

const PreparedDraw* DrawPreparation::NextDraw() {
    if (!m_current || m_current_draw >= m_current->c_draws) {
        return nullptr;
    }
    const PreparedDraw* p_draw = &m_current->draws[m_current_draw++];
    if (PsToggle::Disabled(PsToggle::DrawPreparation)) {
        return nullptr;
    }
    return p_draw;
}

void DrawPreparation::Collect() {
    std::scoped_lock lk{m_mutex};
    while (!m_submissions.empty()) {
        const auto& front = m_submissions.front();
        if (!m_is_baseline_ready || !front->is_gpu_done.load(std::memory_order_acquire)) {
            break;
        }
        if (front->seq >= m_baseline_seq) {
            // Its delta moves the restart state to the start of the next buffer.
            if (!front->is_scanned.load(std::memory_order_acquire)) {
                break;
            }
            m_tail_seq = front->seq + 1;
            if (front->is_end_known) {
                front->delta.Apply(*m_tail_regs);
                m_tail_checksum = front->end_checksum;
            } else {
                m_is_tail_known = false;
            }
        }
        m_submissions.pop_front();
    }
}

void DrawPreparation::ScannerLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:DrawScan");
    PsThreads::MakeBackground();
    auto regs = std::make_unique<AmdGpu::Regs>();
    u64 checksum = 0;
    u64 next_seq = 0;
    // No epoch yet: the first wait loads the tail the GPU thread hands over.
    u64 epoch = ~0ull;
    // False after a buffer whose end state is unknown: only a rebase moves the scanner on.
    bool is_state_known = false;
    AmdGpu::RegDirty dirty;
    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> submission;
        {
            std::unique_lock lk{m_mutex};
            m_cv.wait(lk, stop, [&] {
                if (!m_is_baseline_ready) {
                    return false;
                }
                if (epoch != m_tail_epoch) {
                    return m_is_tail_known;
                }
                return is_state_known && !m_submissions.empty() &&
                       m_submissions.back()->seq >= next_seq;
            });
            if (stop.stop_requested()) {
                return;
            }
            if (epoch != m_tail_epoch) {
                // Rebased by the GPU thread: continue from its state.
                *regs = *m_tail_regs;
                checksum = m_tail_checksum;
                next_seq = m_tail_seq;
                epoch = m_tail_epoch;
                is_state_known = true;
                continue;
            }
            // Unscanned buffers are only collected by a rebase, so `next_seq` is still queued.
            submission = m_submissions[next_seq - m_submissions.front()->seq];
        }
        ZoneScopedNC("Draw scan", RendererMarkerColor);
        submission->start_checksum = checksum;
        submission->nested_words.clear();
        submission->nested_ends.clear();
        dirty.Clear();
        ScanReplay replay{*regs, checksum, dirty, submission->nested_words, m_c_nested_followed};
        u32 c_draws_known = 0;
        bool is_end_known = true;
        const auto& commands = *submission->commands;
        ForEachPacket(commands, [&](const AmdGpu::PM4Header* p_header) {
            if (!ScanPacket(p_header, commands.data() + commands.size(), 0, replay)) {
                is_end_known = false;
                return false;
            }
            const auto opcode = p_header->type3.opcode;
            if (opcode == AmdGpu::PM4ItOpcode::IndirectBuffer) {
                submission->nested_ends.push_back(
                    static_cast<u32>(submission->nested_words.size()));
            }
            c_draws_known += IsDirectDraw(opcode);
            return true;
        });
        submission->c_draws_known = c_draws_known;
        submission->is_end_known = is_end_known;
        if (is_end_known) {
            submission->delta.Capture(*regs, dirty);
            submission->end_checksum = checksum;
        } else {
            m_c_unknown_ends.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::scoped_lock lk{m_mutex};
            // A rebase while this buffer was scanned replaced or dropped it; the scan is stale.
            if (epoch == m_tail_epoch) {
                submission->is_scanned.store(true, std::memory_order_release);
            }
        }
        ++next_seq;
        is_state_known = is_end_known;
        m_cv.notify_all();
    }
}

void DrawPreparation::WorkerLoop(std::stop_token stop, u32 i_worker) {
    Common::SetCurrentThreadName(("bb:DrawPrep" + std::to_string(i_worker)).c_str());
    PsThreads::MakeBackground();
    auto regs = std::make_unique<AmdGpu::Regs>();
    constexpr u64 NoPosition = ~0ull;
    u64 position = NoPosition; ///< `regs` holds the state at the start of this buffer
    u64 epoch = 0;             ///< the tail epoch `position` belongs to
    PipelineSelection sel{};
    sel.regs = regs.get();
    PrepWorker prep_worker{};
    sel.worker = &prep_worker;
    FlatArena arena;
    std::vector<std::shared_ptr<Submission>> path;

    // The nearest scanned buffer the GPU thread has not reached and no worker has claimed.
    const auto claimable = [&]() -> std::shared_ptr<Submission> {
        const u64 gpu = m_gpu_seq.load(std::memory_order_acquire);
        for (const auto& submission : m_submissions) {
            if (submission->seq >= gpu && submission->seq >= m_tail_seq &&
                submission->is_scanned.load(std::memory_order_acquire) &&
                !submission->is_claimed.load(std::memory_order_relaxed)) {
                return submission;
            }
        }
        return {};
    };

    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> target;
        {
            std::unique_lock lk{m_mutex};
            // m_gpu_seq moves without the lock: keep the candidate the predicate found.
            m_cv.wait(lk, stop, [&] {
                target = m_is_baseline_ready && m_is_tail_known ? claimable() : nullptr;
                return target != nullptr;
            });
            if (stop.stop_requested() || !target) {
                return;
            }
            target->is_claimed.store(true, std::memory_order_relaxed);
            // Buffers between this worker's state and the target: their deltas. A position
            // already collected (or past the target, or from before a rebase) restarts from the
            // queue's tail state. Every queued scanned buffer belongs to the current epoch, and
            // none follows one whose end state is unknown (the scanner waits for a rebase there),
            // so every delta on the path is valid.
            if (position == NoPosition || epoch != m_tail_epoch || position < m_tail_seq ||
                position > target->seq) {
                *regs = *m_tail_regs;
                position = m_tail_seq;
                epoch = m_tail_epoch;
            }
            const u64 front = m_submissions.front()->seq;
            path.clear();
            for (u64 seq = position; seq < target->seq; ++seq) {
                path.push_back(m_submissions[seq - front]);
            }
        }
        ZoneScopedNC("Draw prep", RendererMarkerColor);
        for (const auto& step : path) {
            step->delta.Apply(*regs);
        }
        path.clear();
        u64 checksum = target->start_checksum;
        u32 ordinal = 0;
        size_t i_nested = 0;
        u32 off_nested = 0;
        arena = FlatArena{};
        ForEachPacket(*target->commands, [&](const AmdGpu::PM4Header* p_header) {
            // Past the scanner's last known state the registers are not this thread's to guess.
            if (!target->is_end_known && ordinal >= target->c_draws_known) {
                return false;
            }
            if (p_header->type3.opcode == AmdGpu::PM4ItOpcode::IndirectBuffer) {
                if (i_nested >= target->nested_ends.size()) {
                    return false;
                }
                const u32 off_end = target->nested_ends[i_nested++];
                ForEachPacket(std::span<const u32>{target->nested_words}.subspan(
                                  off_nested, off_end - off_nested),
                              [&](const AmdGpu::PM4Header* p_nested) {
                                  AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(*regs, p_nested,
                                                                                 checksum);
                              });
                off_nested = off_end;
                return true;
            }
            AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(*regs, p_header, checksum);
            if (!IsDirectDraw(p_header->type3.opcode)) {
                return true;
            }
            // Past the GPU thread the draws are no longer worth preparing.
            if (ordinal < target->c_draws &&
                target->seq >= m_gpu_seq.load(std::memory_order_acquire)) {
                auto& draw = target->draws[ordinal];
                prep_worker.stages.clear();
                prep_worker.failed = false;
                sel.draw_indirect_params = {};
                if (m_pipeline_cache.PrepareGraphicsPipeline(sel)) {
                    draw.reg_checksum = checksum;
                    draw.key = sel.graphics_key;
                    draw.c_stages = static_cast<u32>(prep_worker.stages.size());
                    for (u32 i = 0; i < draw.c_stages; ++i) {
                        const auto& stage = prep_worker.stages[i];
                        draw.stages[i] = {
                            .p_program = stage.program,
                            .hash = stage.hash,
                            .hw_stage = stage.hw_stage,
                            .pgm_base = stage.pgm_base,
                            .p_flat = arena.Append(*stage.flat),
                            .c_flat = static_cast<u32>(stage.flat->size()),
                        };
                        PrepareResources(prep_worker.infos.at(stage.program), draw.stages[i],
                                         arena);
                    }
                    draw.vertex.is_valid = false;
                    const auto* p_vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
                    if (p_vs && sel.fetch_shader) {
                        PrepareVertexInputs(*p_vs, *sel.fetch_shader, *regs, draw.vertex, arena);
                    }
                    draw.state.store(PreparedDraw::Ready, std::memory_order_release);
                } else {
                    draw.state.store(PreparedDraw::Unavailable, std::memory_order_release);
                }
            }
            ++ordinal;
            return true;
        });
        // The arena's chunks live as long as the submission.
        target->flat_chunks = std::move(arena.m_chunks);
        position = target->is_end_known ? target->seq + 1 : NoPosition;
    }
}

void DrawPreparation::Count(bool was_used) {
    ++(was_used ? m_c_used : m_c_unused);
    static const bool s_should_count = std::getenv("PS_FRAME_STATS") != nullptr;
    // The clock is read every 1024 draws: per draw it was 8% of the GPU thread.
    if (!s_should_count || ((m_c_used + m_c_unused) & 1023) != 0) {
        return;
    }
    static auto s_window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - s_window < std::chrono::seconds(5)) {
        return;
    }
    s_window = now;
    std::printf("Draw preparation: %llu prepared pipelines used, %llu not (%u workers, "
                "%llu scanner rebases, %llu resyncs after a buffer with unknown end state, "
                "%llu after a register checksum mismatch; %llu nested buffers replayed, "
                "%llu buffers with unknown end state; %llu speculative walks abandoned)\n",
                static_cast<unsigned long long>(m_c_used), static_cast<unsigned long long>(m_c_unused),
                m_c_workers, static_cast<unsigned long long>(m_c_rebases),
                static_cast<unsigned long long>(m_c_unknown_resyncs),
                static_cast<unsigned long long>(m_c_checksum_resyncs),
                static_cast<unsigned long long>(
                    m_c_nested_followed.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    m_c_unknown_ends.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(PsSrtFaults::AbandonedWalks()));
    m_c_used = m_c_unused = 0;
}

} // namespace Vulkan
