// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <mutex>

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image_info.h"

namespace Vulkan {

namespace {

struct Target {
    VAddr va_target{};
    vk::Format format{};
    u32 width{}, height{};
    bool operator==(const Target&) const = default;
};

enum DepthUseBits : u32 { DepthTest = 1, DepthWrite = 2, StencilTest = 4 };

struct Entry {
    bool is_compute{};
    bool is_copy{};
    std::vector<Target> colors;
    Target depth;
    u32 depth_use{}; ///< DepthUseBits over the pass's draws; for a copy, depth and stencil moved
    Target copy_dst;
    u32 draws{};
    u64 indices{};
    std::vector<u64> shaders; ///< vs/ps pairs (graphics) or cs (compute), first few
    std::vector<std::pair<Target, bool>> sampled; ///< textures (false) and storage images (true)
    std::string note;
    std::vector<std::string> buffers; ///< dumped constants, per draw
};

// GPU thread only.
std::vector<Entry> s_entries;
std::vector<std::pair<Target, bool>> s_pending_sampled;
std::vector<std::string> s_pending_buffers;

void AddBuffers(Entry& entry) {
    // Constants of full-screen passes (post-processing: camera matrices) and of the first draw
    // of each geometry pass.
    if (entry.draws <= 4) {
        for (auto& b : s_pending_buffers) {
            entry.buffers.push_back(std::move(b));
        }
    }
    s_pending_buffers.clear();
}
bool s_is_pass_open = false;
std::mutex s_display_mutex;
std::vector<VAddr> s_display_buffers;
// psport: read on every draw; the game registers a handful of display buffers once.
std::array<std::atomic<VAddr>, 16> s_display_list{};
std::atomic<u32> s_c_displays{0};

Target ToTarget(const VideoCore::ImageInfo& info) {
    return {info.guest_address, info.pixel_format, info.size.width, info.size.height};
}

std::string Describe(const Target& t) {
    if (!t.va_target) {
        return "-";
    }
    return std::format("{:#x} {} {}x{}", t.va_target, vk::to_string(t.format), t.width, t.height);
}

void AddSampled(Entry& entry) {
    for (const auto& s : s_pending_sampled) {
        if (entry.sampled.size() < 32 &&
            std::ranges::find(entry.sampled, s) == entry.sampled.end()) {
            entry.sampled.push_back(s);
        }
    }
    s_pending_sampled.clear();
}

void AddShader(Entry& entry, u64 hash) {
    if (entry.shaders.size() < 8 && std::ranges::find(entry.shaders, hash) == entry.shaders.end()) {
        entry.shaders.push_back(hash);
    }
}

void Write(VAddr presented) {
    const char* sz_dir = std::getenv("PS_CAPTURE_DIR");
    const std::string path =
        std::format("{}/frame_{}.txt", sz_dir ? sz_dir : ".", static_cast<long long>(std::time(nullptr)));
    FILE* p_file = std::fopen(path.c_str(), "w");
    if (!p_file) {
        std::printf("Frame capture: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(p_file, "presented buffer %#llx\n", static_cast<unsigned long long>(presented));
    u32 i_file = 0;
    for (const auto& e : s_entries) {
        if (e.is_copy) {
            std::fprintf(p_file, "\n#%u DEPTH COPY%s%s\n  from %s\n  to   %s\n", i_file++,
                         (e.depth_use & DepthTest) ? " depth" : "",
                         (e.depth_use & StencilTest) ? " stencil" : "", Describe(e.depth).c_str(),
                         Describe(e.copy_dst).c_str());
            continue;
        }
        if (e.is_compute) {
            std::fprintf(p_file, "\n#%u COMPUTE dispatches %u, cs", i_file++, e.draws);
        } else {
            std::fprintf(p_file, "\n#%u PASS draws %u (indices %llu), vs/ps", i_file++, e.draws,
                         static_cast<unsigned long long>(e.indices));
        }
        for (const u64 h : e.shaders) {
            std::fprintf(p_file, " %016llx", static_cast<unsigned long long>(h));
        }
        std::fprintf(p_file, "\n");
        for (size_t i = 0; i < e.colors.size(); ++i) {
            std::fprintf(p_file, "  color%zu %s\n", i, Describe(e.colors[i]).c_str());
        }
        if (!e.is_compute) {
            std::fprintf(p_file, "  depth  %s%s%s%s\n", Describe(e.depth).c_str(),
                         (e.depth_use & DepthTest) ? " test" : "",
                         (e.depth_use & DepthWrite) ? " write" : "",
                         (e.depth_use & StencilTest) ? " stencil" : "");
        }
        for (const auto& [t, storage] : e.sampled) {
            std::fprintf(p_file, "  %s %s\n", storage ? "writes " : "samples", Describe(t).c_str());
        }
        if (!e.note.empty()) {
            std::fprintf(p_file, "  note: %s\n", e.note.c_str());
        }
        for (const auto& b : e.buffers) {
            std::fprintf(p_file, "%s\n", b.c_str());
        }
    }
    std::fclose(p_file);
    std::printf("Frame capture: %zu passes written to %s\n", s_entries.size(), path.c_str());
}

} // namespace

void FrameCapture::OnFlip(VAddr va_presented) {
    m_last_presented.store(va_presented, std::memory_order_relaxed);
    m_c_flips.fetch_add(1, std::memory_order_release);
    static const char* s_sz_trigger = std::getenv("PS_CAPTURE_TRIGGER");
    if (s_sz_trigger && m_state.load(std::memory_order_relaxed) == Idle &&
        std::filesystem::exists(s_sz_trigger)) {
        std::error_code ec;
        std::filesystem::remove(s_sz_trigger, ec);
        m_state.store(Armed, std::memory_order_release);
        std::printf("Frame capture: armed\n");
    }
}

void FrameCapture::Poll() {}

bool FrameCapture::IsDisplayBuffer(VAddr va_buffer) {
    const u32 c_displays = s_c_displays.load(std::memory_order_acquire);
    for (u32 i = 0; i < c_displays; ++i) {
        if (s_display_list[i].load(std::memory_order_relaxed) == va_buffer) {
            return true;
        }
    }
    if (c_displays < s_display_list.size()) {
        return false;
    }
    std::scoped_lock lk{s_display_mutex};
    return std::ranges::find(s_display_buffers, va_buffer) != s_display_buffers.end();
}

void FrameCapture::AddDisplayBuffer(VAddr va_buffer) {
    std::scoped_lock lk{s_display_mutex};
    if (std::ranges::find(s_display_buffers, va_buffer) == s_display_buffers.end()) {
        s_display_buffers.push_back(va_buffer);
        const u32 c_displays = s_c_displays.load(std::memory_order_relaxed);
        if (c_displays < s_display_list.size()) {
            s_display_list[c_displays].store(va_buffer, std::memory_order_relaxed);
            s_c_displays.store(c_displays + 1, std::memory_order_release);
        }
    }
}

void FrameCapture::BeginPass(const VideoCore::ImageInfo* const* pp_colors, u32 c_colors,
                             const VideoCore::ImageInfo* p_depth) {
    std::vector<Target> targets;
    bool is_display = false;
    {
        std::scoped_lock lk{s_display_mutex};
        for (u32 i = 0; i < c_colors; ++i) {
            targets.push_back(pp_colors[i] ? ToTarget(*pp_colors[i]) : Target{});
            is_display |= pp_colors[i] && std::ranges::find(s_display_buffers, pp_colors[i]->guest_address) !=
                                        s_display_buffers.end();
        }
    }
    // Frame boundaries in the command stream: the pass that writes a display buffer.
    const bool is_new_pass = !(s_is_pass_open && !s_entries.empty() && !s_entries.back().is_compute &&
                            s_entries.back().colors == targets);
    if (is_display && is_new_pass) {
        if (m_state.load(std::memory_order_relaxed) == Recording) {
            Write(m_last_presented.load(std::memory_order_relaxed));
            s_entries.clear();
            s_pending_sampled.clear();
            s_pending_buffers.clear();
            m_state.store(Idle, std::memory_order_release);
            return;
        }
        s_entries.clear();
        s_pending_sampled.clear();
        s_pending_buffers.clear();
        s_is_pass_open = false;
        m_state.store(Recording, std::memory_order_release);
    }
    if (m_state.load(std::memory_order_relaxed) != Recording) {
        s_pending_sampled.clear();
        s_pending_buffers.clear();
        return;
    }
    const Target d = p_depth ? ToTarget(*p_depth) : Target{};
    if (s_is_pass_open && !s_entries.empty() && !s_entries.back().is_compute &&
        s_entries.back().colors == targets && s_entries.back().depth == d) {
        return;
    }
    s_entries.push_back({.is_compute = false, .colors = std::move(targets), .depth = d});
    s_is_pass_open = true;
}

void FrameCapture::Draw(u64 vs_hash, u64 ps_hash, u32 c_indices, u32 c_instances) {
    if (m_state.load(std::memory_order_relaxed) != Recording || s_entries.empty() ||
        s_entries.back().is_compute || s_entries.back().is_copy) {
        return;
    }
    auto& e = s_entries.back();
    ++e.draws;
    e.indices += u64(c_indices) * std::max(c_instances, 1u);
    AddShader(e, vs_hash);
    AddShader(e, ps_hash);
    AddSampled(e);
    AddBuffers(e);
}

void FrameCapture::DepthUse(bool is_test, bool is_write, bool is_stencil) {
    if (m_state.load(std::memory_order_relaxed) != Recording || s_entries.empty() ||
        s_entries.back().is_compute || s_entries.back().is_copy) {
        return;
    }
    s_entries.back().depth_use |=
        (is_test ? DepthTest : 0u) | (is_write ? DepthWrite : 0u) | (is_stencil ? StencilTest : 0u);
}

void FrameCapture::DepthCopy(const VideoCore::ImageInfo& src, const VideoCore::ImageInfo& dst,
                             bool is_depth, bool is_stencil) {
    if (m_state.load(std::memory_order_relaxed) != Recording) {
        return;
    }
    s_entries.push_back({.is_copy = true,
                         .depth = ToTarget(src),
                         .depth_use = (is_depth ? DepthTest : 0u) | (is_stencil ? StencilTest : 0u),
                         .copy_dst = ToTarget(dst)});
    s_is_pass_open = false;
}

void FrameCapture::Dispatch(u64 cs_hash, u32 x, u32 y, u32 z) {
    if (m_state.load(std::memory_order_relaxed) != Recording) {
        s_pending_sampled.clear();
        s_pending_buffers.clear();
        return;
    }
    if (!s_entries.empty() && s_entries.back().is_compute && !s_entries.back().shaders.empty() &&
        s_entries.back().shaders[0] == cs_hash) {
        ++s_entries.back().draws;
    } else {
        s_entries.push_back({.is_compute = true, .draws = 1, .shaders = {cs_hash}});
        s_entries.back().note = std::format("groups {}x{}x{}", x, y, z);
    }
    AddSampled(s_entries.back());
    AddBuffers(s_entries.back());
    s_is_pass_open = false;
}

void FrameCapture::Buffer(u64 stage_hash, u32 slot, VAddr va_buffer, const void* p_data, u64 cb_data) {
    const u64 cb_dumped = std::min<u64>(cb_data, 1024) & ~u64(3);
    std::string out = std::format("  buffer stage {:016x} slot {} at {:#x} size {}:", stage_hash,
                                  slot, va_buffer, cb_data);
    const auto* p_words = static_cast<const float*>(p_data);
    for (u64 i = 0; i < cb_dumped / 4; ++i) {
        if (i % 8 == 0) {
            out += std::format("\n    [{:3}]", i);
        }
        out += std::format(" {:12.6g}", p_words[i]);
    }
    s_pending_buffers.push_back(std::move(out));
}

void FrameCapture::Sampled(const VideoCore::ImageInfo& info, bool is_storage) {
    if (s_pending_sampled.size() < 64) {
        s_pending_sampled.emplace_back(ToTarget(info), is_storage);
    }
}

void FrameCapture::Note(const char* sz_text) {
    if (!s_entries.empty()) {
        s_entries.back().note += sz_text;
    }
}

} // namespace Vulkan
