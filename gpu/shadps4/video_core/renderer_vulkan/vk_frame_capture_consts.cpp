// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture_consts.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <xxhash.h>

#include "core/memory.h"
#include "shader_recompiler/info.h"

namespace Vulkan::CaptureConsts {

namespace {

using WriteSource = FrameCapture::WriteSource;
using Clock = std::chrono::steady_clock;

constexpr u32 CB_HEAD = 256;
constexpr u64 CB_HASHED_MAX = 16ull << 20;
constexpr u64 CB_GPU_COPY_MAX = 1ull << 20;
constexpr size_t C_DRAWS_MAX = 64;
constexpr size_t C_SOURCES_MAX = 64;
constexpr size_t C_JOURNAL_MAX = 1 << 16;
constexpr size_t CB_TEXT_MAX = 128 << 10;
constexpr u32 C_WORDS_PER_ROW = 8;
constexpr size_t C_SUMMARY_ROWS_MAX = 512;
constexpr u32 NO_PASS = ~0u;

struct JournalEntry {
    VAddr va_range{};
    u64 cb_range{};
    WriteSource source{};
    u64 detail{};
    u64 tick{};
    u32 i_pass{};
    u32 i_draw{};
    Clock::time_point time;
};

struct UserDataBlock {
    Shader::SwStage stage{};
    u64 stage_hash{};
    std::vector<u32> words;
    u32 ud_mask{};
    u32 i_first_push{};
};

struct Source {
    Shader::SwStage stage{};
    u64 stage_hash{};
    u32 i_slot{};
    u32 i_resource{};
    const char* sz_route{};
    VAddr va_buffer{};
    u64 cb_buffer{};
    u32 cb_stride{};
    u32 memory{};
    std::string skipped;
    std::vector<u8> head; ///< the first bytes as bound
    u64 cb_hashed{};
    u64 hash{};
    bool is_copy_requested{};
    CaptureDraw::GpuRange gpu;
};

struct DrawRecord {
    u32 i_pass{};
    u32 i_draw{};
    u64 vs_hash{};
    u64 ps_hash{};
    u32 i_pair_draw{};
    PsSkipDraws::Target color0{};
    bool is_skipped{};
    u64 tick{};
    size_t i_journal{}; ///< journal entries before it were journaled before it was recorded
    Clock::time_point time;
    std::vector<UserDataBlock> user_data;
    std::vector<Source> sources;
};

struct PendingWrite {
    VAddr va_range{};
    u64 cb_range{};
    WriteSource source{};
};

// Any thread.
std::mutex s_journal_mutex;
std::vector<JournalEntry> s_journal;
u64 s_c_journal_dropped = 0;
std::atomic<u64> s_position{u64(NO_PASS) << 32};

// GPU thread only.
bool s_is_candidate = false;
bool s_is_compute = false;
std::vector<UserDataBlock> s_pending_user_data;
std::vector<Source> s_pending_sources;
u32 s_c_pending_over = 0;
std::vector<PendingWrite> s_pending_writes;
std::vector<DrawRecord> s_draws;
u32 s_c_matches = 0;
u32 s_c_draws_over = 0;
std::map<std::string, u32> s_not_dumped;
Clock::time_point s_frame_start;

const char* StageName(Shader::SwStage stage) {
    switch (stage) {
    case Shader::SwStage::Vertex:
        return "vs";
    case Shader::SwStage::Fragment:
        return "ps";
    default:
        return "stage";
    }
}

const char* SourceName(WriteSource source) {
    switch (source) {
    case WriteSource::DrawBinding:
        return "draw (storage buffer it writes)";
    case WriteSource::DispatchBinding:
        return "dispatch (storage buffer it writes)";
    case WriteSource::StorageImage:
        return "draw or dispatch (storage image it writes)";
    case WriteSource::RenderTarget:
        return "render target of a pass";
    case WriteSource::DmaFill:
        return "DMA_DATA/COPY_DATA fill";
    case WriteSource::DmaCopy:
        return "DMA_DATA/COPY_DATA/RELEASE_MEM copy";
    case WriteSource::CommandWrite:
        return "command processor write on the CPU (WRITE_DATA, CE dump)";
    case WriteSource::GpuCommandWrite:
        return "WRITE_DATA by the GPU in stream order";
    case WriteSource::GpuLabel:
        return "EOP/EOS/RELEASE_MEM label by the GPU";
    case WriteSource::LateLabel:
        return "fence or label value, written once the GPU is past it";
    case WriteSource::Timestamp:
        return "GPU timestamp";
    case WriteSource::CpuWriteFault:
        return "CPU write fault (first write to a tracked page)";
    case WriteSource::CpuWriteNote:
        return "CPU write notice";
    }
    return "unknown source";
}

bool IsGpuSide(WriteSource source) {
    return source != WriteSource::CpuWriteFault && source != WriteSource::CpuWriteNote;
}

void CountNotDumped(const std::string& reason) {
    ++s_not_dumped[reason];
}

std::string FormatWord(u32 word) {
    const float value = std::bit_cast<float>(word);
    const float magnitude = std::fabs(value);
    if (word == 0) {
        return "0";
    }
    if (std::isfinite(value) && magnitude >= 1e-6f && magnitude <= 1e7f) {
        return std::format("{:.7g}", value);
    }
    return std::format("0x{:08x}", word);
}

u32 LoadWord(std::span<const u8> bytes, size_t i_word) {
    u32 word = 0;
    if ((i_word + 1) * 4 <= bytes.size()) {
        std::memcpy(&word, bytes.data() + i_word * 4, 4);
    }
    return word;
}

std::string Rows(std::span<const u8> bytes, const char* sz_indent) {
    std::string text;
    const size_t c_words = bytes.size() / 4;
    for (size_t i = 0; i < c_words; ++i) {
        if (i % C_WORDS_PER_ROW == 0) {
            text += std::format("{}{}[{:3}]", i ? "\n" : "", sz_indent, i);
        }
        text += std::format(" {:>12}", FormatWord(LoadWord(bytes, i)));
    }
    return text + "\n";
}

std::string DescribeTarget(const PsSkipDraws::Target& target) {
    return target.va_target
               ? std::format("{}x{} {:#x}", target.width, target.height, target.va_target)
               : std::string{"-"};
}

std::string DescribeMemory(u32 memory) {
    if (memory & FrameCapture::InPlace) {
        return std::format("in place (the GPU reads the game's memory when it runs the draw; CPU "
                           "writes there are not tracked){}",
                           (memory & FrameCapture::VramMirror) ? ", VRAM mirror" : "");
    }
    return std::format("copy in a GPU buffer; CPU-modified since its last upload: {}; "
                       "GPU-modified: {}{}",
                       (memory & FrameCapture::CpuModified) ? "yes" : "no",
                       (memory & FrameCapture::GpuModified) ? "yes" : "no",
                       (memory & FrameCapture::VramMirror) ? "; VRAM mirror (PS_LAYER_MEMORY)"
                                                           : "");
}

std::string DescribeEntry(const JournalEntry& entry, Clock::time_point reference) {
    const long long us =
        std::chrono::duration_cast<std::chrono::microseconds>(entry.time - reference).count();
    std::string text = SourceName(entry.source);
    if (entry.source == WriteSource::DmaCopy && entry.detail) {
        text += std::format(" from {:#x}", entry.detail);
    } else if (entry.source == WriteSource::CpuWriteFault && entry.detail) {
        text += std::format(" by guest rip {:#x}", entry.detail);
    }
    text += std::format(" at {:#x}+{}", entry.va_range, entry.cb_range);
    if (entry.i_pass == NO_PASS) {
        text += ", before the recorded frame";
    } else {
        text += std::format(", {} pass #{} draw {}",
                            IsGpuSide(entry.source) ? "at" : "while the GPU side was at",
                            entry.i_pass, entry.i_draw);
    }
    if (entry.tick) {
        text += std::format(", tick {}", entry.tick);
    }
    return text + std::format(", {:+} us from the draw's recording", us);
}

/// Who wrote [va, va + cb) as far as the journal and the buffer cache tell.
std::string DescribeWriters(const DrawRecord& draw, const Source& source,
                            const std::vector<JournalEntry>& journal) {
    if (!source.va_buffer) {
        return "      writer: not guest memory: SGPRs the command buffer set (SET_SH_REG) and what "
               "the SRT walker loaded through them\n";
    }
    const VAddr va_end = source.va_buffer + std::max<u64>(source.cb_buffer, 1);
    const JournalEntry* p_last_before = nullptr;
    const JournalEntry* p_first_after = nullptr;
    u32 c_before = 0;
    u32 c_after = 0;
    for (size_t i = 0; i < journal.size(); ++i) {
        const auto& entry = journal[i];
        if (entry.va_range >= va_end ||
            source.va_buffer >= entry.va_range + std::max<u64>(entry.cb_range, 1)) {
            continue;
        }
        if (i < draw.i_journal) {
            ++c_before;
            p_last_before = &entry;
        } else {
            ++c_after;
            p_first_after = p_first_after ? p_first_after : &entry;
        }
    }
    std::string text = std::format("      writes journaled into it: {} before the draw was "
                                   "recorded, {} after it to the frame's end\n",
                                   c_before, c_after);
    if (p_last_before) {
        text += std::format("        last before: {}\n", DescribeEntry(*p_last_before, draw.time));
    }
    if (p_first_after) {
        text += std::format("        first after: {}\n", DescribeEntry(*p_first_after, draw.time));
    }
    if (p_last_before) {
        return text + std::format("      writer: {}{}\n", SourceName(p_last_before->source),
                                  IsGpuSide(p_last_before->source)
                                      ? ""
                                      : " (later CPU writes to the same pages fault no more "
                                        "until the buffer cache protects them again)");
    }
    if (source.memory & FrameCapture::InPlace) {
        return text + "      writer: unknown: no GPU-side write journaled, and CPU writes to "
                      "memory bound in place are not tracked\n";
    }
    if (source.memory & FrameCapture::CpuModified) {
        return text + "      writer: the CPU, since its last upload (its pages were CPU-modified "
                      "when bound); when and from which code is unknown: nothing about it was "
                      "journaled since the capture was armed\n";
    }
    if (source.memory & FrameCapture::GpuModified) {
        return text + "      writer: the GPU (the range was GPU-modified when bound), before the "
                      "capture was armed; which work is unknown\n";
    }
    return text + "      writer: unknown: written before the capture was armed (no CPU write since "
                  "its last upload, no GPU write journaled)\n";
}

/// The bytes a source shows: the GPU copy when it landed, else as bound.
std::span<const u8> ShownBytes(const Source& source, const u8* p_gpu_copies) {
    if (p_gpu_copies && source.gpu.off_data != CaptureDraw::NOT_COPIED) {
        return {p_gpu_copies + source.gpu.off_data,
                std::min<size_t>(source.gpu.cb_copy, CB_HEAD)};
    }
    return source.head;
}

std::string DescribeSource(const DrawRecord& draw, const Source& source, const u8* p_gpu_copies,
                           const std::vector<JournalEntry>& journal) {
    std::string text = std::format(
        "  {} {:016x} buffer {} (binding {}, {}): address {:#x} size {} stride {}\n",
        StageName(source.stage), source.stage_hash, source.i_resource, source.i_slot,
        source.sz_route ? source.sz_route : "-", source.va_buffer, source.cb_buffer,
        source.cb_stride);
    if (source.va_buffer) {
        text += std::format("      memory when bound: {}\n", DescribeMemory(source.memory));
    }
    if (!source.skipped.empty()) {
        return text + std::format("      not dumped: {}\n", source.skipped) +
               DescribeWriters(draw, source, journal);
    }
    text += std::format("      as bound: {} of {} B, hash {:016x} over {} B\n", source.head.size(),
                        source.cb_buffer, source.hash, source.cb_hashed);
    text += Rows(source.head, "        ");
    if (source.gpu.h_buffer) {
        if (source.gpu.off_data == CaptureDraw::NOT_COPIED || !p_gpu_copies) {
            text += std::format("      gpu at the pass's end: not copied ({})\n",
                                source.gpu.skipped.empty() ? "not reached" : source.gpu.skipped);
        } else {
            const u8* p_copy = p_gpu_copies + source.gpu.off_data;
            const u64 hash = XXH3_64bits(p_copy, source.gpu.cb_copy);
            const std::span<const u8> head{p_copy,
                                           std::min<size_t>(source.gpu.cb_copy, CB_HEAD)};
            const bool is_head_same = std::ranges::equal(head, source.head);
            const bool is_same =
                is_head_same && source.gpu.cb_copy == source.cb_hashed && hash == source.hash;
            text += std::format("      gpu at the pass's end: {} B, hash {:016x}: {}\n",
                                source.gpu.cb_copy, hash,
                                is_same        ? "the same bytes as bound"
                                : is_head_same ? "differs from the bytes as bound after its first "
                                                 "bytes (or covers less of it)"
                                               : "differs from the bytes as bound:");
            if (!is_head_same) {
                text += Rows(head, "        ");
            }
        }
    }
    return text + DescribeWriters(draw, source, journal);
}

std::string DescribeUserData(const UserDataBlock& block) {
    std::string pushed;
    u32 i_push = block.i_first_push;
    for (u32 mask = block.ud_mask; mask; mask &= mask - 1) {
        pushed += std::format(" s{}->ud_regs[{}]", std::countr_zero(mask), i_push++);
    }
    std::string text = std::format(
        "  {} {:016x} user data: {} SGPRs; read by the shader as push constants:{}\n",
        StageName(block.stage), block.stage_hash, block.words.size(),
        pushed.empty() ? " none" : pushed);
    for (size_t i = 0; i < block.words.size(); i += C_WORDS_PER_ROW) {
        std::string raw;
        std::string floats;
        for (size_t j = i; j < std::min(block.words.size(), i + C_WORDS_PER_ROW); ++j) {
            raw += std::format("   0x{:08x}", block.words[j]);
            floats += std::format(" {:>12.6g}", std::bit_cast<float>(block.words[j]));
        }
        text += std::format("    s{:<2} raw  {}\n    s{:<2} f32  {}\n", i, raw, i, floats);
    }
    return text;
}

/// Words that differ between the selected draws, per stage buffer and user data.
std::string Summarize(const u8* p_gpu_copies) {
    if (s_draws.size() < 2) {
        return {};
    }
    struct Values {
        std::string label;
        std::vector<std::optional<std::vector<u32>>> per_draw;
        std::vector<std::optional<VAddr>> addresses;
    };
    std::map<std::pair<u32, u32>, Values> keys;
    const size_t c_draws = s_draws.size();
    const auto values_of = [&](u32 stage, u32 i_key, std::string label) -> Values& {
        auto& values = keys[{stage, i_key}];
        if (values.per_draw.empty()) {
            values.label = std::move(label);
            values.per_draw.resize(c_draws);
            values.addresses.resize(c_draws);
        }
        return values;
    };
    for (size_t d = 0; d < c_draws; ++d) {
        for (const auto& block : s_draws[d].user_data) {
            values_of(u32(block.stage), ~0u, std::format("{} user data", StageName(block.stage)))
                .per_draw[d] = block.words;
        }
        for (const auto& source : s_draws[d].sources) {
            auto& values =
                values_of(u32(source.stage), source.i_resource,
                          std::format("{} buffer {} (binding {})", StageName(source.stage),
                                      source.i_resource, source.i_slot));
            const auto bytes = ShownBytes(source, p_gpu_copies);
            std::vector<u32> words(bytes.size() / 4);
            for (size_t i = 0; i < words.size(); ++i) {
                words[i] = LoadWord(bytes, i);
            }
            values.per_draw[d] = std::move(words);
            values.addresses[d] = source.va_buffer;
        }
    }
    std::string text = "\ndifferences between the selected draws (buffers: the gpu copy when it "
                       "landed, else as bound; first 256 B):\n  draws:";
    for (size_t d = 0; d < c_draws; ++d) {
        text += std::format(" d{}=#{}.{}", d, s_draws[d].i_pass, s_draws[d].i_draw);
    }
    text += "\n";
    size_t c_rows = 0;
    size_t c_rows_omitted = 0;
    const auto add_row = [&](const std::string& row) {
        if (c_rows++ < C_SUMMARY_ROWS_MAX) {
            text += row;
        } else {
            ++c_rows_omitted;
        }
    };
    for (const auto& [key, values] : keys) {
        const bool is_address_same =
            std::ranges::all_of(values.addresses, [&](const auto& va) {
                return va == values.addresses.front();
            });
        if (!is_address_same) {
            std::string row = std::format("  {} address:", values.label);
            for (size_t d = 0; d < c_draws; ++d) {
                row += values.addresses[d] ? std::format(" d{} {:#x}", d, *values.addresses[d])
                                           : std::format(" d{} -", d);
            }
            add_row(row + "\n");
        }
        size_t c_words = 0;
        for (const auto& words : values.per_draw) {
            c_words = std::max(c_words, words ? words->size() : 0);
        }
        for (size_t i = 0; i < c_words; ++i) {
            std::optional<std::optional<u32>> first;
            bool is_different = false;
            for (const auto& words : values.per_draw) {
                if (!words) {
                    continue;
                }
                const std::optional<u32> word =
                    i < words->size() ? std::optional<u32>{(*words)[i]} : std::nullopt;
                if (!first) {
                    first = word;
                } else {
                    is_different |= *first != word;
                }
            }
            if (!is_different) {
                continue;
            }
            std::string row = std::format("  {} word [{}]:", values.label, i);
            for (size_t d = 0; d < c_draws; ++d) {
                const auto& words = values.per_draw[d];
                row += words && i < words->size()
                           ? std::format(" d{} {}", d, FormatWord((*words)[i]))
                           : std::format(" d{} -", d);
            }
            add_row(row + "\n");
        }
    }
    if (c_rows == 0) {
        text += "  none: every word shown is the same in every selected draw\n";
    }
    if (c_rows_omitted) {
        text += std::format("  {} more rows not listed\n", c_rows_omitted);
    }
    return text;
}

void ClearPending() {
    s_pending_user_data.clear();
    s_pending_sources.clear();
    s_c_pending_over = 0;
}

void JournalPending(u32 i_pass, u32 i_draw, u64 tick) {
    if (s_pending_writes.empty()) {
        return;
    }
    const auto now = Clock::now();
    std::scoped_lock lk{s_journal_mutex};
    for (const auto& write : s_pending_writes) {
        if (s_journal.size() >= C_JOURNAL_MAX) {
            ++s_c_journal_dropped;
            continue;
        }
        s_journal.push_back({write.va_range, write.cb_range, write.source, 0, tick, i_pass,
                             i_draw, now});
    }
    s_pending_writes.clear();
}

} // namespace

const PsSkipDraws::Selector* Selector() {
    static const std::optional<PsSkipDraws::Selector> s_selector =
        []() -> std::optional<PsSkipDraws::Selector> {
        const char* sz_value = std::getenv("PS_CAPTURE_DUMP_VSCONST");
        if (!sz_value || !sz_value[0]) {
            return std::nullopt;
        }
        PsSkipDraws::Selector selector{};
        if (const char* sz_why = PsSkipDraws::ParseSelector(sz_value, selector)) {
            std::printf("Frame capture: PS_CAPTURE_DUMP_VSCONST='%s' ignored: %s\n", sz_value,
                        sz_why);
            return std::nullopt;
        }
        std::printf("Frame capture: PS_CAPTURE_DUMP_VSCONST dumps the stage constants of %s\n",
                    selector.text.c_str());
        return selector;
    }();
    return s_selector ? &*s_selector : nullptr;
}

void ClearJournal() {
    std::scoped_lock lk{s_journal_mutex};
    s_journal.clear();
    s_c_journal_dropped = 0;
}

void StartFrame() {
    s_draws.clear();
    s_c_matches = 0;
    s_c_draws_over = 0;
    s_not_dumped.clear();
    s_frame_start = Clock::now();
    ClearPending();
    s_pending_writes.clear();
}

void SetPosition(u32 i_pass, u32 i_draw) {
    s_position.store((u64(i_pass) << 32) | i_draw, std::memory_order_relaxed);
}

void BeginBindings(bool is_recording, bool is_compute, u64 vs_hash, u64 ps_hash) {
    ClearPending();
    s_pending_writes.clear();
    s_is_compute = is_compute;
    const PsSkipDraws::Selector* p_selector = Selector();
    s_is_candidate = is_recording && !is_compute && p_selector &&
                     (p_selector->is_any_vs || p_selector->vs_hash == vs_hash) &&
                     (p_selector->is_any_ps || p_selector->ps_hash == ps_hash);
}

bool IsCandidate(const Shader::Info& stage) {
    return s_is_candidate &&
           (stage.sw_stage == Shader::SwStage::Vertex ||
            stage.sw_stage == Shader::SwStage::Fragment);
}

void NoteUserData(const Shader::Info& stage, u32 i_first_push) {
    if (!IsCandidate(stage)) {
        return;
    }
    const auto words = stage.UserData();
    s_pending_user_data.push_back({.stage = stage.sw_stage,
                                   .stage_hash = stage.pgm_hash,
                                   .words = {words.begin(), words.end()},
                                   .ud_mask = stage.ud_mask.mask,
                                   .i_first_push = i_first_push});
}

void NoteBuffer(const Shader::Info& stage, const FrameCapture::StageBuffer& buffer) {
    if (!IsCandidate(stage)) {
        return;
    }
    if (s_pending_sources.size() >= C_SOURCES_MAX) {
        ++s_c_pending_over;
        return;
    }
    Source source{.stage = stage.sw_stage,
                  .stage_hash = stage.pgm_hash,
                  .i_slot = buffer.i_slot,
                  .i_resource = buffer.i_resource,
                  .sz_route = buffer.sz_route,
                  .va_buffer = buffer.va_buffer,
                  .cb_buffer = buffer.cb_buffer,
                  .cb_stride = buffer.cb_stride,
                  .memory = buffer.memory};
    if (buffer.sz_skipped) {
        source.skipped = buffer.sz_skipped;
    } else if (buffer.p_bytes) {
        const auto* p_bytes = static_cast<const u8*>(buffer.p_bytes);
        source.head.assign(p_bytes, p_bytes + std::min<u64>(buffer.cb_buffer, CB_HEAD));
        source.cb_hashed = std::min(buffer.cb_buffer, CB_HASHED_MAX);
        source.hash = XXH3_64bits(p_bytes, source.cb_hashed);
    } else {
        auto* p_memory = Core::Memory::Instance();
        const u64 cb_mapped = p_memory->ClampRangeSize(buffer.va_buffer, buffer.cb_buffer);
        if (cb_mapped == 0) {
            source.skipped = "not mapped in the game's memory";
        } else {
            // Through the copy path the constant ring uses: it reads past the GPU's read
            // protection (PS_READBACKS=2) instead of faulting on the recording thread.
            std::vector<u8> bytes(std::min(cb_mapped, CB_HASHED_MAX));
            p_memory->CopySparseMemory(buffer.va_buffer, bytes.data(), bytes.size());
            source.cb_hashed = bytes.size();
            source.hash = XXH3_64bits(bytes.data(), bytes.size());
            bytes.resize(std::min<size_t>(bytes.size(), CB_HEAD));
            source.head = std::move(bytes);
        }
    }
    if (source.skipped.empty() && buffer.h_buffer) {
        source.gpu = {.h_buffer = buffer.h_buffer,
                      .off_buffer = buffer.off_buffer,
                      .cb_copy = std::min(buffer.cb_buffer, CB_GPU_COPY_MAX) & ~u64(3),
                      .cb_full = buffer.cb_buffer};
        if (!source.gpu.cb_copy) {
            source.gpu.skipped = "under 4 bytes";
        }
    }
    s_pending_sources.push_back(std::move(source));
}

void NotePendingWrite(VAddr va_range, u64 cb_range, FrameCapture::WriteSource source) {
    if (!Selector() || !va_range || !cb_range) {
        return;
    }
    if (s_pending_writes.size() >= C_SOURCES_MAX) {
        std::scoped_lock lk{s_journal_mutex};
        ++s_c_journal_dropped;
        return;
    }
    if (source == WriteSource::DrawBinding && s_is_compute) {
        source = WriteSource::DispatchBinding;
    }
    s_pending_writes.push_back({va_range, cb_range, source});
}

void Journal(VAddr va_range, u64 cb_range, FrameCapture::WriteSource source, u64 detail,
             u64 tick) {
    if (!va_range || !cb_range) {
        return;
    }
    const u64 position = s_position.load(std::memory_order_relaxed);
    const auto now = Clock::now();
    std::scoped_lock lk{s_journal_mutex};
    if (s_journal.size() >= C_JOURNAL_MAX) {
        ++s_c_journal_dropped;
        return;
    }
    s_journal.push_back({va_range, cb_range, source, detail, tick, u32(position >> 32),
                         u32(position), now});
}

void TakeDraw(u32 i_pass, u32 i_draw, u64 vs_hash, u64 ps_hash, u32 i_pair_draw,
              const PsSkipDraws::Target& color0, bool is_skipped, u64 tick) {
    const PsSkipDraws::Selector* p_selector = Selector();
    if (!p_selector) {
        return;
    }
    size_t i_journal = 0;
    {
        std::scoped_lock lk{s_journal_mutex};
        i_journal = s_journal.size();
    }
    const auto record_time = Clock::now();
    JournalPending(i_pass, i_draw, tick);
    SetPosition(i_pass, i_pass == NO_PASS ? 0 : i_draw + 1);
    const bool is_selected = [&] {
        if (!s_is_candidate || i_pass == NO_PASS ||
            !PsSkipDraws::IsMatch(*p_selector, vs_hash, ps_hash, color0)) {
            return false;
        }
        const u32 i_match = s_c_matches++;
        return !p_selector->has_nth || i_match == p_selector->i_nth;
    }();
    s_is_candidate = false;
    if (!is_selected) {
        ClearPending();
        return;
    }
    if (s_draws.size() >= C_DRAWS_MAX) {
        ++s_c_draws_over;
        CountNotDumped(std::format("over {} selected draws per frame", C_DRAWS_MAX));
        ClearPending();
        return;
    }
    if (s_c_pending_over) {
        s_not_dumped[std::format("over {} buffers per draw", C_SOURCES_MAX)] += s_c_pending_over;
    }
    for (const auto& source : s_pending_sources) {
        if (!source.skipped.empty()) {
            CountNotDumped(source.skipped);
        }
    }
    s_draws.push_back({.i_pass = i_pass,
                       .i_draw = i_draw,
                       .vs_hash = vs_hash,
                       .ps_hash = ps_hash,
                       .i_pair_draw = i_pair_draw,
                       .color0 = color0,
                       .is_skipped = is_skipped,
                       .tick = tick,
                       .i_journal = i_journal,
                       .time = record_time,
                       .user_data = std::move(s_pending_user_data),
                       .sources = std::move(s_pending_sources)});
    ClearPending();
}

void TakeDispatch(u32 i_pass, u64 tick) {
    if (!Selector()) {
        return;
    }
    JournalPending(i_pass, 0, tick);
    SetPosition(i_pass, 0);
    s_is_candidate = false;
    ClearPending();
}

std::vector<CaptureDraw::GpuRange*> PassRanges(u32 i_pass) {
    std::vector<CaptureDraw::GpuRange*> ranges;
    for (auto& draw : s_draws) {
        if (draw.i_pass != i_pass) {
            continue;
        }
        for (auto& source : draw.sources) {
            if (source.gpu.h_buffer && source.gpu.skipped.empty() && !source.is_copy_requested) {
                source.is_copy_requested = true;
                ranges.push_back(&source.gpu);
            }
        }
    }
    return ranges;
}

void WriteFrame(const std::string& stem, const u8* p_gpu_copies) {
    const PsSkipDraws::Selector* p_selector = Selector();
    if (!p_selector) {
        return;
    }
    std::vector<JournalEntry> journal;
    u64 c_dropped = 0;
    {
        std::scoped_lock lk{s_journal_mutex};
        journal = std::move(s_journal);
        s_journal.clear();
        c_dropped = s_c_journal_dropped;
        s_c_journal_dropped = 0;
    }
    std::string text;
    size_t cb_omitted = 0;
    const auto append = [&](const std::string& part) {
        if (cb_omitted == 0 && text.size() + part.size() <= CB_TEXT_MAX) {
            text += part;
        } else {
            cb_omitted += part.size();
        }
    };
    u32 c_buffers = 0;
    u32 c_gpu_copies = 0;
    for (const auto& draw : s_draws) {
        for (const auto& source : draw.sources) {
            c_buffers += source.skipped.empty();
            if (!source.gpu.h_buffer || !source.skipped.empty()) {
                continue;
            }
            if (source.gpu.off_data != CaptureDraw::NOT_COPIED && p_gpu_copies) {
                ++c_gpu_copies;
            } else {
                CountNotDumped(std::format("gpu copy: {}", source.gpu.skipped.empty()
                                                               ? "not reached"
                                                               : source.gpu.skipped));
            }
        }
    }
    u32 c_not_dumped = 0;
    std::string reasons;
    for (const auto& [reason, count] : s_not_dumped) {
        c_not_dumped += count;
        reasons += std::format("{}{} {}", reasons.empty() ? "" : ", ", count, reason);
    }
    append(std::format(
        "PS_CAPTURE_DUMP_VSCONST {}: {} draws selected ({} matched the pair and target{}), {} "
        "buffers dumped, {} gpu copies, {} not dumped{}{}{}\n",
        p_selector->text, s_draws.size(), s_c_matches,
        s_c_draws_over ? std::format(", {} over the cap", s_c_draws_over) : std::string{},
        c_buffers, c_gpu_copies, c_not_dumped, reasons.empty() ? "" : " (", reasons,
        reasons.empty() ? "" : ")"));
    append(std::format("write journal: {} entries since the capture was armed or the previous "
                       "frame was written{}; GPU-side writes in stream order with their pass and "
                       "draw, CPU writes the GPU side heard of with where it was then\n",
                       journal.size(),
                       c_dropped ? std::format(", {} dropped over {}", c_dropped, C_JOURNAL_MAX)
                                 : std::string{}));
    append("words: a float when it is 0 or between 1e-6 and 1e7 in magnitude, else hexadecimal\n");
    for (const auto& draw : s_draws) {
        const long long us =
            std::chrono::duration_cast<std::chrono::microseconds>(draw.time - s_frame_start)
                .count();
        append(std::format("\ndraw #{} draw {}: vs {:016x} ps {:016x} nth {} target {}{}, tick {}, "
                           "{} us into the frame\n",
                           draw.i_pass, draw.i_draw, draw.vs_hash, draw.ps_hash, draw.i_pair_draw,
                           DescribeTarget(draw.color0),
                           draw.is_skipped ? " skipped by PS_SKIP_DRAWS" : "", draw.tick, us));
        if (draw.user_data.empty()) {
            append("  no vertex or pixel stage user data\n");
        }
        for (const auto& block : draw.user_data) {
            append(DescribeUserData(block));
        }
        for (const auto& source : draw.sources) {
            append(DescribeSource(draw, source, p_gpu_copies, journal));
        }
    }
    if (s_draws.empty()) {
        append(std::format("no draw of this frame selected ({} matched the pair and target)\n",
                           s_c_matches));
    }
    append(Summarize(p_gpu_copies));
    if (cb_omitted) {
        text += std::format("\ntruncated: {} bytes of text over the {} KiB cap not written\n",
                            cb_omitted, CB_TEXT_MAX >> 10);
    }
    const std::string path = stem + "_vsconst.txt";
    FILE* p_file = std::fopen(path.c_str(), "w");
    if (!p_file) {
        std::printf("Frame capture: vs constants: cannot write %s\n", path.c_str());
    } else {
        std::fwrite(text.data(), 1, text.size(), p_file);
        std::fclose(p_file);
        std::printf("Frame capture: vs constants: %zu draws, %u buffers, %u gpu copies, %u not "
                    "dumped%s%s%s, journal %zu writes (%llu dropped), %s%s\n",
                    s_draws.size(), c_buffers, c_gpu_copies, c_not_dumped,
                    reasons.empty() ? "" : " (", reasons.c_str(), reasons.empty() ? "" : ")",
                    journal.size(), static_cast<unsigned long long>(c_dropped), path.c_str(),
                    cb_omitted ? std::format(", truncated ({} bytes over {} KiB)", cb_omitted,
                                             CB_TEXT_MAX >> 10)
                                     .c_str()
                               : "");
    }
    s_draws.clear();
}

} // namespace Vulkan::CaptureConsts
