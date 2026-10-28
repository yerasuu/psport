// SPDX-License-Identifier: GPL-2.0-or-later
// Waveform files and frames (phase F4, kb #1096): sceNgs2ParseWaveformData, File and User,
// sceNgs2CalcWaveformBlock and sceNgs2GetWaveformFrameInfo (firmware 11.00, 0xfa40 to 0x10a00), and the
// frame shapes the sampler reads (spec 05 §2, §4, §6.1). rea has not read the parse functions yet (spec
// 08 §4): the files are RIFF WAVE (PCM, IEEE float, and WAVE_FORMAT_EXTENSIBLE with those or ATRAC9) and
// VAGp as the formats define them, the answers fill the SDK's OrbisNgs2WaveformInfo, and the order of the
// checks and their codes are psport's until rea reads 0xfac0.
#include <algorithm>
#include <cstring>
#include <fstream>

#include "Internal.h"
#include "core/libraries/kernel/orbis_error.h"

extern "C" int runtime_file_translate(const char* sz_guest, char* sz_out, std::size_t cb_out);

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kMaxWaveformRateHz = 192000;
constexpr u32 kMaxFrameField = 0xffff;
constexpr u32 kInfiniteRepeats = 0xffffffffu;
constexpr u64 kUnknownSize = ~u64(0);
constexpr u32 kMaxPath = 1024;
// A RIFF file is walked chunk by chunk; real waveforms carry a handful, so the walk stops there
// instead of following a hostile file's chain of empty chunks.
constexpr u32 kMaxRiffChunks = 64;
constexpr u32 kRiffHeaderBytes = 12;
constexpr u32 kChunkHeaderBytes = 8;
constexpr u32 kVagHeaderBytes = 0x30;
constexpr u32 kVagScanBytes = 4096;

enum FormatTag : u16 {
    FORMAT_TAG_PCM = 1,
    FORMAT_TAG_FLOAT = 3,
    FORMAT_TAG_EXTENSIBLE = 0xfffe,
};

// The VAG block flags the sampler honours when configData bit 0 is set (spec 05 §4).
enum VagFlag : u32 {
    VAG_FLAG_END = 1,
    VAG_FLAG_LOOP_END = 3,
    VAG_FLAG_LOOP_START = 6,
};

// KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT share every byte but the first two, which hold the tag.
constexpr u8 kBaseSubformatTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                       0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
// {47E142D2-36BA-4D8D-88FC-61654F8C836C}, as an .at9 file stores it.
constexpr u8 kAtrac9Subformat[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                     0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};

constexpr u32 FourCc(const char (&sz_code)[5]) {
    return u32(u8(sz_code[0])) | u32(u8(sz_code[1])) << 8 | u32(u8(sz_code[2])) << 16 | u32(u8(sz_code[3])) << 24;
}

u16 LoadLe16(const u8* p_bytes) {
    return u16(p_bytes[0] | p_bytes[1] << 8);
}

u32 LoadLe32(const u8* p_bytes) {
    return u32(p_bytes[0]) | u32(p_bytes[1]) << 8 | u32(p_bytes[2]) << 16 | u32(p_bytes[3]) << 24;
}

u32 LoadBe32(const u8* p_bytes) {
    return u32(p_bytes[0]) << 24 | u32(p_bytes[1]) << 16 | u32(p_bytes[2]) << 8 | u32(p_bytes[3]);
}

// Where a parse reads from. Read answers ORBIS_OK only when all cb bytes at off were read; Size is
// kUnknownSize behind the game's read handler.
class Reader {
public:
    virtual ~Reader() = default;
    virtual s32 Read(u64 off, void* p_out, u32 cb) = 0;
    virtual u64 Size() const = 0;
};

class DataReader final : public Reader {
public:
    DataReader(const void* p_data, std::size_t cb_data) : m_p_data{static_cast<const u8*>(p_data)}, m_cb_data{cb_data} {}
    s32 Read(u64 off, void* p_out, u32 cb) override {
        if (off > m_cb_data || cb > m_cb_data - off) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE, "Invalid waveform size ({}+{}>{})", off, cb,
                        m_cb_data);
        }
        std::memcpy(p_out, m_p_data + off, cb);
        return ORBIS_OK;
    }
    u64 Size() const override {
        return m_cb_data;
    }

