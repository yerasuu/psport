// SPDX-License-Identifier: GPL-2.0-or-later
// What the codecs throw becomes a failed call, as an exception must not unwind through the C runtime.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include "../psport_gpu.h"
#include "Codecs.hpp"

namespace {

void LogFailure(const char* sz_call, const std::exception& error) {
    static std::atomic<unsigned> s_logged{0};
    if (s_logged.fetch_add(1, std::memory_order_relaxed) < 8) std::printf("Audiodec: Ajm decoder %s failed: %s\n", sz_call, error.what());
}

class BitReader {
public:
    BitReader(const std::uint8_t* p_data, std::size_t cb_data) : m_p_data(p_data), m_cb_data(cb_data) {}
    std::uint32_t Read(unsigned bits) {
        std::uint32_t value = 0;
        for (; bits; --bits, ++m_off_bits) value = value << 1 | (m_off_bits / 8 < m_cb_data ? m_p_data[m_off_bits / 8] >> (7 - m_off_bits % 8) & 1 : 0);
        return value;
    }
    void Skip(std::size_t bits) { m_off_bits += bits; }
    std::size_t Bytes() const { return (m_off_bits + 7) / 8; }

private:
    const std::uint8_t* m_p_data;
    std::size_t m_cb_data;
    std::size_t m_off_bits = 0;
};

std::uint32_t Big32(const std::uint8_t* p_bytes) { return std::uint32_t{p_bytes[0]} << 24 | std::uint32_t{p_bytes[1]} << 16 | std::uint32_t{p_bytes[2]} << 8 | p_bytes[3]; }

// The end of the frame's side info and main data, where the Xing/Info, VBRI and FGH headers start.
std::size_t OflOffset(const std::uint8_t* p_frame, std::size_t cb_frame, std::uint32_t word) {
    const bool is_mpeg1 = (word >> 19 & 3) == 3, is_mono = (word >> 6 & 3) == 3;
    BitReader reader(p_frame + 4, cb_frame - 4);
    if ((word >> 16 & 1) == 0) reader.Skip(16);
    reader.Skip(is_mpeg1 ? (is_mono ? 18 : 20) : (is_mono ? 9 : 10));
    std::uint32_t main_bits = 0;
    for (int granule = 0; granule < (is_mpeg1 ? 2 : 1); ++granule) {
        for (int channel = 0; channel < (is_mono ? 1 : 2); ++channel) {
            main_bits += reader.Read(12);
            reader.Skip(is_mpeg1 ? 47 : 51);
        }
    }
    reader.Skip(main_bits);
    return std::min(4 + reader.Bytes(), cb_frame);
}

bool ReadFgh(const std::uint8_t* p_cursor, const std::uint8_t* p_end, std::uint32_t& delay, std::uint32_t& total) {
    if (p_cursor + 10 > p_end) return false;
    while (p_cursor + 9 < p_end && *p_cursor != 0xB4) ++p_cursor;
    if (p_cursor + 9 >= p_end) return false;
    std::uint8_t crc = 0xFF;
    for (int i = 0; i < 9; ++i) {
        for (int bit = 7; bit >= 0; --bit) crc = static_cast<std::uint8_t>((crc >> 7 & 1) != (p_cursor[i] >> bit & 1) ? crc * 2 : (crc * 2) ^ 0x45);
    }
    if (p_cursor[9] != crc) return false;
    delay = std::uint32_t{p_cursor[1]} << 8 | p_cursor[2];
    total = Big32(p_cursor + 3);
    return true;
}

bool ReadLame(const std::uint8_t* p_cursor, const std::uint8_t* p_end, std::uint32_t frame_samples, std::uint32_t& delay, std::uint32_t& total) {
    const std::uint8_t flags = p_cursor[7];
    const std::uint32_t frames = flags & 1 && p_cursor + 12 <= p_end ? Big32(p_cursor + 8) : 0;
    p_cursor += 8 + (flags & 1 ? 4 : 0) + (flags & 2 ? 4 : 0) + (flags & 4 ? 100 : 0) + (flags & 8 ? 4 : 0);
    if (p_cursor + 0x18 > p_end || std::memcmp(p_cursor, "LAME", 4) != 0) return false;
    const std::uint32_t encoder_delay = std::uint32_t{p_cursor[0x15]} << 4 | p_cursor[0x16] >> 4;
    const std::uint32_t padding = (std::uint32_t{p_cursor[0x16]} & 15) << 8 | p_cursor[0x17];
    if (frames) total = frames * frame_samples - (encoder_delay + padding);
    delay = frame_samples + encoder_delay + 529;
    return true;
}

// The gapless window an MP3 frame's encoder header announces: LAME (Xing/Info), VBRI with an optional FGH in
// the next frame, or FGH. LAME's delay counts the header frame itself and the 529-sample decoder delay.
void ReadOfl(const std::uint8_t* p_frame, std::size_t cb_frame, const Audiodec::Mp3Header& header, std::uint32_t& delay, std::uint32_t& total) {
    const std::uint32_t frame_samples = (header.word >> 19 & 3) == 3 ? 1152 : 576;
    const std::uint8_t* p_end = p_frame + std::min(cb_frame, header.frameBytes);
    const std::uint8_t* p_cursor = p_frame + OflOffset(p_frame, p_end - p_frame, header.word);
    if (p_cursor + 8 <= p_end && (std::memcmp(p_cursor, "Xing", 4) == 0 || std::memcmp(p_cursor, "Info", 4) == 0)) {
        ReadLame(p_cursor, p_end, frame_samples, delay, total);
    } else if (p_cursor + 26 <= p_end && std::memcmp(p_cursor, "VBRI", 4) == 0) {
        delay = std::uint32_t{p_cursor[6]} << 8 | p_cursor[7];
        Audiodec::Mp3Header next{};
        std::uint32_t next_delay = 0;
        if (Audiodec::ParseMp3Header(p_frame + header.frameBytes, cb_frame - std::min(cb_frame, header.frameBytes), next)) {
            const std::uint8_t* p_next_end = p_frame + std::min(cb_frame, header.frameBytes + next.frameBytes);
            const std::uint8_t* p_next_start = p_frame + header.frameBytes;
            if (ReadFgh(p_next_start + OflOffset(p_next_start, p_next_end - p_next_start, next.word), p_next_end, next_delay, total)) delay += next_delay;
        }
    } else {
        ReadFgh(p_cursor, p_end, delay, total);
    }
}

}

