// SPDX-License-Identifier: GPL-2.0-or-later
// The codecs behind libSceAudiodec and libSceAjm's MP3 and AAC instances: MP3 and AAC over FFmpeg's
// libavcodec, ATRAC9 over LibAtrac9. Each Decode takes one access unit (an MP3 frame, an ADTS frame,
// a raw AAC access unit or an ATRAC9 superframe) from the start of its input and writes interleaved PCM.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Audiodec {

enum class PcmFormat { S16, Float };

enum class UnitStatus { Decoded, NeedsMoreInput, Undecodable, PcmTooSmall };

struct Mp3FrameHeader {
    std::uint32_t word;
    std::uint32_t bitrate_bps;
    std::uint32_t sample_rate_hz;
    std::uint32_t c_samples;
    std::size_t cb_frame;
};

struct DecodedFrame {
    UnitStatus status;
    std::size_t cb_consumed;
    std::size_t cb_produced;
    std::uint32_t c_channels;
    std::uint32_t sample_rate_hz;
    bool has_sbr;
    Mp3FrameHeader mp3;
};

struct Atrac9Layout {
    std::uint32_t c_channels;
    std::uint32_t sample_rate_hz;
    std::uint32_t cb_superframe;
    std::uint32_t c_frames_per_superframe;
    std::uint32_t c_frame_samples;
};

class UnitDecoder {
public:
    virtual ~UnitDecoder() = default;
    virtual DecodedFrame Decode(const std::uint8_t* p_au, std::size_t cb_au, std::uint8_t* p_pcm, std::size_t cb_pcm) = 0;
    virtual void Reset() = 0;
};

// False for anything but a valid MPEG audio frame header with a fixed bitrate at p_data.
bool ParseMp3FrameHeader(const std::uint8_t* p_data, std::size_t cb_data, Mp3FrameHeader& out_header);
// 0 for an index MPEG-4 does not assign a frequency to.
std::uint32_t GetAacFrequencyHz(std::uint32_t i_frequency);

// Each returns nullptr, after logging why, when the codec cannot be opened with these parameters.
std::unique_ptr<UnitDecoder> OpenMp3(PcmFormat format);
std::unique_ptr<UnitDecoder> OpenAac(PcmFormat format, bool is_adts, std::uint32_t i_frequency, std::uint32_t c_channels);
std::unique_ptr<UnitDecoder> OpenAtrac9(PcmFormat format, const std::uint8_t (&config)[4], Atrac9Layout& out_layout);

// Prints the 1st, 2nd, 4th, 8th... occurrence with its count, so a failure repeated on every access
// unit stays visible without flooding the log.
void LogCounted(std::atomic<std::uint32_t>& c_seen, const char* sz_format, ...) __attribute__((format(printf, 2, 3)));

}