private:
    const u8* m_p_data;
    std::size_t m_cb_data;
};

class FileReader final : public Reader {
public:
    FileReader(std::ifstream& file, u64 off_file, u64 cb_file) : m_file{file}, m_off_file{off_file}, m_cb{cb_file} {}
    s32 Read(u64 off, void* p_out, u32 cb) override {
        if (off > m_cb || cb > m_cb - off) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE, "Invalid waveform size ({}+{}>{})", off, cb, m_cb);
        }
        m_file.clear();
        m_file.seekg(std::streamoff(m_off_file + off));
        if (!m_file.read(static_cast<char*>(p_out), std::streamsize(cb))) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE, "Waveform file read failed ({}+{})", off, cb);
        }
        return ORBIS_OK;
    }
    u64 Size() const override {
        return m_cb;
    }

private:
    std::ifstream& m_file;
    u64 m_off_file;
    u64 m_cb;
};

// The handler's offset is 32 bits wide (OrbisNgs2ParseReadHandler); what it answers below 0 is
// handed back to the game as the parse's result.
class UserReader final : public Reader {
public:
    UserReader(OrbisNgs2ParseReadHandler pfn_read, uintptr_t user_data) : m_pfn_read{pfn_read}, m_user_data{user_data} {}
    s32 Read(u64 off, void* p_out, u32 cb) override {
        if (off > 0xffffffffu) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE, "Invalid waveform offset ({})", off);
        }
        const s32 result = m_pfn_read(m_user_data, u32(off), p_out, cb);
        if (result < 0) {
            return Fail(0, result, "Waveform read handler failed ({}+{})", off, cb);
        }
        return ORBIS_OK;
    }
    u64 Size() const override {
        return kUnknownSize;
    }

private:
    OrbisNgs2ParseReadHandler m_pfn_read;
    uintptr_t m_user_data;
};

s32 CheckChannels(u32 c_channels) {
    if (c_channels < 1 || c_channels > kMaxChannels) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS, "Invaild voice channels ({})", c_channels);
    }
    return ORBIS_OK;
}

s32 CheckSampleRate(u32 rate_hz) {
    if (rate_hz < 1 || rate_hz > kMaxWaveformRateHz) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SAMPLE_RATE, "Invalid waveform sample rate ({})", rate_hz);
    }
    return ORBIS_OK;
}

s32 CheckAtrac9Config(u32 config_data, u32 c_channels) {
    Atrac9Config config;
    if (!ParseAtrac9Config(config_data, &config) || config.c_frame_samples == 0) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG, "Invalid ATRAC9 config data ({:#x})", config_data);
    }
    if (config.c_channels != c_channels) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS, "Invaild voice channels ({}!={})", c_channels,
                    config.c_channels);
    }
    return ORBIS_OK;
}

// The sampler setup's checks (spec 03 §5.1), in its order, for the formats the game hands to
// CalcWaveformBlock and GetWaveformFrameInfo.
s32 CheckFormat(const OrbisNgs2WaveformFormat* p_format, FrameShape* p_out_shape) {
    if (!p_format) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT, "Invalid waveform format address ({})",
                    static_cast<const void*>(p_format));
    }
    const OrbisNgs2WaveformFormat& format = *p_format;
    if (!IsWaveformType(format.waveformType)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_TYPE, "Invalid waveform type ({:#x})", format.waveformType);
    }
    if (const s32 result = CheckChannels(format.numChannels); result < 0) {
        return result;
    }
    if (const s32 result = CheckSampleRate(format.sampleRate); result < 0) {
        return result;
    }
    if (format.frameMargin > kMaxFrameField || format.frameMargin < format.frameOffset) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FRAME, "Invalid waveform frame ({},{})", format.frameOffset,
                    format.frameMargin);
    }
    if (format.waveformType == WAVEFORM_ATRAC9) {
        if (const s32 result = CheckAtrac9Config(format.configData, format.numChannels); result < 0) {
            return result;
        }
    }
    if (!GetFrameShape(format.waveformType, format.numChannels, format.configData, format.frameMargin, p_out_shape)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_TYPE, "Waveform type ({:#x}) has no data",
                    format.waveformType);
    }
    return ORBIS_OK;
}

