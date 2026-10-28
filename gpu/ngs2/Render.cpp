// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2SystemRender (spec 01 §5, 04 §1). Phase F2 validates the buffers as the firmware does and
// writes what it writes when no voice reaches an output, silence; the flush, the racks' processing
// and the output conversion come in F3, and until then a render with a voice in use is counted.
#include <chrono>
#include <cstring>

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

u32 BytesPerSample(u32 waveform_type) {
    switch (waveform_type) {
    case 0x12: // PCM I16 little-endian
    case 0x13: // PCM I16 big-endian
        return 2;
    case 0x18: // PCM F32 little-endian
    case 0x19: // PCM F32 big-endian
        return 4;
    default:
        return 0;
    }
}

bool IsRenderChannelCount(u32 c_channels) {
    return c_channels == 1 || c_channels == 2 || c_channels == 6 || c_channels == 8;
}

s32 ValidateBuffers(OrbisNgs2Handle h_system, const OrbisNgs2RenderBufferInfo* p_buffers, u32 c_buffers,
                    u32 num_grain_samples) {
    for (u32 i_buffer = 0; i_buffer < c_buffers; ++i_buffer) {
        const OrbisNgs2RenderBufferInfo& buffer = p_buffers[i_buffer];
        if (!IsRenderChannelCount(buffer.numChannels)) {
            return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS, "Invalid render channels ({})",
                        buffer.numChannels);
        }
        const u32 cb_sample = BytesPerSample(buffer.waveformType);
        if (cb_sample == 0) {
            return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_TYPE, "Invalid render waveform type ({:#x})",
                        buffer.waveformType);
        }
        if (!buffer.buffer) {
            return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS, "Invalid render buffer address ({})",
                        buffer.buffer);
        }
        const std::size_t cb_grain = std::size_t(num_grain_samples) * buffer.numChannels * cb_sample;
        if (buffer.bufferSize < cb_grain) {
            return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE, "Invalid render buffer size ({}!={}[byte])",
                        buffer.bufferSize, cb_grain);
        }
    }
    return ORBIS_OK;
}

bool HasVoiceInUse(const System& system) {
    for (const Rack* p_first : system.p_first_rack) {
        for (const Rack* p_rack = p_first; p_rack; p_rack = p_rack->p_next) {
            for (u32 i_voice = 0; i_voice < p_rack->limits.max_voices; ++i_voice) {
                if (p_rack->p_voices[i_voice].control.load(std::memory_order_relaxed) & VOICE_FLAG_IN_USE) {
                    return true;
                }
            }
        }
    }
    return false;
}

s64 NowMicroseconds() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

s32 PS4_SYSV_ABI sceNgs2SystemRender(OrbisNgs2Handle h_system, const OrbisNgs2RenderBufferInfo* p_buffers,
                                     u32 c_buffers) {
    HandleRef ref{h_system, HANDLE_SYSTEM};
    if (!ref) {
        return InvalidHandleError(h_system, HANDLE_SYSTEM);
    }
    if (!p_buffers || c_buffers < 1 || c_buffers > kMaxRenderBuffers) {
        return Fail(h_system, ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS, "Invalid render buffer info ({},{})",
                    static_cast<const void*>(p_buffers), c_buffers);
    }
    auto* p_system = ref.Owner<System>();
    std::scoped_lock lock{p_system->process_lock};
    p_system->tid_render = std::this_thread::get_id();
    p_system->state_flags.fetch_or(SYSTEM_STATE_RENDERING, std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    const u32 num_grain_samples = p_system->num_grain_samples;
    const u32 sample_rate_hz = p_system->sample_rate_hz;
    const s32 result = ValidateBuffers(h_system, p_buffers, c_buffers, num_grain_samples);
    for (u32 i_buffer = 0; i_buffer < c_buffers; ++i_buffer) {
        const OrbisNgs2RenderBufferInfo& buffer = p_buffers[i_buffer];
        if (!buffer.buffer) {
            continue;
        }
        // A failed render clears every buffer as the game declared it; a good one clears the grain
        // each output holds when no voice wrote to it.
        const std::size_t cb_grain =
            std::size_t(num_grain_samples) * buffer.numChannels * BytesPerSample(buffer.waveformType);
        std::memset(buffer.buffer, 0, result < 0 ? buffer.bufferSize : cb_grain);
    }
    if (result >= 0) {
        if (HasVoiceInUse(*p_system)) {
            static std::uint64_t s_c_uses;
            NoteUnimplemented(&s_c_uses, "sceNgs2SystemRender: voice processing (renders silence until F3)");
        }
        const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        p_system->last_render_ratio = float(elapsed_s * sample_rate_hz / num_grain_samples);
        p_system->last_render_tick_us = NowMicroseconds();
        p_system->c_renders.fetch_add(1, std::memory_order_relaxed);
    }
    p_system->state_flags.fetch_and(~u32(SYSTEM_STATE_RENDERING), std::memory_order_relaxed);
    p_system->tid_render = std::thread::id{};
    return result;
}

} // namespace Libraries::Ngs2
