// SPDX-License-Identifier: GPL-2.0-or-later
// sceNgs2SystemRender (spec 01 §5, 04 §1): checks the game's buffers, flushes what VoiceControl left
// pending unless the game holds SystemLock or RackLock, processes the sampler voices in start order
// and the submixer and mastering voices in graph order, clears the buffers no voice wrote and swaps
// the big-endian ones, then retires finished voices and calls the voice callbacks.
#include <bit>
#include <chrono>
#include <cstring>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

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

// Denormals would stall the decaying filters and envelopes; the firmware flushes them to zero for
// the render (spec 04 §1.4).
class FlushDenormals {
public:
#if defined(__SSE__)
    FlushDenormals() : m_csr{_mm_getcsr()} {
        _mm_setcsr(m_csr | 0x8000);
    }
    ~FlushDenormals() {
        _mm_setcsr(m_csr);
    }

private:
    unsigned m_csr;
#endif
};

template <class T>
void SwapSamples(void* p_buffer, std::size_t c_samples) {
    auto* p_samples = static_cast<T*>(p_buffer);
    for (std::size_t i = 0; i < c_samples; ++i) {
        p_samples[i] = std::byteswap(p_samples[i]);
    }
}

void RenderGrain(RenderContext& context) {
    System* p_system = context.p_system;
    const bool can_flush = p_system->flush_lock.TryLock();
    for (Rack* p_first : p_system->p_first_rack) {
        for (Rack* p_rack = p_first; p_rack; p_rack = p_rack->p_next) {
            FlushRack(p_system, p_rack, can_flush);
        }
    }
    if (can_flush) {
        if (p_system->is_topology_dirty) {
            OrderGraph(p_system);
        }
        p_system->flush_lock.Unlock();
    }
    {
        FlushDenormals flush_denormals;
        for (Rack* p_rack = p_system->p_first_rack[RACK_CLASS_SOURCE]; p_rack; p_rack = p_rack->p_next) {
            for (Voice* p_voice = p_rack->p_first_active; p_voice; p_voice = p_voice->p_next_active) {
                ProcessVoice(context, p_voice);
            }
        }
        for (Voice* p_voice = p_system->p_first_ordered; p_voice; p_voice = p_voice->p_next_ordered) {
            if (p_voice->is_active) {
                ProcessVoice(context, p_voice);
            }
        }
    }
    for (u32 i_buffer = 0; i_buffer < context.c_buffers; ++i_buffer) {
        const OrbisNgs2RenderBufferInfo& buffer = context.p_buffers[i_buffer];
        const std::size_t c_samples = std::size_t(context.c_grain_samples) * buffer.numChannels;
        if (!(context.written_mask & (1u << i_buffer))) {
            std::memset(buffer.buffer, 0, c_samples * BytesPerSample(buffer.waveformType));
        } else if (buffer.waveformType == 0x13) {
            SwapSamples<u16>(buffer.buffer, c_samples);
        } else if (buffer.waveformType == 0x19) {
            SwapSamples<u32>(buffer.buffer, c_samples);
        }
    }
    for (Rack* p_first : p_system->p_first_rack) {
        for (Rack* p_rack = p_first; p_rack; p_rack = p_rack->p_next) {
            PostRack(p_rack);
        }
    }
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
    if (result < 0) {
        // A failed render clears every buffer as the game declared it.
        for (u32 i_buffer = 0; i_buffer < c_buffers; ++i_buffer) {
            if (p_buffers[i_buffer].buffer) {
                std::memset(p_buffers[i_buffer].buffer, 0, p_buffers[i_buffer].bufferSize);
            }
        }
    } else {
        RenderContext context{p_system, p_buffers, c_buffers, 0, num_grain_samples, sample_rate_hz,
                              static_cast<float*>(p_system->p_grain_work)};
        RenderGrain(context);
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
