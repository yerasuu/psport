// SPDX-License-Identifier: GPL-2.0-or-later
// libSceNgs2's export table for the native Ngs2 (kb #625). The system, rack, voice
// and report API (phase F2, kb #1094), the render (F3, kb #1095), the waveform parsing (F4, kb #1096)
// and pan, geom and FFT (F7, kb #1099) are implemented; the three PS5 exports return
// ORBIS_NGS2_ERROR_FAIL, the firmware's internal failure, log their first call and are counted in the
// call report. Every other export runs inside an ExportCall, which logs and counts its failures, and
// PS_NGS2_TRACE=1 traces its first calls.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_error.h"

#include "Internal.h"
#include "Spatial.h"

namespace Libraries::Ngs2 {
namespace {

struct Export {
    const char* sz_nid;
    const char* sz_name;
};

// Three PS5 exports AnyPS5's retired port registered (decision #1206), kept so that a game importing
// them still binds and its calls are counted; firmware 11.00 has none of them.
constexpr std::array UNIMPLEMENTED_EXPORTS{
    Export{"AQkj7C0f3PY", "sceNgs2SystemResetOption"},
    Export{"9eic4AmjGVI", "sceNgs2VoiceQueryInfo"},
    Export{"AbYvTOZ8Pts", "sceNgs2VoiceRunCommands"},
};

template <std::size_t I>
s32 PS4_SYSV_ABI FailUnimplemented() {
    static std::uint64_t s_c_calls;
    if (__atomic_fetch_add(&s_c_calls, 1, __ATOMIC_RELAXED) == 0) {
        Common::Log::Stubbed(UNIMPLEMENTED_EXPORTS[I].sz_name, &s_c_calls);
        LOG_ERROR(Lib_Ngs2, "{} is not implemented by the native Ngs2 yet; returns {:#x}",
                  UNIMPLEMENTED_EXPORTS[I].sz_name, static_cast<u32>(ORBIS_NGS2_ERROR_FAIL));
    }
    return ORBIS_NGS2_ERROR_FAIL;
}

template <std::size_t... I>
void RegisterExports(Core::Loader::SymbolsResolver* sym, std::index_sequence<I...>) {
    (LIB_FUNCTION(UNIMPLEMENTED_EXPORTS[I].sz_nid, "libSceNgs2", 1, "libSceNgs2", &FailUnimplemented<I>), ...);
}

// PS_NGS2_TRACE=1: each export's first calls with their arguments and result, to find which call of a
// title's audio set-up fails or answers differently; the cap keeps the render and voice control, called
// every grain, from flooding the log.
constexpr u32 kTracedCallsPerExport = 64;

bool IsTracing() {
    static const bool s_is_tracing = [] {
        const char* sz_value = std::getenv("PS_NGS2_TRACE");
        return sz_value && sz_value[0] == '1';
    }();
    return s_is_tracing;
}

// The structures that size and shape a system or rack are shown whole, since a title's set-up is
// mostly told by them; outputs only once the call succeeded and wrote them.
template <class T>
void DescribeArgument(fmt::memory_buffer& text, T value, bool has_succeeded) {
    const auto out = std::back_inserter(text);
    if constexpr (std::is_pointer_v<T>) {
        using Pointee = std::remove_cv_t<std::remove_pointer_t<T>>;
        fmt::format_to(out, "{:#x}", reinterpret_cast<uintptr_t>(value));
        if (!value) {
            return;
        }
        if constexpr (std::is_same_v<Pointee, OrbisNgs2ContextBufferInfo>) {
            if (std::is_const_v<std::remove_pointer_t<T>> || has_succeeded) {
                fmt::format_to(out, " {{buffer {:#x}, size {:#x}}}", reinterpret_cast<uintptr_t>(value->hostBuffer),
                               value->hostBufferSize);
            }
        } else if constexpr (std::is_same_v<Pointee, OrbisNgs2SystemOption>) {
            fmt::format_to(out, " {{size {:#x}", value->size);
            if (value->size >= sizeof(OrbisNgs2SystemOption)) {
                fmt::format_to(out, ", flags {:#x}, max grain {}, grain {}, rate {}", value->flags,
                               value->maxGrainSamples, value->numGrainSamples, value->sampleRate);
            }
            fmt::format_to(out, "}}");
        } else if constexpr (std::is_same_v<Pointee, OrbisNgs2RackOption>) {
            fmt::format_to(out, " {{size {:#x}", value->size);
            if (value->size >= sizeof(OrbisNgs2RackOption)) {
                fmt::format_to(out, ", flags {:#x}, max grain {}, voices {}, delay blocks {}, matrices {}, ports {}",
                               value->flags, value->maxGrainSamples, value->maxVoices, value->maxInputDelayBlocks,
                               value->maxMatrices, value->maxPorts);
            }
            fmt::format_to(out, "}}");
        } else if constexpr (std::is_same_v<Pointee, OrbisNgs2BufferAllocator>) {
            fmt::format_to(out, " {{alloc {:#x}, free {:#x}, user data {:#x}}}",
                           reinterpret_cast<uintptr_t>(value->allocHandler),
                           reinterpret_cast<uintptr_t>(value->freeHandler), value->userData);
        } else if constexpr (std::is_same_v<Pointee, OrbisNgs2VoiceParamHeader>) {
            fmt::format_to(out, " {{id {:#x}, size {:#x}, next {}}}", value->id, value->size, value->next);
        } else if constexpr (std::is_same_v<Pointee, char>) {
            fmt::format_to(out, " \"{:.128}\"", value);
        } else if constexpr (!std::is_const_v<std::remove_pointer_t<T>> &&
                             (std::is_same_v<Pointee, uintptr_t> || std::is_same_v<Pointee, u32>)) {
            if (has_succeeded) {
                fmt::format_to(out, " -> {:#x}", *value);
            }
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        fmt::format_to(out, "{}", value);
    } else {
        fmt::format_to(out, "{:#x}", value);
    }
}

template <class... Args>
void Trace(const char* sz_export, std::atomic<u32>* p_c_traced, s32 result, Args... args) {
    const u32 i_call = p_c_traced->fetch_add(1, std::memory_order_relaxed);
    if (i_call > kTracedCallsPerExport) {
        return;
    }
    if (i_call == kTracedCallsPerExport) {
        std::printf("Ngs2 trace: %s: calls past the first %u not traced\n", sz_export, kTracedCallsPerExport);
        return;
    }
    fmt::memory_buffer text;
    bool is_first = true;
    const auto describe = [&](auto value) {
        if (!is_first) {
            text.append(std::string_view{", "});
        }
        is_first = false;
        DescribeArgument(text, value, result == ORBIS_OK);
    };
    (describe(args), ...);
    std::printf("Ngs2 trace: %s(%.*s) -> %#x\n", sz_export, static_cast<int>(text.size()), text.data(),
                static_cast<u32>(result));
}

template <auto F>
struct Observed;

template <class... Args, s32 (PS4_SYSV_ABI* F)(Args...)>
struct Observed<F> {
    static inline const char* s_sz_name;
    static inline std::atomic<u32> s_c_traced;

    static s32 PS4_SYSV_ABI Call(Args... args) {
        s32 result;
        {
            ExportCall call{s_sz_name};
            result = F(args...);
            call.NoteResult(result);
        }
        if (IsTracing()) {
            Trace(s_sz_name, &s_c_traced, result, args...);
        }
        return result;
    }
};

template <auto F>
auto Observe(const char* sz_name) {
    Observed<F>::s_sz_name = sz_name;
    return &Observed<F>::Call;
}

} // namespace

#define NGS2_EXPORT(nid, function) LIB_FUNCTION(nid, "libSceNgs2", 1, "libSceNgs2", Observe<&function>(#function))

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    NGS2_EXPORT("0eFLVCfWVds", sceNgs2RackQueryBufferSize);
    NGS2_EXPORT("cLV4aiT9JpA", sceNgs2RackCreate);
    NGS2_EXPORT("U546k6orxQo", sceNgs2RackCreateWithAllocator);
    NGS2_EXPORT("lCqD7oycmIM", sceNgs2RackDestroy);
    NGS2_EXPORT("MzTa7VLjogY", sceNgs2RackLock);
    NGS2_EXPORT("++YZ7P9e87U", sceNgs2RackUnlock);
    NGS2_EXPORT("JNTMIaBIbV4", sceNgs2RackSetUserData);
    NGS2_EXPORT("Mn4XNDg03XY", sceNgs2RackGetUserData);
    NGS2_EXPORT("MwmHz8pAdAo", sceNgs2RackGetVoiceHandle);
    NGS2_EXPORT("M4LYATRhRUE", sceNgs2RackGetInfo);
    NGS2_EXPORT("uu94irFOGpA", sceNgs2VoiceControl);
    NGS2_EXPORT("rEh728kXk3w", sceNgs2VoiceGetStateFlags);
    NGS2_EXPORT("-TOuuAQ-buE", sceNgs2VoiceGetState);
    NGS2_EXPORT("W-Z8wWMBnhk", sceNgs2VoiceGetOwner);
    NGS2_EXPORT("jjBVvPN9964", sceNgs2VoiceGetMatrixInfo);
    NGS2_EXPORT("WCayTgob7-o", sceNgs2VoiceGetPortInfo);
    NGS2_EXPORT("uBIN24Tv2MI", sceNgs2ReportRegisterHandler);
    NGS2_EXPORT("nPzb7Ly-VjE", sceNgs2ReportUnregisterHandler);
    NGS2_EXPORT("pgFAiLR5qT4", sceNgs2SystemQueryBufferSize);
    NGS2_EXPORT("koBbCMvOKWw", sceNgs2SystemCreate);
    NGS2_EXPORT("mPYgU4oYpuY", sceNgs2SystemCreateWithAllocator);
    NGS2_EXPORT("u-WrYDaJA3k", sceNgs2SystemDestroy);
    NGS2_EXPORT("gThZqM5PYlQ", sceNgs2SystemLock);
    NGS2_EXPORT("JXRC5n0RQls", sceNgs2SystemUnlock);
    NGS2_EXPORT("GZB2v0XnG0k", sceNgs2SystemSetUserData);
    NGS2_EXPORT("4lFaRxd-aLs", sceNgs2SystemGetUserData);
    NGS2_EXPORT("vU7TQ62pItw", sceNgs2SystemGetInfo);
    NGS2_EXPORT("l4Q2dWEH6UM", sceNgs2SystemSetGrainSamples);
    NGS2_EXPORT("-tbc2SxQD60", sceNgs2SystemSetSampleRate);
    NGS2_EXPORT("i0VnXM-C9fc", sceNgs2SystemRender);
    NGS2_EXPORT("U-+7HsswcIs", sceNgs2SystemEnumRackHandles);
    NGS2_EXPORT("vubFP0T6MP0", sceNgs2SystemEnumHandles);
    NGS2_EXPORT("6qN1zaEZuN0", sceNgs2CustomRackGetModuleInfo);
    NGS2_EXPORT("hyVLT2VlOYk", sceNgs2ParseWaveformData);
    NGS2_EXPORT("iprCTXPVWMI", sceNgs2ParseWaveformFile);
    NGS2_EXPORT("t9T0QM17Kvo", sceNgs2ParseWaveformUser);
    NGS2_EXPORT("3pCNbVM11UA", sceNgs2CalcWaveformBlock);
    NGS2_EXPORT("ekGJmmoc8j4", sceNgs2GetWaveformFrameInfo);
    NGS2_EXPORT("xa8oL9dmXkM", sceNgs2PanInit);
    NGS2_EXPORT("gbMKV+8Enuo", sceNgs2PanGetVolumeMatrix);
    NGS2_EXPORT("1WsleK-MTkE", sceNgs2GeomCalcListener);
    NGS2_EXPORT("7Lcfo8SmpsU", sceNgs2GeomResetListenerParam);
    NGS2_EXPORT("0lbbayqDNoE", sceNgs2GeomResetSourceParam);
    NGS2_EXPORT("eF8yRCC6W64", sceNgs2GeomApply);
    NGS2_EXPORT("-YNfTO6KOMY", sceNgs2FftQuerySize);
    NGS2_EXPORT("Kg1MA5j7KFk", sceNgs2FftInit);
    NGS2_EXPORT("D8eCqBxSojA", sceNgs2FftProcess);
    RegisterExports(sym, std::make_index_sequence<UNIMPLEMENTED_EXPORTS.size()>{});
    if (IsTracing()) {
        std::printf("Ngs2 trace: on (native): the first %u calls of each export with their arguments and result\n",
                    kTracedCallsPerExport);
    }
}

#undef NGS2_EXPORT

} // namespace Libraries::Ngs2
