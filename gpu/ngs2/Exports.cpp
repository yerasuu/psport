// SPDX-License-Identifier: GPL-2.0-or-later
// The Ngs2 functions in the loader's table, by NID. Each is called through Guarded: what the
// sources throw for a case they do not implement or an argument they reject becomes a logged
// error code, as an exception must not unwind through the game's frames.
#include <cstdint>
#include <cstdio>
#include <exception>

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

template <auto F>
struct Guarded;
template <typename... Args, int (*F)(Args...)>
struct Guarded<F> {
    static int Call(Args... args) {
        try {
            return F(args...);
        } catch (const std::exception& error) {
            std::printf("Ngs2: %s\n", error.what());
            return GuardedFailure;
        }
    }
};
} // namespace

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("3pCNbVM11UA", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2CalcWaveformBlock>::Call);
    LIB_FUNCTION("eF8yRCC6W64", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2GeomApply>::Call);
    LIB_FUNCTION("1WsleK-MTkE", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2GeomCalcListener>::Call);
    LIB_FUNCTION("7Lcfo8SmpsU", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2GeomResetListenerParam>::Call);
    LIB_FUNCTION("0lbbayqDNoE", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2GeomResetSourceParam>::Call);
    LIB_FUNCTION("gbMKV+8Enuo", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2PanGetVolumeMatrix>::Call);
    LIB_FUNCTION("xa8oL9dmXkM", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2PanInit>::Call);
    LIB_FUNCTION("hyVLT2VlOYk", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2ParseWaveformData>::Call);
    LIB_FUNCTION("iprCTXPVWMI", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2ParseWaveformFile>::Call);
    LIB_FUNCTION("cLV4aiT9JpA", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackCreate>::Call);
    LIB_FUNCTION("U546k6orxQo", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackCreateWithAllocator>::Call);
    LIB_FUNCTION("lCqD7oycmIM", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackDestroy>::Call);
    LIB_FUNCTION("M4LYATRhRUE", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackGetInfo>::Call);
    LIB_FUNCTION("MwmHz8pAdAo", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackGetVoiceHandle>::Call);
    LIB_FUNCTION("MzTa7VLjogY", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackLock>::Call);
    LIB_FUNCTION("0eFLVCfWVds", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackQueryBufferSize>::Call);
    LIB_FUNCTION("++YZ7P9e87U", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2RackUnlock>::Call);
    LIB_FUNCTION("koBbCMvOKWw", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemCreate>::Call);
    LIB_FUNCTION("mPYgU4oYpuY", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemCreateWithAllocator>::Call);
    LIB_FUNCTION("u-WrYDaJA3k", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemDestroy>::Call);
    LIB_FUNCTION("vU7TQ62pItw", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemGetInfo>::Call);
    LIB_FUNCTION("4lFaRxd-aLs", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemGetUserData>::Call);
    LIB_FUNCTION("gThZqM5PYlQ", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemLock>::Call);
    LIB_FUNCTION("pgFAiLR5qT4", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemQueryBufferSize>::Call);
    LIB_FUNCTION("i0VnXM-C9fc", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemRender>::Call);
    LIB_FUNCTION("AQkj7C0f3PY", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemResetOption>::Call);
    LIB_FUNCTION("l4Q2dWEH6UM", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemSetGrainSamples>::Call);
    LIB_FUNCTION("-tbc2SxQD60", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemSetSampleRate>::Call);
    LIB_FUNCTION("GZB2v0XnG0k", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemSetUserData>::Call);
    LIB_FUNCTION("JXRC5n0RQls", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2SystemUnlock>::Call);
    LIB_FUNCTION("uu94irFOGpA", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceControl>::Call);
    LIB_FUNCTION("WCayTgob7-o", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceGetPortInfo>::Call);
    LIB_FUNCTION("-TOuuAQ-buE", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceGetState>::Call);
    LIB_FUNCTION("rEh728kXk3w", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceGetStateFlags>::Call);
    LIB_FUNCTION("9eic4AmjGVI", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceQueryInfo>::Call);
    LIB_FUNCTION("AbYvTOZ8Pts", "libSceNgs2", 1, "libSceNgs2", Guarded<sceNgs2VoiceRunCommands>::Call);
}
} // namespace Libraries::Ngs2