// The block covering c_samples samples from i_sample on, counted after the codec's delay: whole frames
// from the one holding the first sample, which the block skips up to. Fails when the offset or size
// leave 32 bits.
bool CalcBlock(const FrameShape& shape, u32 c_delay_samples, u64 i_sample, u32 c_samples, u32 num_repeats,
               OrbisNgs2WaveformBlock* p_out_block) {
    const u64 i_position = i_sample + c_delay_samples;
    const u64 i_frame = i_position / shape.c_frame_samples;
    const u64 c_skip = i_position % shape.c_frame_samples;
    const u64 c_frames = (c_skip + c_samples + shape.c_frame_samples - 1) / shape.c_frame_samples;
    const u64 off_data = i_frame * shape.cb_frame;
    const u64 cb_data = c_frames * shape.cb_frame;
    if (off_data > 0xffffffffu || cb_data > 0xffffffffu) {
        return false;
    }
    *p_out_block = {u32(off_data), u32(cb_data), num_repeats, u32(c_skip), c_samples, 0, 0};
    return true;
}

struct ParsedWaveform {
    OrbisNgs2WaveformInfo info;
    FrameShape shape;
    bool has_format;
    bool has_data;
    bool has_fact;
    bool has_fact_delay;
    bool has_loop;
    u32 c_fact_samples;
    u32 c_fact_delay;
    u32 loop_begin;
    u32 loop_end;
    u32 loop_play_count;
};

s32 ParseFormatChunk(const u8* p_chunk, u32 cb_chunk, ParsedWaveform* p_wave) {
    if (cb_chunk < 16) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Invalid fmt chunk size ({})", cb_chunk);
    }
    u16 tag = LoadLe16(p_chunk);
    const u32 c_channels = LoadLe16(p_chunk + 2);
    const u32 rate_hz = LoadLe32(p_chunk + 4);
    const u32 cb_block_align = LoadLe16(p_chunk + 12);
    const u32 c_bits = LoadLe16(p_chunk + 14);
    u32 config_data = 0;
    bool is_atrac9 = false;
    if (tag == FORMAT_TAG_EXTENSIBLE) {
        if (cb_chunk < 40) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Invalid extensible fmt chunk size ({})", cb_chunk);
        }
        const u8* p_subformat = p_chunk + 24;
        if (std::memcmp(p_subformat, kAtrac9Subformat, sizeof(kAtrac9Subformat)) == 0) {
            if (cb_chunk < 48) {
                return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Invalid ATRAC9 fmt chunk size ({})",
                            cb_chunk);
            }
            is_atrac9 = true;
            config_data = LoadBe32(p_chunk + 44);
        } else if (std::memcmp(p_subformat + 2, kBaseSubformatTail, sizeof(kBaseSubformatTail)) == 0) {
            tag = LoadLe16(p_subformat);
        } else {
            return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Unknown waveform subformat");
        }
    }
    if (const s32 result = CheckChannels(c_channels); result < 0) {
        return result;
    }
    if (const s32 result = CheckSampleRate(rate_hz); result < 0) {
        return result;
    }
    u32 waveform_type = 0;
    u32 cb_frame_margin = 0;
    if (is_atrac9) {
        if (const s32 result = CheckAtrac9Config(config_data, c_channels); result < 0) {
            return result;
        }
        waveform_type = WAVEFORM_ATRAC9;
    } else {
        if (tag == FORMAT_TAG_PCM) {
            // 8-bit WAVE samples are unsigned, the wider ones signed.
            waveform_type = c_bits == 8 ? 0x11 : c_bits == 16 ? 0x12 : c_bits == 24 ? 0x14 : c_bits == 32 ? 0x16 : 0;
        } else if (tag == FORMAT_TAG_FLOAT) {
            waveform_type = c_bits == 32 ? 0x18 : c_bits == 64 ? 0x1a : 0;
        } else {
            return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Unknown waveform format tag ({:#x})", tag);
        }
        if (waveform_type == 0) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT, "Invalid waveform bits ({}, tag {:#x})", c_bits,
                        tag);
        }
        const u32 cb_samples = c_channels * BytesPerSample(waveform_type);
        if (cb_block_align < cb_samples) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT, "Invalid waveform block align ({}<{})",
                        cb_block_align, cb_samples);
        }
        cb_frame_margin = cb_block_align - cb_samples;
    }
    p_wave->info.format = {waveform_type, c_channels, rate_hz, config_data, 0, cb_frame_margin};
    GetFrameShape(waveform_type, c_channels, config_data, cb_frame_margin, &p_wave->shape);
    p_wave->has_format = true;
    return ORBIS_OK;
}

