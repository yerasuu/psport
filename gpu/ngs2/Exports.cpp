// SPDX-License-Identifier: GPL-2.0-or-later
// The Ngs2 functions in the loader's table, by NID. Each is called through Guarded: what the
// sources throw for a case they do not implement or an argument they reject becomes a logged
// error code, as an exception must not unwind through the game's frames.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <unordered_map>

#include "Ngs2Types.hpp"
#include "core/libraries/libs.h"

extern "C" {
int sceNgs2CalcWaveformBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos, uint32_t num_samples, Ngs2WaveformBlock* block);
int sceNgs2GeomApply(const Ngs2GeomListenerWork* listener, const Ngs2GeomSourceParam* source, Ngs2GeomAttribute* out_attrib, uint32_t flags);
int sceNgs2GeomCalcListener(const Ngs2GeomListenerParam* param, Ngs2GeomListenerWork* out_work, uint32_t flags);
int sceNgs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param);
int sceNgs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param);
int sceNgs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params, uint32_t num_params, uint32_t matrix_format, float* out_volume_matrix);
int sceNgs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle, uint32_t num_speakers);
int sceNgs2ParseWaveformData(const void* data, size_t data_size, Ngs2WaveformInfo* info);
int sceNgs2ParseWaveformFile(const char* path, uint32_t offset, Ngs2WaveformInfo* info);
int sceNgs2RackCreate(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option, const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
int sceNgs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option, const Ngs2BufferAllocator* allocator, uintptr_t* handle);
int sceNgs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info);
int sceNgs2RackGetInfo(uintptr_t rack_handle, Ngs2RackInfo* info, size_t info_size);
int sceNgs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id, uintptr_t* handle);
int sceNgs2RackLock(uintptr_t rack_handle);
int sceNgs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option, Ngs2ContextBufferInfo* buffer_info);
int sceNgs2RackUnlock(uintptr_t rack_handle);
int sceNgs2SystemCreate(const Ngs2SystemOption* option, const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
int sceNgs2SystemCreateWithAllocator(const Ngs2SystemOption* option, const Ngs2BufferAllocator* allocator, uintptr_t* handle);
int sceNgs2SystemDestroy(uintptr_t system_handle, Ngs2ContextBufferInfo* buffer_info);
int sceNgs2SystemGetInfo(uintptr_t system_handle, Ngs2SystemInfo* info, size_t info_size);
int sceNgs2SystemGetUserData(uintptr_t system_handle, uintptr_t* user_data);
int sceNgs2SystemLock(uintptr_t system_handle);
int sceNgs2SystemQueryBufferSize(const Ngs2SystemOption* option, Ngs2ContextBufferInfo* buffer_info);
int sceNgs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info);
int sceNgs2SystemResetOption(Ngs2SystemOption* option);
int sceNgs2SystemSetGrainSamples(uintptr_t system_handle, uint32_t num_samples);
int sceNgs2SystemSetSampleRate(uintptr_t system_handle, uint32_t sample_rate);
int sceNgs2SystemSetUserData(uintptr_t system_handle, uintptr_t user_data);
int sceNgs2SystemUnlock(uintptr_t system_handle);
int sceNgs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list);
int sceNgs2VoiceGetPortInfo(uintptr_t voice_handle, uint32_t port, Ngs2VoicePortInfo* info, size_t info_size);
int sceNgs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state, size_t state_size);
int sceNgs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags);
int sceNgs2VoiceQueryInfo(uintptr_t voice_handle, uint32_t info_id, void* info, size_t info_size);
int sceNgs2VoiceRunCommands(uintptr_t voice_handle, const Ngs2VoiceCommand* commands, size_t num_commands);
}

