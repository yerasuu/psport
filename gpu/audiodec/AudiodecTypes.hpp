// SPDX-License-Identifier: GPL-2.0-only
// AnyPS5's libSceAudiodec structures (core/libs/SceTypes.hpp); the PS4 library takes the same layouts.
#pragma once
#include <cstdint>

struct AudiodecAuInfo {
    std::uint32_t ui_size;
    void* p_au_addr;
    std::uint32_t ui_au_size;
};

struct AudiodecPcmItem {
    std::uint32_t ui_size;
    void* p_pcm_addr;
    std::uint32_t ui_pcm_size;
};

struct AudiodecCtrl {
    void* pParam;
    void* pBsiInfo;
    AudiodecAuInfo* pAuInfo;
    AudiodecPcmItem* pPcmItem;
};

struct AudiodecParamAt9 {
    std::uint32_t ui_size;
    std::int32_t i_bw_pcm;
    std::uint8_t ui_config_data[4];
};

struct AudiodecAt9Info {
    std::uint32_t ui_size;
    std::uint32_t ui_channel;
    std::uint32_t ui_bitrate;
    std::uint32_t ui_sampling_rate;
    std::uint32_t ui_super_frame_size;
    std::uint32_t ui_frames_in_super_frame;
    std::uint32_t ui_next_frame_size;
    std::uint32_t ui_frame_samples;
    std::int32_t i_result;
};

struct AudiodecParamMp3 {
    std::uint32_t ui_size;
    std::int32_t i_bw_pcm;
};

struct AudiodecMp3Info {
    std::uint32_t ui_size;
    std::uint32_t ui_header;
    std::uint8_t uc_crc;
    std::uint8_t uc_mode;
    std::uint8_t uc_mode_extension;
    std::uint8_t uc_copyright;
    std::uint8_t uc_original;
    std::uint8_t uc_emphasis;
    std::uint8_t uc_reserved[2];
    std::int32_t i_result;
};

struct AudiodecParamM4aac {
    std::uint32_t ui_size;
    std::int32_t i_bw_pcm;
    std::uint32_t ui_config_number;
    std::uint32_t ui_sampling_freq_index;
    std::uint32_t ui_max_channels;
    std::uint32_t ui_enable_heaac;
};

struct AudiodecM4aacInfo {
    std::uint32_t ui_size;
    std::uint32_t ui_sampling_freq;
    std::uint32_t ui_number_of_channels;
    std::uint32_t ui_heaac;
    std::int32_t i_result;
};