void ParseFactChunk(const u8* p_chunk, u32 cb_chunk, ParsedWaveform* p_wave) {
    if (cb_chunk >= 4) {
        p_wave->has_fact = true;
        p_wave->c_fact_samples = LoadLe32(p_chunk);
    }
    // An ATRAC9 fact chunk goes on with the input overlap delay and the encoder delay.
    if (cb_chunk >= 12) {
        p_wave->has_fact_delay = true;
        p_wave->c_fact_delay = LoadLe32(p_chunk + 8);
    }
}

// The first sample loop of a smpl chunk: start and end positions (the end inclusive) and its play
// count, 0 for endless.
void ParseSampleChunk(const u8* p_chunk, u32 cb_chunk, ParsedWaveform* p_wave) {
    if (cb_chunk < 60 || LoadLe32(p_chunk + 28) == 0) {
        return;
    }
    p_wave->has_loop = true;
    p_wave->loop_begin = LoadLe32(p_chunk + 36 + 8);
    p_wave->loop_end = LoadLe32(p_chunk + 36 + 12);
    p_wave->loop_play_count = LoadLe32(p_chunk + 36 + 20);
}

s32 SetDataRange(Reader& reader, u64 off_data, u64 cb_data, OrbisNgs2WaveformInfo* p_info) {
    if (off_data + cb_data > 0xffffffffu) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE, "Invalid waveform data ({}+{})", off_data, cb_data);
    }
    const u64 cb_source = reader.Size();
    if (cb_source != kUnknownSize && (off_data > cb_source || cb_data > cb_source - off_data)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE, "Invalid waveform data ({}+{}>{})", off_data, cb_data,
                    cb_source);
    }
    p_info->dataOffset = u32(off_data);
    p_info->dataSize = u32(cb_data);
    return ORBIS_OK;
}

