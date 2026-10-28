// SPDX-License-Identifier: GPL-2.0-or-later
// libSceAudiodec's exports: decoder handles 1 to C_DECODER_SLOTS over the codecs in Decoders.cpp.
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include "Abi.hpp"
#include "Decoders.hpp"
#include "common/types.h"
#include "core/libraries/libs.h"

namespace Libraries::Audiodec {

namespace {

using namespace ::Audiodec;
using namespace ::Audiodec::Abi;

constexpr std::size_t C_DECODER_SLOTS = 64;

struct DecoderSlot {
    std::mutex lock;
    std::uint32_t codec = 0;
    std::unique_ptr<UnitDecoder> p_decoder;
    Atrac9Layout atrac9_layout{};
};

// Create and delete take the table exclusively; decodes on different handles run side by side.
std::shared_mutex s_table_lock;
std::array<DecoderSlot, C_DECODER_SLOTS> s_slots;
std::array<std::atomic<std::uint32_t>, C_CODEC_TYPES> s_c_initialized{};

bool IsCodec(std::uint32_t codec) { return codec == ORBIS_AUDIODEC_TYPE_AT9 || codec == ORBIS_AUDIODEC_TYPE_MP3 || codec == ORBIS_AUDIODEC_TYPE_M4AAC; }

const char* GetCodecName(std::uint32_t codec) {
    switch (codec) {
        case ORBIS_AUDIODEC_TYPE_AT9: return "ATRAC9";
        case ORBIS_AUDIODEC_TYPE_MP3: return "MP3";
        case ORBIS_AUDIODEC_TYPE_M4AAC: return "M4AAC";
        default: return "?";
    }
}

std::int32_t RejectCodec(std::uint32_t codec) {
    static std::atomic<std::uint32_t> s_c_rejected{0};
    LogCounted(s_c_rejected, "codec type %u is not one of ATRAC9 (1), MP3 (2) or M4AAC (3)", codec);
    return ORBIS_AUDIODEC_ERROR_INVALID_TYPE;
}

std::int32_t CheckWordSize(std::int32_t word_size) {
    if (word_size == ORBIS_AUDIODEC_WORD_16BIT || word_size == ORBIS_AUDIODEC_WORD_FLOAT) return 0;
    static std::atomic<std::uint32_t> s_c_24bit{0}, s_c_invalid{0};
    if (word_size == ORBIS_AUDIODEC_WORD_24BIT) {
        LogCounted(s_c_24bit, "24-bit PCM output is not implemented");
    } else {
        LogCounted(s_c_invalid, "PCM word size %d is not valid", word_size);
    }
    return ORBIS_AUDIODEC_ERROR_INVALID_WORD_LENGTH;
}

PcmFormat ReadPcmFormat(const void* p_param) {
    return static_cast<const SceAudiodecParamHeader*>(p_param)->iBwPcm == ORBIS_AUDIODEC_WORD_16BIT ? PcmFormat::S16 : PcmFormat::Float;
}

std::int32_t CheckCtrl(const SceAudiodecCtrl* p_ctrl, std::uint32_t codec) {
    if (!p_ctrl) return ORBIS_AUDIODEC_ERROR_INVALID_CTRL_POINTER;
    if (!p_ctrl->pParam) return ORBIS_AUDIODEC_ERROR_INVALID_PARAM_POINTER;
    if (!p_ctrl->pBsiInfo) return ORBIS_AUDIODEC_ERROR_INVALID_BSI_INFO_POINTER;
    const auto& header = *static_cast<const SceAudiodecParamHeader*>(p_ctrl->pParam);
    const std::uint32_t cb_info = *static_cast<const std::uint32_t*>(p_ctrl->pBsiInfo);
    bool is_param_size_valid = false, is_info_size_valid = false;
    switch (codec) {
        case ORBIS_AUDIODEC_TYPE_AT9:
            is_param_size_valid = header.uiSize == sizeof(SceAudiodecParamAt9);
            is_info_size_valid = cb_info == sizeof(SceAudiodecAt9Info);
            break;
        case ORBIS_AUDIODEC_TYPE_MP3:
            is_param_size_valid = header.uiSize == sizeof(SceAudiodecParamMp3);
            is_info_size_valid = cb_info == sizeof(SceAudiodecMp3Info);
            break;
        case ORBIS_AUDIODEC_TYPE_M4AAC:
            is_param_size_valid = header.uiSize >= sizeof(SceAudiodecParamM4aac);
            is_info_size_valid = cb_info == sizeof(SceAudiodecM4aacInfo);
            break;
        default: break;
    }
    if (!is_param_size_valid) return ORBIS_AUDIODEC_ERROR_INVALID_PARAM_SIZE;
    if (!is_info_size_valid) return ORBIS_AUDIODEC_ERROR_INVALID_BSI_INFO_SIZE;
    return CheckWordSize(header.iBwPcm);
}

std::int32_t CheckBuffers(const SceAudiodecCtrl* p_ctrl) {
    const SceAudiodecAuInfo* p_au = p_ctrl->pAuInfo;
    const SceAudiodecPcmItem* p_pcm = p_ctrl->pPcmItem;
    if (!p_au) return ORBIS_AUDIODEC_ERROR_INVALID_AU_INFO_POINTER;
    if (!p_pcm) return ORBIS_AUDIODEC_ERROR_INVALID_PCM_ITEM_POINTER;
    if (p_au->uiSize != sizeof(SceAudiodecAuInfo)) return ORBIS_AUDIODEC_ERROR_INVALID_AU_INFO_SIZE;
    if (p_pcm->uiSize != sizeof(SceAudiodecPcmItem)) return ORBIS_AUDIODEC_ERROR_INVALID_PCM_ITEM_SIZE;
    if (!p_au->pAuAddr) return ORBIS_AUDIODEC_ERROR_INVALID_AU_POINTER;
    if (!p_pcm->pPcmAddr) return ORBIS_AUDIODEC_ERROR_INVALID_PCM_POINTER;
    if (p_au->uiAuSize == 0) return ORBIS_AUDIODEC_ERROR_INVALID_AU_SIZE;
    if (p_pcm->uiPcmSize == 0) return ORBIS_AUDIODEC_ERROR_INVALID_PCM_SIZE;
    return 0;
}

void WriteAt9Info(SceAudiodecAt9Info& info, const Atrac9Layout& layout) {
    const std::uint64_t c_superframe_samples = std::uint64_t{layout.c_frame_samples} * layout.c_frames_per_superframe;
    info.uiChannels = layout.c_channels;
    info.uiBitRate = static_cast<std::uint32_t>(std::uint64_t{layout.cb_superframe} * 8 * layout.sample_rate_hz / c_superframe_samples);
    info.uiSamplingRate = layout.sample_rate_hz;
    info.uiSuperFrameSize = layout.cb_superframe;
    info.uiFramesInSuperFrame = layout.c_frames_per_superframe;
    info.uiNextFrameSize = layout.cb_superframe;
    info.uiFrameSamples = layout.c_frame_samples;
    info.iResult = 0;
}

void WriteMp3Info(SceAudiodecMp3Info& info, std::uint32_t word) {
    info.uiHeader = word;
    info.ucCrc = (word >> 16 & 1) == 0;
    info.ucMode = static_cast<std::uint8_t>(word >> 6 & 3);
    info.ucModeExtension = static_cast<std::uint8_t>(word >> 4 & 3);
    info.ucCopyright = static_cast<std::uint8_t>(word >> 3 & 1);
    info.ucOriginal = static_cast<std::uint8_t>(word >> 2 & 1);
    info.ucEmphasis = static_cast<std::uint8_t>(word & 3);
    info.ucReserved[0] = info.ucReserved[1] = 0;
    info.iResult = 0;
}

std::int32_t OpenM4aac(const SceAudiodecParamM4aac& param, SceAudiodecM4aacInfo& info, DecoderSlot& slot) {
    static std::atomic<std::uint32_t> s_c_rejected{0};
    const bool is_adts = param.uiConfigNumber == ORBIS_AUDIODEC_M4AAC_CONFIG_ADTS;
    if (!is_adts && param.uiConfigNumber != ORBIS_AUDIODEC_M4AAC_CONFIG_RAW) {
        LogCounted(s_c_rejected, "M4AAC config number %u is neither ADTS (1) nor raw (2)", param.uiConfigNumber);
        return ORBIS_AUDIODEC_ERROR_M4AAC_INVALID_CONFIG_NUMBER;
    }
    const std::uint32_t frequency_hz = GetAacFrequencyHz(param.uiSamplingFreqIndex);
    if (!is_adts && frequency_hz == 0) {
        LogCounted(s_c_rejected, "raw M4AAC sampling frequency index %u is not valid", param.uiSamplingFreqIndex);
        return ORBIS_AUDIODEC_ERROR_M4AAC_INVALID_SAMPLING_FREQ;
    }
    if (param.uiMaxChannels > ORBIS_AUDIODEC_M4AAC_MAX_CHANNELS) {
        LogCounted(s_c_rejected, "M4AAC with %u channels is not valid", param.uiMaxChannels);
        return ORBIS_AUDIODEC_ERROR_M4AAC_INVALID_MAX_CHANNELS;
    }
    if (param.uiEnableHeaac > 1) {
        LogCounted(s_c_rejected, "M4AAC HE-AAC switch %u is not valid", param.uiEnableHeaac);
        return ORBIS_AUDIODEC_ERROR_M4AAC_INVALID_ENABLE_HEAAC;
    }
    const std::uint32_t c_channels = param.uiMaxChannels ? param.uiMaxChannels : 2;
    slot.p_decoder = OpenAac(ReadPcmFormat(&param), is_adts, param.uiSamplingFreqIndex, c_channels);
    if (!slot.p_decoder) return ORBIS_AUDIODEC_ERROR_API_FAIL;
    info.uiSamplingFreq = frequency_hz;
    info.uiNumberOfChannels = c_channels;
    info.uiHeaac = 0;
    info.iResult = 0;
    return 0;
}

std::int32_t OpenDecoder(const SceAudiodecCtrl& ctrl, std::uint32_t codec, DecoderSlot& slot) {
    switch (codec) {
        case ORBIS_AUDIODEC_TYPE_AT9: {
            slot.p_decoder = OpenAtrac9(ReadPcmFormat(ctrl.pParam), static_cast<const SceAudiodecParamAt9*>(ctrl.pParam)->uiConfigData, slot.atrac9_layout);
            if (!slot.p_decoder) return ORBIS_AUDIODEC_ERROR_AT9_INVALID_CONFIG_DATA;
            WriteAt9Info(*static_cast<SceAudiodecAt9Info*>(ctrl.pBsiInfo), slot.atrac9_layout);
            return 0;
        }
        case ORBIS_AUDIODEC_TYPE_MP3:
            slot.p_decoder = OpenMp3(ReadPcmFormat(ctrl.pParam));
            return slot.p_decoder ? 0 : ORBIS_AUDIODEC_ERROR_API_FAIL;
        default:
            return OpenM4aac(*static_cast<const SceAudiodecParamM4aac*>(ctrl.pParam), *static_cast<SceAudiodecM4aacInfo*>(ctrl.pBsiInfo), slot);
    }
}

void LogCreate(const SceAudiodecCtrl& ctrl, std::uint32_t codec) {
    static std::atomic<std::uint32_t> s_c_logged{0};
    if (s_c_logged.fetch_add(1, std::memory_order_relaxed) >= 16) return;
    const auto& header = *static_cast<const SceAudiodecParamHeader*>(ctrl.pParam);
    if (codec == ORBIS_AUDIODEC_TYPE_M4AAC) {
        const auto& param = *static_cast<const SceAudiodecParamM4aac*>(ctrl.pParam);
        std::printf("Audiodec: create M4AAC decoder: word size %d, config %u, sampling frequency index %u, %u channels, HE-AAC %u\n",
                    param.iBwPcm, param.uiConfigNumber, param.uiSamplingFreqIndex, param.uiMaxChannels, param.uiEnableHeaac);
    } else {
        std::printf("Audiodec: create %s decoder: word size %d\n", GetCodecName(codec), header.iBwPcm);
    }
}

DecoderSlot* FindSlot(std::int32_t h_decoder) {
    if (h_decoder <= 0 || static_cast<std::size_t>(h_decoder) > C_DECODER_SLOTS) return nullptr;
    DecoderSlot& slot = s_slots[static_cast<std::size_t>(h_decoder - 1)];
    return slot.p_decoder ? &slot : nullptr;
}

std::int32_t RejectHandle(std::int32_t h_decoder) {
    static std::atomic<std::uint32_t> s_c_rejected{0};
    LogCounted(s_c_rejected, "handle %d is not an open decoder", h_decoder);
    return ORBIS_AUDIODEC_ERROR_INVALID_HANDLE;
}

s32 PS4_SYSV_ABI sceAudiodecInitLibrary(u32 codec) {
    if (!IsCodec(codec)) return RejectCodec(codec);
    s_c_initialized[codec].fetch_add(1, std::memory_order_relaxed);
    return 0;
}

s32 PS4_SYSV_ABI sceAudiodecTermLibrary(u32 codec) {
    if (!IsCodec(codec)) return RejectCodec(codec);
    std::uint32_t c_now = s_c_initialized[codec].load(std::memory_order_relaxed);
    while (c_now > 0 && !s_c_initialized[codec].compare_exchange_weak(c_now, c_now - 1, std::memory_order_relaxed)) {
    }
    return 0;
}

s32 PS4_SYSV_ABI sceAudiodecCreateDecoder(SceAudiodecCtrl* p_ctrl, u32 codec) {
    if (!IsCodec(codec)) return RejectCodec(codec);
    if (const std::int32_t error = CheckCtrl(p_ctrl, codec); error != 0) return error;
    LogCreate(*p_ctrl, codec);
    if (s_c_initialized[codec].load(std::memory_order_relaxed) == 0) {
        static std::atomic<std::uint32_t> s_c_uninitialized{0};
        LogCounted(s_c_uninitialized, "%s decoder created before sceAudiodecInitLibrary", GetCodecName(codec));
        return ORBIS_AUDIODEC_ERROR_ARG;
    }
    std::unique_lock table(s_table_lock);
    for (std::size_t i_slot = 0; i_slot < C_DECODER_SLOTS; ++i_slot) {
        DecoderSlot& slot = s_slots[i_slot];
        if (slot.p_decoder) continue;
        slot.codec = codec;
        if (const std::int32_t error = OpenDecoder(*p_ctrl, codec, slot); error != 0) {
            slot.p_decoder.reset();
            return error;
        }
        return static_cast<s32>(i_slot + 1);
    }
    static std::atomic<std::uint32_t> s_c_full{0};
    LogCounted(s_c_full, "all %zu decoder handles are in use", C_DECODER_SLOTS);
    return ORBIS_AUDIODEC_ERROR_API_FAIL;
}

s32 PS4_SYSV_ABI sceAudiodecDeleteDecoder(s32 h_decoder) {
    std::unique_lock table(s_table_lock);
    DecoderSlot* p_slot = FindSlot(h_decoder);
    if (!p_slot) return RejectHandle(h_decoder);
    p_slot->p_decoder.reset();
    return 0;
}

s32 PS4_SYSV_ABI sceAudiodecClearContext(s32 h_decoder) {
    std::shared_lock table(s_table_lock);
    DecoderSlot* p_slot = FindSlot(h_decoder);
    if (!p_slot) return RejectHandle(h_decoder);
    std::lock_guard slot_lock(p_slot->lock);
    p_slot->p_decoder->Reset();
    return 0;
}

s32 PS4_SYSV_ABI sceAudiodecDecode(s32 h_decoder, SceAudiodecCtrl* p_ctrl) {
    std::shared_lock table(s_table_lock);
    DecoderSlot* p_slot = FindSlot(h_decoder);
    if (!p_slot) return RejectHandle(h_decoder);
    if (const std::int32_t error = CheckCtrl(p_ctrl, p_slot->codec); error != 0) return error;
    if (const std::int32_t error = CheckBuffers(p_ctrl); error != 0) return error;

    std::lock_guard slot_lock(p_slot->lock);
    SceAudiodecAuInfo& au = *p_ctrl->pAuInfo;
    SceAudiodecPcmItem& pcm = *p_ctrl->pPcmItem;
    const DecodedFrame frame = p_slot->p_decoder->Decode(static_cast<const std::uint8_t*>(au.pAuAddr), au.uiAuSize,
                                                              static_cast<std::uint8_t*>(pcm.pPcmAddr), pcm.uiPcmSize);
    if (p_slot->codec == ORBIS_AUDIODEC_TYPE_M4AAC) {
        auto& info = *static_cast<SceAudiodecM4aacInfo*>(p_ctrl->pBsiInfo);
        info.iResult = frame.status == UnitStatus::Undecodable    ? ORBIS_AUDIODEC_M4AAC_RESULT_DECODE_ERROR
                       : frame.status == UnitStatus::NeedsMoreInput ? ORBIS_AUDIODEC_M4AAC_RESULT_INSUFFICIENT_DATA
                                                                    : 0;
        if (frame.c_channels) {
            info.uiSamplingFreq = frame.sample_rate_hz;
            info.uiNumberOfChannels = frame.c_channels;
            info.uiHeaac = frame.has_sbr;
        }
    }
    switch (frame.status) {
        case UnitStatus::Decoded: break;
        case UnitStatus::PcmTooSmall: {
            static std::atomic<std::uint32_t> s_c_small{0};
            LogCounted(s_c_small, "%s PCM buffer of %u bytes is too small", GetCodecName(p_slot->codec), pcm.uiPcmSize);
            return ORBIS_AUDIODEC_ERROR_INVALID_PCM_SIZE;
        }
        case UnitStatus::NeedsMoreInput:
        case UnitStatus::Undecodable: {
            static std::atomic<std::uint32_t> s_c_failed{0};
            LogCounted(s_c_failed, "%s access unit of %u bytes %s", GetCodecName(p_slot->codec), au.uiAuSize,
                       frame.status == UnitStatus::NeedsMoreInput ? "is incomplete" : "is not decodable");
            au.uiAuSize = 0;
            pcm.uiPcmSize = 0;
            return ORBIS_AUDIODEC_ERROR_API_FAIL;
        }
    }
    if (p_slot->codec == ORBIS_AUDIODEC_TYPE_MP3) WriteMp3Info(*static_cast<SceAudiodecMp3Info*>(p_ctrl->pBsiInfo), frame.mp3.word);
    if (p_slot->codec == ORBIS_AUDIODEC_TYPE_AT9) WriteAt9Info(*static_cast<SceAudiodecAt9Info*>(p_ctrl->pBsiInfo), p_slot->atrac9_layout);
    au.uiAuSize = static_cast<std::uint32_t>(frame.cb_consumed);
    pcm.uiPcmSize = static_cast<std::uint32_t>(frame.cb_produced);
    return 0;
}

}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("6Vf9WTLDoss", "libSceAudiodec", 1, "libSceAudiodec", sceAudiodecClearContext);
    LIB_FUNCTION("O3f1sLMWRvs", "libSceAudiodec", 1, "libSceAudiodec", sceAudiodecCreateDecoder);
    LIB_FUNCTION("KHXHMDLkILw", "libSceAudiodec", 1, "libSceAudiodec", sceAudiodecDecode);
    LIB_FUNCTION("Tp+ZEy69mLk", "libSceAudiodec", 1, "libSceAudiodec", sceAudiodecDeleteDecoder);
    LIB_FUNCTION("VjhsmxpcezI", "libSceAudiodec", 1, "libSceAudiodec", sceAudiodecInitLibrary);
    LIB_FUNCTION("h5jSB2QIDV0", "libSceAudiodec", 1, "libSceAudiodec", sceAudiodecTermLibrary);
}

}
