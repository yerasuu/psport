// SPDX-License-Identifier: GPL-2.0-or-later
// psport: speculative draw preparation on worker threads (see vk_draw_prep.h).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "psport_threads.h"
#include "psport_toggles.h"
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

/// Walks the type-3 packets of a command buffer (type-2 padding skipped).
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
        const size_t words = p_header->type3.NumWords() + 1;
        if (at + words > commands.size()) {
            return;
        }
        func(p_header);
        at += words;
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
    submission->commands.assign(commands.begin(), commands.end());
    ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* p_header) {
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
        m_initial_regs = std::make_unique<AmdGpu::Regs>(regs);
        m_tail_regs = std::make_unique<AmdGpu::Regs>(regs);
        m_initial_checksum = m_tail_checksum = reg_checksum;
        m_baseline_seq = m_tail_seq = seq;
        m_is_baseline_ready = true;
        m_cv.notify_all();
    }
    // Idle helpers can starve on a busy CPU. Rather than queueing without bound, restart
    // them from the GPU thread's own state (one register copy) once the scanner lags.
    constexpr u64 MaxScanLag = 64;
    if (!m_submissions.empty() && seq > m_tail_seq + MaxScanLag &&
        m_submissions.front()->seq < seq && !m_submissions.front()->is_scanned.load()) {
        *m_tail_regs = regs;
        m_tail_seq = seq;
        m_tail_checksum = reg_checksum;
        while (!m_submissions.empty() && m_submissions.front()->seq < seq) {
            m_submissions.pop_front();
        }
        ++m_c_rebases;
        m_cv.notify_all();
    }
    if (!m_submissions.empty() && seq >= m_submissions.front()->seq &&
        seq - m_submissions.front()->seq < m_submissions.size()) {
        m_current = m_submissions[seq - m_submissions.front()->seq];
    }
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
            front->delta.Apply(*m_tail_regs);
            m_tail_seq = front->seq + 1;
            m_tail_checksum = front->end_checksum;
        }
        m_submissions.pop_front();
    }
}

void DrawPreparation::ScannerLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:DrawScan");
    PsThreads::MakeBackground();
    std::unique_ptr<AmdGpu::Regs> regs;
    u64 checksum = 0;
    u64 next_seq = 0;
    {
        std::unique_lock lk{m_mutex};
        m_cv.wait(lk, stop, [&] { return m_is_baseline_ready; });
        if (stop.stop_requested()) {
            return;
        }
        regs = std::make_unique<AmdGpu::Regs>(*m_initial_regs);
        checksum = m_initial_checksum;
        next_seq = m_baseline_seq;
    }
    AmdGpu::RegDirty dirty;
    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> submission;
        {
            std::unique_lock lk{m_mutex};
            m_cv.wait(lk, stop, [&] {
                return next_seq < m_tail_seq ||
                       (!m_submissions.empty() && m_submissions.back()->seq >= next_seq);
            });
            if (stop.stop_requested()) {
                return;
            }
            if (next_seq < m_tail_seq) {
                // Rebased by the GPU thread: continue from its state.
                *regs = *m_tail_regs;
                checksum = m_tail_checksum;
                next_seq = m_tail_seq;
                continue;
            }
            // Unscanned buffers are only collected by a rebase, so `next_seq` is still queued.
            submission = m_submissions[next_seq - m_submissions.front()->seq];
        }
        submission->start_checksum = checksum;
        dirty.Clear();
        ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* p_header) {
            AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(*regs, p_header, checksum, &dirty);
        });
        submission->delta.Capture(*regs, dirty);
        submission->end_checksum = checksum;
        {
            std::scoped_lock lk{m_mutex};
            submission->is_scanned.store(true, std::memory_order_release);
        }
        ++next_seq;
        m_cv.notify_all();
    }
}

void DrawPreparation::WorkerLoop(std::stop_token stop, u32 i_worker) {
    Common::SetCurrentThreadName(("bb:DrawPrep" + std::to_string(i_worker)).c_str());
    PsThreads::MakeBackground();
    auto regs = std::make_unique<AmdGpu::Regs>();
    constexpr u64 NoPosition = ~0ull;
    u64 position = NoPosition; ///< `regs` holds the state at the start of this buffer
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
            // gpu_seq moves without the lock: keep the candidate the predicate found.
            m_cv.wait(lk, stop, [&] {
                target = m_is_baseline_ready ? claimable() : nullptr;
                return target != nullptr;
            });
            if (stop.stop_requested() || !target) {
                return;
            }
            target->is_claimed.store(true, std::memory_order_relaxed);
            // Buffers between this worker's state and the target: their deltas. A position
            // already collected (or past the target) restarts from the queue's tail state.
            if (position == NoPosition || position < m_tail_seq || position > target->seq) {
                *regs = *m_tail_regs;
                position = m_tail_seq;
            }
            const u64 front = m_submissions.front()->seq;
            path.clear();
            for (u64 seq = position; seq < target->seq; ++seq) {
                path.push_back(m_submissions[seq - front]);
            }
        }
        for (const auto& step : path) {
            step->delta.Apply(*regs);
        }
        path.clear();
        u64 checksum = target->start_checksum;
        u32 ordinal = 0;
        arena = FlatArena{};
        ForEachPacket(target->commands, [&](const AmdGpu::PM4Header* p_header) {
            AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(*regs, p_header, checksum);
            if (!IsDirectDraw(p_header->type3.opcode)) {
                return;
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
        });
        // The arena's chunks live as long as the submission.
        target->flat_chunks = std::move(arena.m_chunks);
        position = target->seq + 1;
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
                "%llu scanner rebases)\n",
                static_cast<unsigned long long>(m_c_used), static_cast<unsigned long long>(m_c_unused),
                m_c_workers, static_cast<unsigned long long>(m_c_rebases));
    m_c_used = m_c_unused = 0;
}

} // namespace Vulkan