// The blocks a game queues to play the waveform once, or the part before its loop, the loop
// (repeating as the smpl chunk says) and the part after it, which an exit-loop request reaches.
s32 FillBlocks(const ParsedWaveform& wave, OrbisNgs2WaveformInfo* p_info) {
    struct Part {
        u32 i_first;
        u32 c_samples;
        u32 num_repeats;
    };
    Part parts[3];
    u32 c_parts = 0;
    const u32 c_samples = p_info->numSamples;
    if (wave.has_loop && wave.loop_begin <= wave.loop_end && wave.loop_begin < c_samples) {
        const u32 loop_end = std::min(wave.loop_end, c_samples - 1);
        p_info->loopBeginPosition = wave.loop_begin;
        p_info->loopEndPosition = loop_end;
        if (wave.loop_begin > 0) {
            parts[c_parts++] = {0, wave.loop_begin, 0};
        }
        const u32 num_repeats = wave.loop_play_count == 0 ? kInfiniteRepeats : wave.loop_play_count - 1;
        parts[c_parts++] = {wave.loop_begin, loop_end + 1 - wave.loop_begin, num_repeats};
        if (loop_end + 1 < c_samples) {
            parts[c_parts++] = {loop_end + 1, c_samples - loop_end - 1, 0};
        }
    } else if (c_samples > 0) {
        parts[c_parts++] = {0, c_samples, 0};
    }
    for (u32 i_part = 0; i_part < c_parts; ++i_part) {
        OrbisNgs2WaveformBlock& block = p_info->aBlock[i_part];
        if (!CalcBlock(wave.shape, p_info->numDelaySamples, parts[i_part].i_first, parts[i_part].c_samples,
                       parts[i_part].num_repeats, &block)) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE, "Invalid waveform block ({}+{})",
                        parts[i_part].i_first, parts[i_part].c_samples);
        }
        // The frames past the data, which the delay and whole frames may reach, are left out.
        block.dataOffset = std::min(block.dataOffset, p_info->dataSize);
        block.dataSize = std::min(block.dataSize, p_info->dataSize - block.dataOffset);
        block.dataOffset += p_info->dataOffset;
    }
    p_info->numBlocks = c_parts;
    return ORBIS_OK;
}

void FillShape(const FrameShape& shape, OrbisNgs2WaveformInfo* p_info) {
    p_info->audioUnitSize = shape.cb_unit;
    p_info->numAudioUnitSamples = shape.c_unit_samples;
    p_info->numAudioUnitPerFrame = shape.c_units;
    p_info->audioFrameSize = shape.cb_frame;
    p_info->numAudioFrameSamples = shape.c_frame_samples;
}

s32 ParseRiff(Reader& reader, const u8* p_header, ParsedWaveform* p_wave) {
    const u64 cb_source = reader.Size();
    const u64 off_riff_end = std::min<u64>(u64(kChunkHeaderBytes) + LoadLe32(p_header + 4), cb_source);
    u64 off_chunk = kRiffHeaderBytes;
    for (u32 c_chunks = 0; c_chunks < kMaxRiffChunks && off_chunk + kChunkHeaderBytes <= off_riff_end; ++c_chunks) {
        u8 chunk_header[kChunkHeaderBytes];
        if (const s32 result = reader.Read(off_chunk, chunk_header, sizeof(chunk_header)); result < 0) {
            return result;
        }
        const u32 id = LoadLe32(chunk_header);
        const u32 cb_chunk = LoadLe32(chunk_header + 4);
        const u64 off_payload = off_chunk + kChunkHeaderBytes;
        if (id == FourCc("data")) {
            if (const s32 result = SetDataRange(reader, off_payload, cb_chunk, &p_wave->info); result < 0) {
                return result;
            }
            p_wave->has_data = true;
        } else if (id == FourCc("fmt ") || id == FourCc("fact") || id == FourCc("smpl")) {
            if (cb_chunk > off_riff_end - off_payload) {
                return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Invalid waveform chunk size ({:#x}: {})", id,
                            cb_chunk);
            }
            u8 payload[64]{};
            const u32 cb_read = std::min<u32>(cb_chunk, sizeof(payload));
            if (const s32 result = reader.Read(off_payload, payload, cb_read); result < 0) {
                return result;
            }
            if (id == FourCc("fmt ")) {
                if (const s32 result = ParseFormatChunk(payload, cb_chunk, p_wave); result < 0) {
                    return result;
                }
            } else if (id == FourCc("fact")) {
                ParseFactChunk(payload, cb_chunk, p_wave);
            } else {
                ParseSampleChunk(payload, cb_read, p_wave);
            }
        }
        off_chunk = off_payload + cb_chunk + (cb_chunk & 1);
    }
    if (!p_wave->has_format) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT, "Waveform has no fmt chunk");
    }
    if (!p_wave->has_data) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Waveform has no data chunk");
    }
    OrbisNgs2WaveformInfo& info = p_wave->info;
    const FrameShape& shape = p_wave->shape;
    const u32 c_frames = info.dataSize / shape.cb_frame;
    if (info.format.waveformType == WAVEFORM_ATRAC9) {
        info.numDelaySamples = p_wave->has_fact_delay ? p_wave->c_fact_delay : shape.c_delay_samples;
        const u64 c_coded = u64(c_frames) * shape.c_frame_samples;
        info.numSamples = p_wave->has_fact ? p_wave->c_fact_samples
                                           : u32(std::min<u64>(c_coded - std::min<u64>(c_coded, info.numDelaySamples),
                                                               0xffffffffu));
        // An .at9 file counts its loop in coded samples, delay included: a loop to the end of a song
        // ends at numSamples + delay - 1.
        p_wave->loop_begin -= std::min(p_wave->loop_begin, info.numDelaySamples);
        p_wave->loop_end -= std::min(p_wave->loop_end, info.numDelaySamples);
    } else {
        info.numSamples = c_frames;
    }
    return ORBIS_OK;
}

