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

void LogFailure(const char* call, const std::exception& error) {
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) std::printf("Audiodec: Ajm decoder %s failed: %s\n", call, error.what());
}

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data(data), size(size) {}
    std::uint32_t Read(unsigned bits) {
        std::uint32_t value = 0;
        for (; bits; --bits, ++offset) value = value << 1 | (offset / 8 < size ? data[offset / 8] >> (7 - offset % 8) & 1 : 0);
        return value;
    }
    void Skip(std::size_t bits) { offset += bits; }
    std::size_t Bytes() const { return (offset + 7) / 8; }

private:
    const std::uint8_t* data;
    std::size_t size;
    std::size_t offset = 0;
};

std::uint32_t Big32(const std::uint8_t* p) { return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3]; }

// The end of the frame's side info and main data, where the Xing/Info, VBRI and FGH headers start.
std::size_t OflOffset(const std::uint8_t* frame, std::size_t size, std::uint32_t word) {
    const bool mpeg1 = (word >> 19 & 3) == 3, mono = (word >> 6 & 3) == 3;
    BitReader reader(frame + 4, size - 4);
    if ((word >> 16 & 1) == 0) reader.Skip(16);
    reader.Skip(mpeg1 ? (mono ? 18 : 20) : (mono ? 9 : 10));
    std::uint32_t mainBits = 0;
    for (int granule = 0; granule < (mpeg1 ? 2 : 1); ++granule) {
        for (int channel = 0; channel < (mono ? 1 : 2); ++channel) {
            mainBits += reader.Read(12);
            reader.Skip(mpeg1 ? 47 : 51);
        }
    }
    reader.Skip(mainBits);
    return std::min(4 + reader.Bytes(), size);
}

bool ReadFgh(const std::uint8_t* p, const std::uint8_t* end, std::uint32_t& delay, std::uint32_t& total) {
    if (p + 10 > end) return false;
    while (p + 9 < end && *p != 0xB4) ++p;
    if (p + 9 >= end) return false;
    std::uint8_t crc = 0xFF;
    for (int i = 0; i < 9; ++i) {
        for (int bit = 7; bit >= 0; --bit) crc = static_cast<std::uint8_t>((crc >> 7 & 1) != (p[i] >> bit & 1) ? crc * 2 : (crc * 2) ^ 0x45);
    }
    if (p[9] != crc) return false;
    delay = std::uint32_t{p[1]} << 8 | p[2];
    total = Big32(p + 3);
    return true;
}

bool ReadLame(const std::uint8_t* p, const std::uint8_t* end, std::uint32_t frameSamples, std::uint32_t& delay, std::uint32_t& total) {
    const std::uint8_t flags = p[7];
    const std::uint32_t frames = flags & 1 && p + 12 <= end ? Big32(p + 8) : 0;
    p += 8 + (flags & 1 ? 4 : 0) + (flags & 2 ? 4 : 0) + (flags & 4 ? 100 : 0) + (flags & 8 ? 4 : 0);
    if (p + 0x18 > end || std::memcmp(p, "LAME", 4) != 0) return false;
    const std::uint32_t encoderDelay = std::uint32_t{p[0x15]} << 4 | p[0x16] >> 4;
    const std::uint32_t padding = (std::uint32_t{p[0x16]} & 15) << 8 | p[0x17];
    if (frames) total = frames * frameSamples - (encoderDelay + padding);
    delay = frameSamples + encoderDelay + 529;
    return true;
}

// The gapless window an MP3 frame's encoder header announces: LAME (Xing/Info), VBRI with an optional FGH in
// the next frame, or FGH. LAME's delay counts the header frame itself and the 529-sample decoder delay.
void ReadOfl(const std::uint8_t* frame, std::size_t size, const Audiodec::Mp3Header& header, std::uint32_t& delay, std::uint32_t& total) {
    const std::uint32_t frameSamples = (header.word >> 19 & 3) == 3 ? 1152 : 576;
    const std::uint8_t* end = frame + std::min(size, header.frameBytes);
    const std::uint8_t* p = frame + OflOffset(frame, end - frame, header.word);
    if (p + 8 <= end && (std::memcmp(p, "Xing", 4) == 0 || std::memcmp(p, "Info", 4) == 0)) {
        ReadLame(p, end, frameSamples, delay, total);
    } else if (p + 26 <= end && std::memcmp(p, "VBRI", 4) == 0) {
        delay = std::uint32_t{p[6]} << 8 | p[7];
        Audiodec::Mp3Header next{};
        std::uint32_t nextDelay = 0;
        if (Audiodec::ParseMp3Header(frame + header.frameBytes, size - std::min(size, header.frameBytes), next)) {
            const std::uint8_t* nextEnd = frame + std::min(size, header.frameBytes + next.frameBytes);
            const std::uint8_t* nextStart = frame + header.frameBytes;
            if (ReadFgh(nextStart + OflOffset(nextStart, nextEnd - nextStart, next.word), nextEnd, nextDelay, total)) delay += nextDelay;
        }
    } else {
        ReadFgh(p, end, delay, total);
    }
}

}

extern "C" void* psport_gpu_audio_decoder_create(int aac, int adts, uint32_t frequency_index, uint32_t channels) {
    try {
        auto decoder = aac ? Audiodec::CreateAac(Audiodec::WORD_SIZE_FLOAT, adts != 0, frequency_index, channels) : Audiodec::CreateMp3(Audiodec::WORD_SIZE_FLOAT);
        return decoder.release();
    } catch (const std::exception& error) {
        LogFailure("create", error);
        return nullptr;
    }
}

extern "C" int psport_gpu_audio_decoder_decode(void* decoder, const uint8_t* data, uint64_t size, float* pcm, uint64_t pcm_size, PsAudioFrame* frame) {
    try {
        const Audiodec::DecodeResult decoded = static_cast<Audiodec::Decoder*>(decoder)->Decode(data, size, reinterpret_cast<std::uint8_t*>(pcm), pcm_size);
        switch (decoded.status) {
            case Audiodec::DecodeStatus::Ok: break;
            case Audiodec::DecodeStatus::PartialInput: return 1;
            case Audiodec::DecodeStatus::InvalidData: return 2;
            case Audiodec::DecodeStatus::NotEnoughRoom: return 3;
        }
        const auto samples = decoded.channels ? static_cast<std::uint32_t>(decoded.produced / (sizeof(float) * decoded.channels)) : 0;
        const auto bitrate = decoded.mp3.word ? decoded.mp3.bitrate
            : samples ? static_cast<std::uint32_t>(decoded.consumed * 8 * decoded.sampleRate / samples) : 0;
        *frame = {decoded.consumed, samples, decoded.channels, decoded.sampleRate, bitrate, decoded.mp3.word, decoded.highEfficiency, 0, 0};
        if (decoded.mp3.word) ReadOfl(data, size, decoded.mp3, frame->encoder_delay, frame->total_samples);
        return 0;
    } catch (const std::exception& error) {
        LogFailure("decode", error);
        return 2;
    }
}

extern "C" void psport_gpu_audio_decoder_reset(void* decoder) {
    static_cast<Audiodec::Decoder*>(decoder)->Reset();
}

extern "C" void psport_gpu_audio_decoder_destroy(void* decoder) {
    delete static_cast<Audiodec::Decoder*>(decoder);
}