namespace Libraries::Ngs2 {
namespace {
// Within the library's 0x804Axxxx range so the title sees a failure; which code the console
// returns in each of these cases is not known.
constexpr int GuardedFailure = static_cast<int>(0x804A0001);

// BB_NGS2_TRACE=1: the first calls of each function with their result, and the level of what
// each traced render wrote, to follow a title through the library when it stays silent.
constexpr unsigned TracedCalls = 40;

bool Tracing() {
    static const bool tracing = [] {
        const char* env = std::getenv("BB_NGS2_TRACE");
        return env && env[0] == '1';
    }();
    return tracing;
}

std::unordered_map<std::uintptr_t, const char*>& Names() {
    static std::unordered_map<std::uintptr_t, const char*> names;
    return names;
}

template <typename... Args>
void Traced(const char* name, int result, Args...) {
    std::printf("Ngs2 trace: %s -> %#x\n", name, static_cast<unsigned>(result));
}

void Traced(const char* name, int result, uintptr_t, const Ngs2RenderBufferInfo* buffers, uint32_t count) {
    float peak = 0.0f;
    for (uint32_t b = 0; buffers && b < count; ++b) {
        const auto& info = buffers[b];
        if (info.buffer == nullptr || info.waveform_type != SCE_NGS2_WAVEFORM_TYPE_PCM_F32L) continue;
        const auto* samples = static_cast<const float*>(info.buffer);
        for (std::size_t i = 0; i < info.buffer_size / sizeof(float); ++i) peak = std::max(peak, std::fabs(samples[i]));
    }
    std::printf("Ngs2 trace: %s -> %#x, %u buffers, peak %g\n", name, static_cast<unsigned>(result), count, peak);
}

template <auto F>
struct Guarded;
template <typename... Args, int (*F)(Args...)>
struct Guarded<F> {
    static int Call(Args... args) {
        int result;
        try {
            result = F(args...);
        } catch (const std::exception& error) {
            std::printf("Ngs2: %s\n", error.what());
            result = GuardedFailure;
        }
        static std::atomic<unsigned> calls{0};
        if (Tracing() && calls.fetch_add(1, std::memory_order_relaxed) < TracedCalls) {
            Traced(Names()[reinterpret_cast<std::uintptr_t>(F)], result, args...);
        }
        return result;
    }
};
} // namespace

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2CalcWaveformBlock)] = "sceNgs2CalcWaveformBlock";
    LIB_FUNCTION("3pCNbVM11UA", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2CalcWaveformBlock>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2GeomApply)] = "sceNgs2GeomApply";
    LIB_FUNCTION("eF8yRCC6W64", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2GeomApply>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2GeomCalcListener)] = "sceNgs2GeomCalcListener";
    LIB_FUNCTION("1WsleK-MTkE", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2GeomCalcListener>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2GeomResetListenerParam)] = "sceNgs2GeomResetListenerParam";
    LIB_FUNCTION("7Lcfo8SmpsU", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2GeomResetListenerParam>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2GeomResetSourceParam)] = "sceNgs2GeomResetSourceParam";
    LIB_FUNCTION("0lbbayqDNoE", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2GeomResetSourceParam>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2PanGetVolumeMatrix)] = "sceNgs2PanGetVolumeMatrix";
    LIB_FUNCTION("gbMKV+8Enuo", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2PanGetVolumeMatrix>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2PanInit)] = "sceNgs2PanInit";
    LIB_FUNCTION("xa8oL9dmXkM", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2PanInit>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2ParseWaveformData)] = "sceNgs2ParseWaveformData";
    LIB_FUNCTION("hyVLT2VlOYk", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2ParseWaveformData>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2ParseWaveformFile)] = "sceNgs2ParseWaveformFile";
    LIB_FUNCTION("iprCTXPVWMI", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2ParseWaveformFile>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2RackCreate)] = "sceNgs2RackCreate";
    LIB_FUNCTION("cLV4aiT9JpA", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackCreate>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2RackCreateWithAllocator)] = "sceNgs2RackCreateWithAllocator";
    LIB_FUNCTION("U546k6orxQo", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackCreateWithAllocator>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2RackDestroy)] = "sceNgs2RackDestroy";
    LIB_FUNCTION("lCqD7oycmIM", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackDestroy>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2RackGetInfo)] = "sceNgs2RackGetInfo";
    LIB_FUNCTION("M4LYATRhRUE", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackGetInfo>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2RackGetVoiceHandle)] = "sceNgs2RackGetVoiceHandle";
    LIB_FUNCTION("MwmHz8pAdAo", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackGetVoiceHandle>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2RackLock)] = "sceNgs2RackLock";
    LIB_FUNCTION("MzTa7VLjogY", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackLock>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2RackQueryBufferSize)] = "sceNgs2RackQueryBufferSize";
    LIB_FUNCTION("0eFLVCfWVds", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackQueryBufferSize>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2RackUnlock)] = "sceNgs2RackUnlock";
    LIB_FUNCTION("++YZ7P9e87U", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackUnlock>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemCreate)] = "sceNgs2SystemCreate";
    LIB_FUNCTION("koBbCMvOKWw", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemCreate>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemCreateWithAllocator)] = "sceNgs2SystemCreateWithAllocator";
    LIB_FUNCTION("mPYgU4oYpuY", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemCreateWithAllocator>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemDestroy)] = "sceNgs2SystemDestroy";
    LIB_FUNCTION("u-WrYDaJA3k", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemDestroy>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemGetInfo)] = "sceNgs2SystemGetInfo";
    LIB_FUNCTION("vU7TQ62pItw", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemGetInfo>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemGetUserData)] = "sceNgs2SystemGetUserData";
    LIB_FUNCTION("4lFaRxd-aLs", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemGetUserData>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemLock)] = "sceNgs2SystemLock";
    LIB_FUNCTION("gThZqM5PYlQ", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemLock>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemQueryBufferSize)] = "sceNgs2SystemQueryBufferSize";
    LIB_FUNCTION("pgFAiLR5qT4", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemQueryBufferSize>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemRender)] = "sceNgs2SystemRender";
    LIB_FUNCTION("i0VnXM-C9fc", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemRender>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemResetOption)] = "sceNgs2SystemResetOption";
    LIB_FUNCTION("AQkj7C0f3PY", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemResetOption>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemSetGrainSamples)] = "sceNgs2SystemSetGrainSamples";
    LIB_FUNCTION("l4Q2dWEH6UM", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemSetGrainSamples>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemSetSampleRate)] = "sceNgs2SystemSetSampleRate";
    LIB_FUNCTION("-tbc2SxQD60", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemSetSampleRate>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemSetUserData)] = "sceNgs2SystemSetUserData";
    LIB_FUNCTION("GZB2v0XnG0k", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemSetUserData>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2SystemUnlock)] = "sceNgs2SystemUnlock";
    LIB_FUNCTION("JXRC5n0RQls", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemUnlock>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2VoiceControl)] = "sceNgs2VoiceControl";
    LIB_FUNCTION("uu94irFOGpA", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceControl>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2VoiceGetPortInfo)] = "sceNgs2VoiceGetPortInfo";
    LIB_FUNCTION("WCayTgob7-o", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceGetPortInfo>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2VoiceGetState)] = "sceNgs2VoiceGetState";
    LIB_FUNCTION("-TOuuAQ-buE", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceGetState>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2VoiceGetStateFlags)] = "sceNgs2VoiceGetStateFlags";
    LIB_FUNCTION("rEh728kXk3w", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceGetStateFlags>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2VoiceQueryInfo)] = "sceNgs2VoiceQueryInfo";
    LIB_FUNCTION("9eic4AmjGVI", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceQueryInfo>::Call);
    Names()[reinterpret_cast<std::uintptr_t>(&sceNgs2VoiceRunCommands)] = "sceNgs2VoiceRunCommands";
    LIB_FUNCTION("AbYvTOZ8Pts", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceRunCommands>::Call);
}
} // namespace Libraries::Ngs2