// A VAGp file (big-endian header of 0x30 bytes: size of the data at 0xc, sample rate at 0x10, a
// channel count at 0x1e that mono files leave 0) with the data straight after. Loop points come from
// the block flags of the last channel, which the sampler follows with configData bit 0.
s32 ParseVag(Reader& reader, ParsedWaveform* p_wave) {
    u8 header[kVagHeaderBytes];
    if (const s32 result = reader.Read(0, header, sizeof(header)); result < 0) {
        return result;
    }
    const u32 c_channels = header[0x1e] > 1 ? header[0x1e] : 1;
    const u32 rate_hz = LoadBe32(header + 0x10);
    if (const s32 result = CheckChannels(c_channels); result < 0) {
        return result;
    }
    if (const s32 result = CheckSampleRate(rate_hz); result < 0) {
        return result;
    }
    const u32 cb_stride = c_channels * kVagBlockBytes;
    const u32 c_frames = LoadBe32(header + 0xc) / cb_stride;
    OrbisNgs2WaveformInfo& info = p_wave->info;
    if (const s32 result = SetDataRange(reader, kVagHeaderBytes, u64(c_frames) * cb_stride, &info); result < 0) {
        return result;
    }
    GetFrameShape(WAVEFORM_VAG, c_channels, 0, 0, &p_wave->shape);
    info.numSamples = u32(std::min<u64>(u64(c_frames) * kVagFrameSamples, 0xffffffffu));
    u8 scan[kVagScanBytes];
    const u32 c_scan_frames = kVagScanBytes / cb_stride;
    bool has_loop_start = false;
    u32 i_loop_start = 0;
    for (u32 i_frame = 0; i_frame < c_frames && !p_wave->has_loop; i_frame += c_scan_frames) {
        const u32 c_read = std::min(c_scan_frames, c_frames - i_frame);
        if (const s32 result = reader.Read(info.dataOffset + u64(i_frame) * cb_stride, scan, c_read * cb_stride);
            result < 0) {
            return result;
        }
        for (u32 i_read = 0; i_read < c_read; ++i_read) {
            const u32 flags = scan[i_read * cb_stride + cb_stride - kVagBlockBytes + 1] & 0xf;
            if (flags == VAG_FLAG_LOOP_START && !has_loop_start) {
                has_loop_start = true;
                i_loop_start = i_frame + i_read;
            } else if (flags == VAG_FLAG_LOOP_END && has_loop_start) {
                p_wave->has_loop = true;
                info.loopBeginPosition = i_loop_start * kVagFrameSamples;
                info.loopEndPosition = (i_frame + i_read + 1) * kVagFrameSamples - 1;
                break;
            }
        }
    }
    info.format = {WAVEFORM_VAG, c_channels, rate_hz, p_wave->has_loop ? 1u : 0u, 0, 0};
    if (info.numSamples > 0) {
        CalcBlock(p_wave->shape, 0, 0, info.numSamples, 0, &info.aBlock[0]);
        info.aBlock[0].dataOffset = info.dataOffset;
        info.numBlocks = 1;
    }
    return ORBIS_OK;
}

