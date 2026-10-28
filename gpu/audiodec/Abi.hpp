// SPDX-License-Identifier: GPL-2.0-or-later
// libSceAudiodec as the game links against it: codec types, PCM word sizes, error codes and the
// structures it passes, with the PS4's 64-bit layouts.
#pragma once
#include <cstdint>

namespace Audiodec::Abi {

constexpr std::uint32_t ORBIS_AUDIODEC_TYPE_AT9 = 1;
constexpr std::uint32_t ORBIS_AUDIODEC_TYPE_MP3 = 2;
constexpr std::uint32_t ORBIS_AUDIODEC_TYPE_M4AAC = 3;
constexpr std::uint32_t C_CODEC_TYPES = 4;

constexpr std::int32_t ORBIS_AUDIODEC_WORD_24BIT = 0;
constexpr std::int32_t ORBIS_AUDIODEC_WORD_16BIT = 1;
constexpr std::int32_t ORBIS_AUDIODEC_WORD_FLOAT = 2;

constexpr std::uint32_t ORBIS_AUDIODEC_M4AAC_CONFIG_ADTS = 1;
constexpr std::uint32_t ORBIS_AUDIODEC_M4AAC_CONFIG_RAW = 2;
constexpr std::uint32_t ORBIS_AUDIODEC_M4AAC_MAX_CHANNELS = 8;
constexpr std::int32_t ORBIS_AUDIODEC_M4AAC_RESULT_DECODE_ERROR = -4;
constexpr std::int32_t ORBIS_AUDIODEC_M4AAC_RESULT_INSUFFICIENT_DATA = -5;

constexpr std::int32_t ORBIS_AUDIODEC_ERROR_API_FAIL = static_cast<std::int32_t>(0x807F0000);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_TYPE = static_cast<std::int32_t>(0x807F0001);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_ARG = static_cast<std::int32_t>(0x807F0002);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_PARAM_SIZE = static_cast<std::int32_t>(0x807F0004);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_BSI_INFO_SIZE = static_cast<std::int32_t>(0x807F0005);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_AU_INFO_SIZE = static_cast<std::int32_t>(0x807F0006);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_PCM_ITEM_SIZE = static_cast<std::int32_t>(0x807F0007);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_CTRL_POINTER = static_cast<std::int32_t>(0x807F0008);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_PARAM_POINTER = static_cast<std::int32_t>(0x807F0009);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_BSI_INFO_POINTER = static_cast<std::int32_t>(0x807F000A);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_AU_INFO_POINTER = static_cast<std::int32_t>(0x807F000B);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_PCM_ITEM_POINTER = static_cast<std::int32_t>(0x807F000C);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_AU_POINTER = static_cast<std::int32_t>(0x807F000D);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_PCM_POINTER = static_cast<std::int32_t>(0x807F000E);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_HANDLE = static_cast<std::int32_t>(0x807F000F);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_WORD_LENGTH = static_cast<std::int32_t>(0x807F0010);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_AU_SIZE = static_cast<std::int32_t>(0x807F0011);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_INVALID_PCM_SIZE = static_cast<std::int32_t>(0x807F0012);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_M4AAC_INVALID_SAMPLING_FREQ = static_cast<std::int32_t>(0x807F0300);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_M4AAC_INVALID_ENABLE_HEAAC = static_cast<std::int32_t>(0x807F0302);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_M4AAC_INVALID_CONFIG_NUMBER = static_cast<std::int32_t>(0x807F0303);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_M4AAC_INVALID_MAX_CHANNELS = static_cast<std::int32_t>(0x807F0304);
constexpr std::int32_t ORBIS_AUDIODEC_ERROR_AT9_INVALID_CONFIG_DATA = static_cast<std::int32_t>(0x807F1000);

struct SceAudiodecAuInfo {
    std::uint32_t uiSize;
    void* pAuAddr;
    std::uint32_t uiAuSize;
};

struct SceAudiodecPcmItem {
    std::uint32_t uiSize;
    void* pPcmAddr;
    std::uint32_t uiPcmSize;
};

struct SceAudiodecCtrl {
    void* pParam;
    void* pBsiInfo;
    SceAudiodecAuInfo* pAuInfo;
    SceAudiodecPcmItem* pPcmItem;
};

// Every codec's parameter block starts with these two fields.
struct SceAudiodecParamHeader {
    std::uint32_t uiSize;
    std::int32_t iBwPcm;
};

struct SceAudiodecParamAt9 {
    std::uint32_t uiSize;
    std::int32_t iBwPcm;
    std::uint8_t uiConfigData[4];
};

struct SceAudiodecAt9Info {
    std::uint32_t uiSize;
    std::uint32_t uiChannels;
    std::uint32_t uiBitRate;
    std::uint32_t uiSamplingRate;
    std::uint32_t uiSuperFrameSize;
    std::uint32_t uiFramesInSuperFrame;
    std::uint32_t uiNextFrameSize;
    std::uint32_t uiFrameSamples;
    std::int32_t iResult;
};

struct SceAudiodecParamMp3 {
    std::uint32_t uiSize;
    std::int32_t iBwPcm;
};

struct SceAudiodecMp3Info {
    std::uint32_t uiSize;
    std::uint32_t uiHeader;
    std::uint8_t ucCrc;
    std::uint8_t ucMode;
    std::uint8_t ucModeExtension;
    std::uint8_t ucCopyright;
    std::uint8_t ucOriginal;
    std::uint8_t ucEmphasis;
    std::uint8_t ucReserved[2];
    std::int32_t iResult;
};

struct SceAudiodecParamM4aac {
    std::uint32_t uiSize;
    std::int32_t iBwPcm;
    std::uint32_t uiConfigNumber;
    std::uint32_t uiSamplingFreqIndex;
    std::uint32_t uiMaxChannels;
    std::uint32_t uiEnableHeaac;
};

struct SceAudiodecM4aacInfo {
    std::uint32_t uiSize;
    std::uint32_t uiSamplingFreq;
    std::uint32_t uiNumberOfChannels;
    std::uint32_t uiHeaac;
    std::int32_t iResult;
};

static_assert(sizeof(SceAudiodecAuInfo) == 24 && sizeof(SceAudiodecPcmItem) == 24 && sizeof(SceAudiodecCtrl) == 32);
static_assert(sizeof(SceAudiodecParamAt9) == 12 && sizeof(SceAudiodecAt9Info) == 36);
static_assert(sizeof(SceAudiodecParamMp3) == 8 && sizeof(SceAudiodecMp3Info) == 20);
static_assert(sizeof(SceAudiodecParamM4aac) == 24 && sizeof(SceAudiodecM4aacInfo) == 20);

}
