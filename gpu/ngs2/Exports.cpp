// SPDX-License-Identifier: GPL-2.0-or-later
// libSceNgs2's export table for the native Ngs2 (PS_NGS2=native, kb #625). The system, rack and
// report API is implemented (phase F2, kb #1094); what is still missing returns
// ORBIS_NGS2_ERROR_FAIL, the firmware's internal failure, logs its first call and is counted in the
// call report, so a title on this backend shows what it still needs.
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_error.h"

#include "Internal.h"

namespace Libraries::Ngs2 {
namespace {

struct Export {
    const char* sz_nid;
    const char* sz_name;
};

// The exports not implemented yet: 20 of firmware 11.00's 47, then the three PS5 ones
// gpu/ngs2_anyps5 also registers, so that switching backends leaves every import bound the same.
constexpr std::array UNIMPLEMENTED_EXPORTS{
    Export{"uu94irFOGpA", "sceNgs2VoiceControl"},
    Export{"rEh728kXk3w", "sceNgs2VoiceGetStateFlags"},
    Export{"-TOuuAQ-buE", "sceNgs2VoiceGetState"},
    Export{"W-Z8wWMBnhk", "sceNgs2VoiceGetOwner"},
    Export{"jjBVvPN9964", "sceNgs2VoiceGetMatrixInfo"},
    Export{"WCayTgob7-o", "sceNgs2VoiceGetPortInfo"},
    Export{"-YNfTO6KOMY", "sceNgs2FftQuerySize"},
    Export{"Kg1MA5j7KFk", "sceNgs2FftInit"},
    Export{"D8eCqBxSojA", "sceNgs2FftProcess"},
    Export{"1WsleK-MTkE", "sceNgs2GeomCalcListener"},
    Export{"7Lcfo8SmpsU", "sceNgs2GeomResetListenerParam"},
    Export{"0lbbayqDNoE", "sceNgs2GeomResetSourceParam"},
    Export{"eF8yRCC6W64", "sceNgs2GeomApply"},
    Export{"xa8oL9dmXkM", "sceNgs2PanInit"},
    Export{"gbMKV+8Enuo", "sceNgs2PanGetVolumeMatrix"},
    Export{"hyVLT2VlOYk", "sceNgs2ParseWaveformData"},
    Export{"iprCTXPVWMI", "sceNgs2ParseWaveformFile"},
    Export{"t9T0QM17Kvo", "sceNgs2ParseWaveformUser"},
    Export{"3pCNbVM11UA", "sceNgs2CalcWaveformBlock"},
    Export{"ekGJmmoc8j4", "sceNgs2GetWaveformFrameInfo"},
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

} // namespace

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("0eFLVCfWVds", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackQueryBufferSize);
    LIB_FUNCTION("cLV4aiT9JpA", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackCreate);
    LIB_FUNCTION("U546k6orxQo", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackCreateWithAllocator);
    LIB_FUNCTION("lCqD7oycmIM", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackDestroy);
    LIB_FUNCTION("MzTa7VLjogY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackLock);
    LIB_FUNCTION("++YZ7P9e87U", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackUnlock);
    LIB_FUNCTION("JNTMIaBIbV4", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackSetUserData);
    LIB_FUNCTION("Mn4XNDg03XY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetUserData);
    LIB_FUNCTION("MwmHz8pAdAo", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetVoiceHandle);
    LIB_FUNCTION("M4LYATRhRUE", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetInfo);
    LIB_FUNCTION("uBIN24Tv2MI", "libSceNgs2", 1, "libSceNgs2", sceNgs2ReportRegisterHandler);
    LIB_FUNCTION("nPzb7Ly-VjE", "libSceNgs2", 1, "libSceNgs2", sceNgs2ReportUnregisterHandler);
    LIB_FUNCTION("pgFAiLR5qT4", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemQueryBufferSize);
    LIB_FUNCTION("koBbCMvOKWw", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemCreate);
    LIB_FUNCTION("mPYgU4oYpuY", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemCreateWithAllocator);
    LIB_FUNCTION("u-WrYDaJA3k", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemDestroy);
    LIB_FUNCTION("gThZqM5PYlQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemLock);
    LIB_FUNCTION("JXRC5n0RQls", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemUnlock);
    LIB_FUNCTION("GZB2v0XnG0k", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetUserData);
    LIB_FUNCTION("4lFaRxd-aLs", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemGetUserData);
    LIB_FUNCTION("vU7TQ62pItw", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemGetInfo);
    LIB_FUNCTION("l4Q2dWEH6UM", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetGrainSamples);
    LIB_FUNCTION("-tbc2SxQD60", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetSampleRate);
    LIB_FUNCTION("i0VnXM-C9fc", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemRender);
    LIB_FUNCTION("U-+7HsswcIs", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemEnumRackHandles);
    LIB_FUNCTION("vubFP0T6MP0", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemEnumHandles);
    LIB_FUNCTION("6qN1zaEZuN0", "libSceNgs2", 1, "libSceNgs2", sceNgs2CustomRackGetModuleInfo);
    RegisterExports(sym, std::make_index_sequence<UNIMPLEMENTED_EXPORTS.size()>{});
}

} // namespace Libraries::Ngs2