s32 Parse(Reader& reader, OrbisNgs2WaveformInfo* p_out_info) {
    u8 header[kRiffHeaderBytes];
    if (const s32 result = reader.Read(0, header, sizeof(header)); result < 0) {
        return result;
    }
    ParsedWaveform wave{};
    if (LoadLe32(header) == FourCc("RIFF") && LoadLe32(header + 8) == FourCc("WAVE")) {
        if (const s32 result = ParseRiff(reader, header, &wave); result < 0) {
            return result;
        }
        if (const s32 result = FillBlocks(wave, &wave.info); result < 0) {
            return result;
        }
    } else if (LoadLe32(header) == FourCc("VAGp")) {
        if (const s32 result = ParseVag(reader, &wave); result < 0) {
            return result;
        }
    } else {
        return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Unknown waveform format ({:#x})", LoadLe32(header));
    }
    FillShape(wave.shape, &wave.info);
    const OrbisNgs2WaveformInfo& info = wave.info;
    LOG_DEBUG(Lib_Ngs2, "Parsed waveform: type {:#x}, {} channels, {} Hz, {} samples, data {}+{}, {} blocks",
              info.format.waveformType, info.format.numChannels, info.format.sampleRate, info.numSamples,
              info.dataOffset, info.dataSize, info.numBlocks);
    *p_out_info = info;
    return ORBIS_OK;
}

s32 CheckInfoAddress(const OrbisNgs2WaveformInfo* p_out_info) {
    if (!p_out_info) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid waveform info address ({})",
                    static_cast<const void*>(p_out_info));
    }
    return ORBIS_OK;
}

} // namespace

bool IsWaveformType(u32 type) {
    return (type >= WAVEFORM_FIRST_PCM && type <= WAVEFORM_VAG) || type == WAVEFORM_ATRAC9 ||
           (type >= WAVEFORM_FIRST_GENERATOR && type <= WAVEFORM_LAST_GENERATOR);
}

u32 BytesPerSample(u32 waveform_type) {
    switch (waveform_type) {
    case 0x10:
    case 0x11:
        return 1;
    case 0x12:
    case 0x13:
        return 2;
    case 0x14:
    case 0x15:
        return 3;
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
        return 4;
    default:
        return 8;
    }
}

bool ParseAtrac9Config(u32 config_data, Atrac9Config* p_out_config) {
    if ((config_data >> 24) != 0xfe) {
        return false;
    }
    constexpr u32 kChannels[8] = {1, 2, 2, 6, 8, 4, 0, 0};
    const u32 i_rate = (config_data >> 20) & 0xf;
    p_out_config->c_channels = kChannels[(config_data >> 17) & 7];
    p_out_config->cb_frame = ((config_data >> 5) & 0x7ff) + 1;
    p_out_config->c_frames_per_superframe = 1u << ((config_data >> 3) & 3);
    p_out_config->c_frame_samples = i_rate == 1 ? 64 : i_rate == 4 ? 128 : i_rate == 7 ? 256 : 0;
    return true;
}

