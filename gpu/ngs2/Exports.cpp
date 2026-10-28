// SPDX-License-Identifier: GPL-2.0-or-later
// libSceNgs2's export table for the native Ngs2 (PS_NGS2=native, kb #625). Phase F1 registers every
// export and implements none: each returns ORBIS_NGS2_ERROR_FAIL, the firmware's internal failure,
// logs its first call and is counted in the call report, so a title on this backend shows what it
// still needs.
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_error.h"

namespace Libraries::Ngs2 {
namespace {

struct Export {
    const char* sz_nid;
    const char* sz_name;
};

// Firmware 11.00's 47 exports (DT_SCE_SYMTAB of libSceNgs2.sprx), then the three PS5 ones
// gpu/ngs2_anyps5 also registers, so that switching backends leaves every import bound the same.
constexpr std::array EXPORTS{
    Export{"-YNfTO6KOMY", "sceNgs2FftQuerySize"},
    Export{"Kg1MA5j7KFk", "sceNgs2FftInit"},
    Export{"D8eCqBxSojA", "sceNgs2FftProcess"},
    Export{"1WsleK-MTkE", "sceNgs2GeomCalcListener"},
    Export{"7Lcfo8SmpsU", "sceNgs2GeomResetListenerParam"},
    Export{"0lbbayqDNoE", "sceNgs2GeomResetSourceParam"},
    Export{"eF8yRCC6W64", "sceNgs2GeomApply"},
    Export{"xa8oL9dmXkM", "sceNgs2PanInit"},
    Export{"gbMKV+8Enuo", "sceNgs2PanGetVolumeMatrix"},
    Export{"0eFLVCfWVds", "sceNgs2RackQueryBufferSize"},
    Export{"cLV4aiT9JpA", "sceNgs2RackCreate"},
    Export{"U546k6orxQo", "sceNgs2RackCreateWithAllocator"},
    Export{"lCqD7oycmIM", "sceNgs2RackDestroy"},
    Export{"MzTa7VLjogY", "sceNgs2RackLock"},
    Export{"++YZ7P9e87U", "sceNgs2RackUnlock"},
    Export{"JNTMIaBIbV4", "sceNgs2RackSetUserData"},
    Export{"Mn4XNDg03XY", "sceNgs2RackGetUserData"},
    Export{"MwmHz8pAdAo", "sceNgs2RackGetVoiceHandle"},
    Export{"M4LYATRhRUE", "sceNgs2RackGetInfo"},
    Export{"uu94irFOGpA", "sceNgs2VoiceControl"},
    Export{"rEh728kXk3w", "sceNgs2VoiceGetStateFlags"},
    Export{"-TOuuAQ-buE", "sceNgs2VoiceGetState"},
    Export{"W-Z8wWMBnhk", "sceNgs2VoiceGetOwner"},
    Export{"jjBVvPN9964", "sceNgs2VoiceGetMatrixInfo"},
    Export{"WCayTgob7-o", "sceNgs2VoiceGetPortInfo"},
    Export{"uBIN24Tv2MI", "sceNgs2ReportRegisterHandler"},
    Export{"nPzb7Ly-VjE", "sceNgs2ReportUnregisterHandler"},
    Export{"pgFAiLR5qT4", "sceNgs2SystemQueryBufferSize"},
    Export{"koBbCMvOKWw", "sceNgs2SystemCreate"},
    Export{"mPYgU4oYpuY", "sceNgs2SystemCreateWithAllocator"},
    Export{"u-WrYDaJA3k", "sceNgs2SystemDestroy"},
    Export{"gThZqM5PYlQ", "sceNgs2SystemLock"},
    Export{"JXRC5n0RQls", "sceNgs2SystemUnlock"},
    Export{"GZB2v0XnG0k", "sceNgs2SystemSetUserData"},
    Export{"4lFaRxd-aLs", "sceNgs2SystemGetUserData"},
    Export{"vU7TQ62pItw", "sceNgs2SystemGetInfo"},
    Export{"l4Q2dWEH6UM", "sceNgs2SystemSetGrainSamples"},
    Export{"-tbc2SxQD60", "sceNgs2SystemSetSampleRate"},
    Export{"i0VnXM-C9fc", "sceNgs2SystemRender"},
    Export{"U-+7HsswcIs", "sceNgs2SystemEnumRackHandles"},
    Export{"vubFP0T6MP0", "sceNgs2SystemEnumHandles"},
    Export{"hyVLT2VlOYk", "sceNgs2ParseWaveformData"},
    Export{"iprCTXPVWMI", "sceNgs2ParseWaveformFile"},
    Export{"t9T0QM17Kvo", "sceNgs2ParseWaveformUser"},
    Export{"3pCNbVM11UA", "sceNgs2CalcWaveformBlock"},
    Export{"ekGJmmoc8j4", "sceNgs2GetWaveformFrameInfo"},
    Export{"6qN1zaEZuN0", "sceNgs2CustomRackGetModuleInfo"},
    Export{"AQkj7C0f3PY", "sceNgs2SystemResetOption"},
    Export{"9eic4AmjGVI", "sceNgs2VoiceQueryInfo"},
    Export{"AbYvTOZ8Pts", "sceNgs2VoiceRunCommands"},
};

template <std::size_t I>
s32 PS4_SYSV_ABI FailUnimplemented() {
    static std::uint64_t s_c_calls;
    if (__atomic_fetch_add(&s_c_calls, 1, __ATOMIC_RELAXED) == 0) {
        Common::Log::Stubbed(EXPORTS[I].sz_name, &s_c_calls);
        LOG_ERROR(Lib_Ngs2, "{} is not implemented by the native Ngs2 yet; returns {:#x}",
                  EXPORTS[I].sz_name, static_cast<u32>(ORBIS_NGS2_ERROR_FAIL));
    }
    return ORBIS_NGS2_ERROR_FAIL;
}

template <std::size_t... I>
void RegisterExports(Core::Loader::SymbolsResolver* sym, std::index_sequence<I...>) {
    (LIB_FUNCTION(EXPORTS[I].sz_nid, "libSceNgs2", 1, "libSceNgs2", &FailUnimplemented<I>), ...);
}

} // namespace

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    RegisterExports(sym, std::make_index_sequence<EXPORTS.size()>{});
}

} // namespace Libraries::Ngs2
