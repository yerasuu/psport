// SPDX-License-Identifier: GPL-2.0-or-later
#include "Decoders.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>
#include "libatrac9.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace Audiodec {

namespace {

constexpr std::uint32_t AAC_FREQUENCIES_HZ[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                             22050, 16000, 12000, 11025, 8000,  7350};

// Kilobits per second by [is_mpeg1][layer - 1][bitrate index]; index 0 (free format) and 15 are invalid.
constexpr std::uint16_t MP3_BITRATES_KBPS[2][3][15] = {
    {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},
    {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
     {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
     {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
};
// By [version field][sampling frequency index]: MPEG-2.5, reserved, MPEG-2, MPEG-1.
constexpr std::uint32_t MP3_SAMPLE_RATES_HZ[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};

constexpr std::size_t CB_ADTS_HEADER = 7;

std::size_t GetCbSample(PcmFormat format) { return format == PcmFormat::S16 ? sizeof(std::int16_t) : sizeof(float); }

void PutSample(std::uint8_t* p_out, PcmFormat format, float value) {
    if (format == PcmFormat::Float) {
        std::memcpy(p_out, &value, sizeof(value));
        return;
    }
    const auto sample = static_cast<std::int16_t>(std::clamp(std::lrintf(value * 32768.0f), -32768L, 32767L));
    std::memcpy(p_out, &sample, sizeof(sample));
}

DecodedFrame MakeFailure(UnitStatus status) {
    DecodedFrame frame{};
    frame.status = status;
    return frame;
}

struct CodecContextDeleter {
    void operator()(AVCodecContext* p_context) const { avcodec_free_context(&p_context); }
};
struct PacketDeleter {
    void operator()(AVPacket* p_packet) const { av_packet_free(&p_packet); }
};
struct FrameDeleter {
    void operator()(AVFrame* p_frame) const { av_frame_free(&p_frame); }
};

const char* DescribeError(int error, char (&sz_out)[AV_ERROR_MAX_STRING_SIZE]) {
    av_strerror(error, sz_out, sizeof(sz_out));
    return sz_out;
}

enum class Framing { Mp3, Adts, RawAac };

const char* GetFramingName(Framing framing) {
    switch (framing) {
        case Framing::Mp3: return "MP3";
        case Framing::Adts: return "ADTS AAC";
        case Framing::RawAac: return "raw AAC";
    }
    return "?";
}

class LavcDecoder final : public UnitDecoder {
public:
    static std::unique_ptr<UnitDecoder> Open(PcmFormat format, Framing framing, const std::uint8_t* p_extradata, std::size_t cb_extradata) {
        static std::atomic<std::uint32_t> s_c_failures{0};
        const AVCodec* p_codec = avcodec_find_decoder_by_name(framing == Framing::Mp3 ? "mp3float" : "aac");
        if (!p_codec) {
            LogCounted(s_c_failures, "FFmpeg has no %s decoder", GetFramingName(framing));
            return nullptr;
        }
        std::unique_ptr<AVCodecContext, CodecContextDeleter> p_context(avcodec_alloc_context3(p_codec));
        std::unique_ptr<AVPacket, PacketDeleter> p_packet(av_packet_alloc());
        std::unique_ptr<AVFrame, FrameDeleter> p_frame(av_frame_alloc());
        if (!p_context || !p_packet || !p_frame) {
            LogCounted(s_c_failures, "out of memory opening a %s decoder", GetFramingName(framing));
            return nullptr;
        }
        if (cb_extradata) {
            p_context->extradata = static_cast<std::uint8_t*>(av_mallocz(cb_extradata + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!p_context->extradata) {
                LogCounted(s_c_failures, "out of memory opening a %s decoder", GetFramingName(framing));
                return nullptr;
            }
            std::memcpy(p_context->extradata, p_extradata, cb_extradata);
            p_context->extradata_size = static_cast<int>(cb_extradata);
        }
        if (const int error = avcodec_open2(p_context.get(), p_codec, nullptr); error < 0) {
            char sz_error[AV_ERROR_MAX_STRING_SIZE];
            LogCounted(s_c_failures, "FFmpeg cannot open the %s decoder: %s", GetFramingName(framing), DescribeError(error, sz_error));
            return nullptr;
        }
        std::unique_ptr<UnitDecoder> p_decoder(new (std::nothrow) LavcDecoder(format, framing, std::move(p_context), std::move(p_packet), std::move(p_frame)));
        if (!p_decoder) LogCounted(s_c_failures, "out of memory opening a %s decoder", GetFramingName(framing));
        return p_decoder;
    }

    DecodedFrame Decode(const std::uint8_t* p_au, std::size_t cb_au, std::uint8_t* p_pcm, std::size_t cb_pcm) override {
        DecodedFrame frame{};
        frame.status = Delimit(p_au, cb_au, frame);
        if (frame.status != UnitStatus::Decoded) return MakeFailure(frame.status);

        // send_packet copies a packet that holds no buffer reference, so the guest's access unit is only read.
        m_p_packet->data = const_cast<std::uint8_t*>(p_au);
        m_p_packet->size = static_cast<int>(frame.cb_consumed);
        const int sent = avcodec_send_packet(m_p_context.get(), m_p_packet.get());
        m_p_packet->data = nullptr;
        m_p_packet->size = 0;
        if (sent < 0) return Reject("rejected", sent);

        const int received = avcodec_receive_frame(m_p_context.get(), m_p_frame.get());
        if (received == AVERROR(EAGAIN)) return frame;
        if (received < 0) return Reject("failed", received);
        frame.status = Convert(p_pcm, cb_pcm, frame);
        av_frame_unref(m_p_frame.get());
        return frame.status == UnitStatus::Decoded ? frame : MakeFailure(frame.status);
    }

    void Reset() override { avcodec_flush_buffers(m_p_context.get()); }

private:
    LavcDecoder(PcmFormat format, Framing framing, std::unique_ptr<AVCodecContext, CodecContextDeleter> p_context,
                  std::unique_ptr<AVPacket, PacketDeleter> p_packet, std::unique_ptr<AVFrame, FrameDeleter> p_frame)
        : m_format(format), m_framing(framing), m_p_context(std::move(p_context)), m_p_packet(std::move(p_packet)),
          m_p_frame(std::move(p_frame)) {}

    // How many bytes at p_au make the next access unit; a raw AAC access unit is the whole input.
    UnitStatus Delimit(const std::uint8_t* p_au, std::size_t cb_au, DecodedFrame& frame) const {
        std::size_t cb_unit = cb_au;
        if (m_framing == Framing::Mp3) {
            if (cb_au < 4) return UnitStatus::NeedsMoreInput;
            if (!ParseMp3FrameHeader(p_au, cb_au, frame.mp3)) return UnitStatus::Undecodable;
            cb_unit = frame.mp3.cb_frame;
        } else if (m_framing == Framing::Adts) {
            if (cb_au < CB_ADTS_HEADER) return UnitStatus::NeedsMoreInput;
            if (p_au[0] != 0xFF || (p_au[1] & 0xF6) != 0xF0) return UnitStatus::Undecodable;
            cb_unit = (std::size_t{p_au[3]} & 3) << 11 | std::size_t{p_au[4]} << 3 | p_au[5] >> 5;
            const std::size_t cb_header = (p_au[1] & 1) ? CB_ADTS_HEADER : CB_ADTS_HEADER + 2;
            if (cb_unit <= cb_header) return UnitStatus::Undecodable;
        }
        if (cb_unit > cb_au) return UnitStatus::NeedsMoreInput;
        if (cb_unit == 0 || cb_unit > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) return UnitStatus::Undecodable;
        frame.cb_consumed = cb_unit;
        return UnitStatus::Decoded;
    }

    DecodedFrame Reject(const char* sz_what, int error) {
        static std::atomic<std::uint32_t> s_c_rejected{0};
        char sz_error[AV_ERROR_MAX_STRING_SIZE];
        LogCounted(s_c_rejected, "FFmpeg %s a %s access unit: %s", sz_what, GetFramingName(m_framing), DescribeError(error, sz_error));
        return MakeFailure(UnitStatus::Undecodable);
    }

    UnitStatus Convert(std::uint8_t* p_pcm, std::size_t cb_pcm, DecodedFrame& frame) const {
        const AVFrame& decoded = *m_p_frame;
        const auto format = static_cast<AVSampleFormat>(decoded.format);
        if (format != AV_SAMPLE_FMT_FLTP && format != AV_SAMPLE_FMT_FLT) {
            static std::atomic<std::uint32_t> s_c_formats{0};
            LogCounted(s_c_formats, "FFmpeg's %s decoder output sample format %s is not implemented", GetFramingName(m_framing),
                       av_get_sample_fmt_name(format));
            return UnitStatus::Undecodable;
        }
        const auto c_channels = static_cast<std::size_t>(std::max(decoded.ch_layout.nb_channels, 0));
        const auto c_samples = static_cast<std::size_t>(std::max(decoded.nb_samples, 0));
        const std::size_t cb_sample = GetCbSample(m_format);
        if (c_samples * c_channels * cb_sample > cb_pcm) return UnitStatus::PcmTooSmall;
        for (std::size_t i_sample = 0; i_sample < c_samples; ++i_sample) {
            for (std::size_t i_channel = 0; i_channel < c_channels; ++i_channel) {
                const float value = format == AV_SAMPLE_FMT_FLTP ? reinterpret_cast<const float*>(decoded.extended_data[i_channel])[i_sample]
                                                                 : reinterpret_cast<const float*>(decoded.extended_data[0])[i_sample * c_channels + i_channel];
                PutSample(p_pcm + (i_sample * c_channels + i_channel) * cb_sample, m_format, value);
            }
        }
        frame.cb_produced = c_samples * c_channels * cb_sample;
        frame.c_channels = static_cast<std::uint32_t>(c_channels);
        frame.sample_rate_hz = static_cast<std::uint32_t>(std::max(decoded.sample_rate, 0));
        frame.has_sbr = m_framing != Framing::Mp3 && c_samples > 1024;
        return UnitStatus::Decoded;
    }

    PcmFormat m_format;
    Framing m_framing;
    std::unique_ptr<AVCodecContext, CodecContextDeleter> m_p_context;
    std::unique_ptr<AVPacket, PacketDeleter> m_p_packet;
    std::unique_ptr<AVFrame, FrameDeleter> m_p_frame;
};

class Atrac9Decoder final : public UnitDecoder {
public:
    static std::unique_ptr<UnitDecoder> Open(PcmFormat format, const std::uint8_t (&config)[4], Atrac9Layout& out_layout) {
        static std::atomic<std::uint32_t> s_c_failures{0};
        std::unique_ptr<Atrac9Decoder> p_decoder(new (std::nothrow) Atrac9Decoder(format, config));
        if (!p_decoder || !p_decoder->Initialize()) {
            LogCounted(s_c_failures, "ATRAC9 config data %02x %02x %02x %02x is not decodable", config[0], config[1], config[2], config[3]);
            return nullptr;
        }
        out_layout = p_decoder->m_layout;
        return p_decoder;
    }

    ~Atrac9Decoder() override {
        if (m_h_atrac9) Atrac9ReleaseHandle(m_h_atrac9);
    }

    DecodedFrame Decode(const std::uint8_t* p_au, std::size_t cb_au, std::uint8_t* p_pcm, std::size_t cb_pcm) override {
        if (!m_h_atrac9) return MakeFailure(UnitStatus::Undecodable);
        const Atrac9Layout& info = m_layout;
        if (cb_au < info.cb_superframe) return MakeFailure(UnitStatus::NeedsMoreInput);
        const std::size_t c_frame_values = std::size_t{info.c_frame_samples} * info.c_channels;
        const std::size_t cb_frame_pcm = c_frame_values * GetCbSample(m_format);
        if (cb_frame_pcm * info.c_frames_per_superframe > cb_pcm) return MakeFailure(UnitStatus::PcmTooSmall);

        std::size_t off_au = 0;
        for (std::uint32_t i_frame = 0; i_frame < info.c_frames_per_superframe; ++i_frame) {
            int cb_used = 0;
            const int error = Atrac9Decode(m_h_atrac9, p_au + off_au, static_cast<int>(info.cb_superframe - off_au), m_p_scratch.get(), &cb_used);
            if (error != 0 || cb_used < 0 || off_au + static_cast<std::size_t>(cb_used) > info.cb_superframe) {
                static std::atomic<std::uint32_t> s_c_errors{0};
                LogCounted(s_c_errors, "LibAtrac9 failed frame %u of a superframe: %#x", i_frame, static_cast<unsigned>(error));
                return MakeFailure(UnitStatus::Undecodable);
            }
            off_au += static_cast<std::size_t>(cb_used);
            std::uint8_t* p_out = p_pcm + i_frame * cb_frame_pcm;
            if (m_format == PcmFormat::S16) {
                std::memcpy(p_out, m_p_scratch.get(), cb_frame_pcm);
            } else {
                for (std::size_t i = 0; i < c_frame_values; ++i) PutSample(p_out + i * sizeof(float), m_format, m_p_scratch[i] / 32768.0f);
            }
        }
        DecodedFrame frame{};
        frame.status = UnitStatus::Decoded;
        frame.cb_consumed = info.cb_superframe;
        frame.cb_produced = cb_frame_pcm * info.c_frames_per_superframe;
        frame.c_channels = info.c_channels;
        frame.sample_rate_hz = info.sample_rate_hz;
        return frame;
    }

    // LibAtrac9 keeps each channel's overlap in its handle and has no flush, so a fresh handle clears it.
    void Reset() override {
        if (m_h_atrac9) Atrac9ReleaseHandle(m_h_atrac9);
        m_h_atrac9 = nullptr;
        if (!Initialize()) {
            static std::atomic<std::uint32_t> s_c_failures{0};
            LogCounted(s_c_failures, "ATRAC9 decoder could not be reinitialized; it fails every decode");
        }
    }

private:
    Atrac9Decoder(PcmFormat format, const std::uint8_t (&config)[4]) : m_format(format) { std::memcpy(m_config, config, sizeof(m_config)); }

    bool Initialize() {
        m_h_atrac9 = Atrac9GetHandle();
        if (!m_h_atrac9 || Atrac9InitDecoder(m_h_atrac9, m_config) != 0) return false;
        Atrac9CodecInfo codec{};
        if (Atrac9GetCodecInfo(m_h_atrac9, &codec) != 0 || codec.channels <= 0 || codec.samplingRate <= 0 || codec.superframeSize <= 0 ||
            codec.framesInSuperframe <= 0 || codec.frameSamples <= 0) {
            return false;
        }
        m_layout = {static_cast<std::uint32_t>(codec.channels), static_cast<std::uint32_t>(codec.samplingRate),
                         static_cast<std::uint32_t>(codec.superframeSize), static_cast<std::uint32_t>(codec.framesInSuperframe),
                         static_cast<std::uint32_t>(codec.frameSamples)};
        if (!m_p_scratch) m_p_scratch.reset(new (std::nothrow) std::int16_t[std::size_t{m_layout.c_frame_samples} * m_layout.c_channels]);
        return m_p_scratch != nullptr;
    }

    PcmFormat m_format;
    std::uint8_t m_config[4];
    void* m_h_atrac9 = nullptr;
    Atrac9Layout m_layout{};
    std::unique_ptr<std::int16_t[]> m_p_scratch;
};

}

bool ParseMp3FrameHeader(const std::uint8_t* p_data, std::size_t cb_data, Mp3FrameHeader& out_header) {
    if (cb_data < 4) return false;
    const std::uint32_t word = std::uint32_t{p_data[0]} << 24 | std::uint32_t{p_data[1]} << 16 | std::uint32_t{p_data[2]} << 8 | p_data[3];
    const std::uint32_t version = word >> 19 & 3, layer_bits = word >> 17 & 3, i_bitrate = word >> 12 & 15, i_rate = word >> 10 & 3;
    if ((word >> 21) != 0x7FF || version == 1 || layer_bits == 0 || i_bitrate == 0 || i_bitrate == 15 || i_rate == 3) return false;
    const std::uint32_t layer = 4 - layer_bits;
    const bool is_mpeg1 = version == 3;
    const std::uint32_t bitrate_bps = MP3_BITRATES_KBPS[is_mpeg1][layer - 1][i_bitrate] * 1000u;
    const std::uint32_t sample_rate_hz = MP3_SAMPLE_RATES_HZ[version][i_rate];
    const std::uint32_t c_padding_slots = word >> 9 & 1;
    const std::uint32_t c_samples = layer == 1 ? 384 : layer == 3 && !is_mpeg1 ? 576 : 1152;
    out_header.word = word;
    out_header.bitrate_bps = bitrate_bps;
    out_header.sample_rate_hz = sample_rate_hz;
    out_header.c_samples = c_samples;
    out_header.cb_frame = layer == 1 ? (12 * bitrate_bps / sample_rate_hz + c_padding_slots) * 4 : c_samples / 8 * bitrate_bps / sample_rate_hz + c_padding_slots;
    return true;
}

std::uint32_t GetAacFrequencyHz(std::uint32_t i_frequency) {
    return i_frequency < std::size(AAC_FREQUENCIES_HZ) ? AAC_FREQUENCIES_HZ[i_frequency] : 0;
}

std::unique_ptr<UnitDecoder> OpenMp3(PcmFormat format) { return LavcDecoder::Open(format, Framing::Mp3, nullptr, 0); }

std::unique_ptr<UnitDecoder> OpenAac(PcmFormat format, bool is_adts, std::uint32_t i_frequency, std::uint32_t c_channels) {
    if (is_adts) return LavcDecoder::Open(format, Framing::Adts, nullptr, 0);
    static std::atomic<std::uint32_t> s_c_unsupported{0};
    if (GetAacFrequencyHz(i_frequency) == 0) {
        LogCounted(s_c_unsupported, "raw AAC with sampling frequency index %u is not implemented", i_frequency);
        return nullptr;
    }
    // MPEG-4 channel configurations 1 to 6 name their channel count, 7 is 7.1; there is none for 7 channels.
    std::uint32_t channel_config = c_channels;
    if (c_channels == 7 || c_channels == 8) {
        channel_config = 7;
    } else if (c_channels == 0 || c_channels > 8) {
        LogCounted(s_c_unsupported, "raw AAC with %u channels is not implemented; decoding as stereo", c_channels);
        channel_config = 2;
    }
    // AudioSpecificConfig for AAC LC: object type 2, frequency index, channel configuration, GASpecificConfig zeros.
    const std::uint8_t config[2] = {static_cast<std::uint8_t>(2 << 3 | i_frequency >> 1),
                                    static_cast<std::uint8_t>((i_frequency & 1) << 7 | channel_config << 3)};
    return LavcDecoder::Open(format, Framing::RawAac, config, sizeof(config));
}

std::unique_ptr<UnitDecoder> OpenAtrac9(PcmFormat format, const std::uint8_t (&config)[4], Atrac9Layout& out_layout) {
    return Atrac9Decoder::Open(format, config, out_layout);
}

void LogCounted(std::atomic<std::uint32_t>& c_seen, const char* sz_format, ...) {
    const std::uint32_t c_now = c_seen.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((c_now & (c_now - 1)) != 0) return;
    char sz_message[256];
    va_list args;
    va_start(args, sz_format);
    std::vsnprintf(sz_message, sizeof(sz_message), sz_format, args);
    va_end(args);
    std::printf("Audiodec: %s (%u times)\n", sz_message, c_now);
}

}