bool GetFrameShape(u32 waveform_type, u32 c_channels, u32 config_data, u32 cb_frame_margin, FrameShape* p_out_shape) {
    if (waveform_type >= WAVEFORM_FIRST_PCM && waveform_type <= WAVEFORM_LAST_PCM) {
        const u32 cb_samples = c_channels * BytesPerSample(waveform_type);
        *p_out_shape = {cb_frame_margin + cb_samples, 1, cb_samples, 1, 1, 0};
        return true;
    }
    if (waveform_type == WAVEFORM_VAG) {
        const u32 cb_blocks = c_channels * kVagBlockBytes;
        *p_out_shape = {cb_frame_margin + cb_blocks, kVagFrameSamples, cb_blocks, kVagFrameSamples, 1, 0};
        return true;
    }
    Atrac9Config config;
    if (waveform_type != WAVEFORM_ATRAC9 || !ParseAtrac9Config(config_data, &config) || config.c_frame_samples == 0) {
        return false;
    }
    // NOTE: the delay is one frame, the decoder's overlap, as the fact chunks of the .at9 files at
    // hand say; rea has not read the firmware's own (0x10a00).
    *p_out_shape = {cb_frame_margin + config.cb_frame * config.c_frames_per_superframe,
                    config.c_frame_samples * config.c_frames_per_superframe,
                    config.cb_frame,
                    config.c_frame_samples,
                    config.c_frames_per_superframe,
                    config.c_frame_samples};
    return true;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformData(const void* p_data, std::size_t cb_data, OrbisNgs2WaveformInfo* p_out_info) {
    if (const s32 result = CheckInfoAddress(p_out_info); result < 0) {
        return result;
    }
    if (!p_data) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS, "Invalid waveform data address ({})", p_data);
    }
    DataReader reader{p_data, cb_data};
    return Parse(reader, p_out_info);
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformFile(const char* sz_path, u64 off_file, OrbisNgs2WaveformInfo* p_out_info) {
    if (const s32 result = CheckInfoAddress(p_out_info); result < 0) {
        return result;
    }
    if (!sz_path) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS, "Invalid waveform path address ({})",
                    static_cast<const void*>(sz_path));
    }
    char sz_host[kMaxPath];
    if (runtime_file_translate(sz_path, sz_host, sizeof(sz_host)) != 0) {
        return Fail(0, ORBIS_KERNEL_ERROR_ENOENT, "Waveform file not found ({})", sz_path);
    }
    std::ifstream file{sz_host, std::ios::binary | std::ios::ate};
    if (!file) {
        return Fail(0, ORBIS_KERNEL_ERROR_ENOENT, "Waveform file not found ({})", sz_path);
    }
    const std::streamoff cb_file = file.tellg();
    if (cb_file < 0) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE, "Waveform file size unknown ({})", sz_path);
    }
    const u64 cb_after = u64(cb_file) > off_file ? u64(cb_file) - off_file : 0;
    FileReader reader{file, off_file, cb_after};
    return Parse(reader, p_out_info);
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformUser(OrbisNgs2ParseReadHandler pfn_read, uintptr_t user_data,
                                          OrbisNgs2WaveformInfo* p_out_info) {
    if (const s32 result = CheckInfoAddress(p_out_info); result < 0) {
        return result;
    }
    if (!pfn_read) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS, "Invalid waveform read handler ({})",
                    reinterpret_cast<const void*>(pfn_read));
    }
    UserReader reader{pfn_read, user_data};
    return Parse(reader, p_out_info);
}

s32 PS4_SYSV_ABI sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* p_format, u32 i_sample, u32 c_samples,
                                          OrbisNgs2WaveformBlock* p_out_block) {
    FrameShape shape;
    if (const s32 result = CheckFormat(p_format, &shape); result < 0) {
        return result;
    }
    if (!p_out_block) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid waveform block address ({})",
                    static_cast<const void*>(p_out_block));
    }
    if (!CalcBlock(shape, shape.c_delay_samples, i_sample, c_samples, 0, p_out_block)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE, "Invalid waveform block ({}+{})", i_sample, c_samples);
    }
    return ORBIS_OK;
}

// The answers the game does not ask for (a null address) are left out.
s32 PS4_SYSV_ABI sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* p_format, u32* p_out_cb_frame,
                                             u32* p_out_c_frame_samples, u32* p_out_c_units_per_frame,
                                             u32* p_out_c_delay_samples) {
    FrameShape shape;
    if (const s32 result = CheckFormat(p_format, &shape); result < 0) {
        return result;
    }
    if (p_out_cb_frame) {
        *p_out_cb_frame = shape.cb_frame;
    }
    if (p_out_c_frame_samples) {
        *p_out_c_frame_samples = shape.c_frame_samples;
    }
    if (p_out_c_units_per_frame) {
        *p_out_c_units_per_frame = shape.c_units;
    }
    if (p_out_c_delay_samples) {
        *p_out_c_delay_samples = shape.c_delay_samples;
    }
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