extern "C" void* psport_gpu_audio_decoder_create(int aac, int adts, uint32_t i_frequency, uint32_t channels) {
    try {
        auto decoder = aac ? Audiodec::CreateAac(Audiodec::WORD_SIZE_FLOAT, adts != 0, i_frequency, channels) : Audiodec::CreateMp3(Audiodec::WORD_SIZE_FLOAT);
        return decoder.release();
    } catch (const std::exception& error) {
        LogFailure("create", error);
        return nullptr;
    }
}

extern "C" int psport_gpu_audio_decoder_decode(void* p_decoder, const uint8_t* p_data, uint64_t cb_data, float* p_pcm, uint64_t cb_pcm, PsAudioFrame* p_frame) {
    try {
        const Audiodec::DecodeResult decoded = static_cast<Audiodec::Decoder*>(p_decoder)->Decode(p_data, cb_data, reinterpret_cast<std::uint8_t*>(p_pcm), cb_pcm);
        switch (decoded.status) {
            case Audiodec::DecodeStatus::Ok: break;
            case Audiodec::DecodeStatus::PartialInput: return 1;
            case Audiodec::DecodeStatus::InvalidData: return 2;
            case Audiodec::DecodeStatus::NotEnoughRoom: return 3;
        }
        const auto samples = decoded.channels ? static_cast<std::uint32_t>(decoded.produced / (sizeof(float) * decoded.channels)) : 0;
        const auto bitrate = decoded.mp3.word ? decoded.mp3.bitrate
            : samples ? static_cast<std::uint32_t>(decoded.consumed * 8 * decoded.sampleRate / samples) : 0;
        *p_frame = {decoded.consumed, samples, decoded.channels, decoded.sampleRate, bitrate, decoded.mp3.word, decoded.highEfficiency, 0, 0};
        if (decoded.mp3.word) ReadOfl(p_data, cb_data, decoded.mp3, p_frame->encoder_delay, p_frame->total_samples);
        return 0;
    } catch (const std::exception& error) {
        LogFailure("decode", error);
        return 2;
    }
}

extern "C" void psport_gpu_audio_decoder_reset(void* p_decoder) {
    static_cast<Audiodec::Decoder*>(p_decoder)->Reset();
}

extern "C" void psport_gpu_audio_decoder_destroy(void* p_decoder) {
    delete static_cast<Audiodec::Decoder*>(p_decoder);
}
