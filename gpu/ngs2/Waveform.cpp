// SPDX-License-Identifier: GPL-2.0-or-later
// Waveform files and frames (phase F4, kb #1096; the firmware's own reading since kb #1119):
// sceNgs2ParseWaveformData, File and User, sceNgs2CalcWaveformBlock and sceNgs2GetWaveformFrameInfo,
// ported from firmware 11.00's 0xfa40 to 0x10bf0 as rea read them (spec 10 §1, §2). A parse takes VAGp
// and RIFF files (PCM, IEEE float, WAVE_FORMAT_EXTENSIBLE with PCM or ATRAC9) and checks no more than
// the firmware does: channels, sample rates and sizes reach the game as the file gives them.
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>

#include "Internal.h"
#include "core/libraries/kernel/orbis_error.h"

extern "C" int runtime_file_translate(const char* sz_guest, char* sz_out, std::size_t cb_out);

namespace Libraries::Ngs2 {
namespace {

constexpr u32 kMaxPath = 1024;
// NOTE: the firmware searches a chunk without a bound, so a chunk size that brings the walk back to an
// earlier offset modulo 2^32 loops forever there (spec 10 §1.4); the file is the game's untrusted input,
// so a search here gives up after this many chunks.
constexpr u32 kMaxRiffChunks = 64;
constexpr u32 kRiffHeaderBytes = 12;
constexpr u32 kChunkHeaderBytes = 8;
constexpr u32 kVagHeaderBytes = 0x30;
constexpr u32 kFormatChunkCap = 0x34;
constexpr u32 kExtensibleFormatBytes = 0x28;
constexpr u32 kFactChunkCap = 4;
constexpr u32 kSampleChunkCap = 0x3c;
constexpr u32 kInfiniteRepeats = 0xffffffffu;
// Older SDKs queue the part before the loop even when it is empty (spec 10 §1.6).
constexpr u32 kSkipEmptyIntroSdkVersion = 0x1700000;

enum FormatTag : u16 {
    FORMAT_TAG_PCM = 1,
    FORMAT_TAG_FLOAT = 3,
    FORMAT_TAG_EXTENSIBLE = 0xfffe,
};

enum VagFlag : u32 {
    VAG_FLAG_LOOP_END = 3,
    VAG_FLAG_LOOP_START = 6,
    VAG_FLAG_LOOP_BIT = 2,
};

// KSDATAFORMAT_SUBTYPE_PCM (0x54030) and ATRAC9's subformat (0x54040); the firmware has no IEEE float one.
constexpr u8 kPcmSubformat[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
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

// The ATRAC9 frame's samples for a config's sample rate index (the table at 0x328e0), whatever its
// other bits hold.
u32 Atrac9FrameSamples(u32 config_data) {
    constexpr u16 kFrameSamples[16] = {0, 64, 0, 0, 128, 0, 0, 256, 0, 0, 0, 0, 0, 0, 0, 0};
    return kFrameSamples[(config_data >> 20) & 0xf];
}

// Where a parse reads from (0x10b30): ORBIS_OK once cb bytes at the 32-bit offset off are in p_out, or
// 0x804A0408 for any read that fails.
class Reader {
public:
    virtual ~Reader() = default;
    virtual s32 Read(u32 off, void* p_out, u32 cb) = 0;
};

class DataReader final : public Reader {
public:
    DataReader(const void* p_data, std::size_t cb_data) : m_p_data{static_cast<const u8*>(p_data)}, m_cb_data{cb_data} {}
    s32 Read(u32 off, void* p_out, u32 cb) override {
        if (m_cb_data < u64(off) + cb) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Waveform read past the data ({}+{}>{})", off, cb,
                        m_cb_data);
        }
        if (!m_p_data) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS, "Invalid waveform data address ({})",
                        static_cast<const void*>(m_p_data));
        }
        if (cb > 0) {
            std::memcpy(p_out, m_p_data + off, cb);
        }
        return ORBIS_OK;
    }

private:
    const u8* m_p_data;
    std::size_t m_cb_data;
};

class FileReader final : public Reader {
public:
    FileReader(std::ifstream& file, u64 off_file) : m_file{file}, m_off_file{off_file} {}
    s32 Read(u32 off, void* p_out, u32 cb) override {
        m_file.clear();
        m_file.seekg(std::streamoff(m_off_file + off));
        if (!m_file || !m_file.read(static_cast<char*>(p_out), std::streamsize(cb))) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Waveform file read failed ({}+{})", off, cb);
        }
        return ORBIS_OK;
    }

private:
    std::ifstream& m_file;
    u64 m_off_file;
};

// The handler's answer is not handed back: below 0 the read fails, anything else fills the buffer.
class UserReader final : public Reader {
public:
    UserReader(OrbisNgs2ParseReadHandler pfn_read, uintptr_t user_data) : m_pfn_read{pfn_read}, m_user_data{user_data} {}
    s32 Read(u32 off, void* p_out, u32 cb) override {
        if (const s32 result = m_pfn_read(m_user_data, off, p_out, cb); result < 0) {
            return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Waveform read handler failed ({:#x} at {}+{})",
                        static_cast<u32>(result), off, cb);
        }
        return ORBIS_OK;
    }

private:
    OrbisNgs2ParseReadHandler m_pfn_read;
    uintptr_t m_user_data;
};

struct Chunk {
    u32 off_payload;
    u32 cb_payload;
};

// 0x10bf0: the chunk id, searched from offset 12 up to riffSize - 4 with no padding between chunks, a
// chunk of size 0 ending the search. min(size, cb_cap) bytes of it land in p_out. A failed read ends the
// search too: the callers answer their own code for a chunk not found.
bool FindChunk(Reader& reader, u32 riff_size, u32 id, u8* p_out, u32 cb_cap, Chunk* p_out_chunk) {
    u32 off = kRiffHeaderBytes;
    for (u32 c_chunks = 0; c_chunks < kMaxRiffChunks && u64(off) < u64(riff_size) - 4; ++c_chunks) {
        u8 header[kChunkHeaderBytes];
        if (reader.Read(off, header, sizeof(header)) < 0) {
            return false;
        }
        const u32 cb_chunk = LoadLe32(header + 4);
        if (LoadLe32(header) == id) {
            if (reader.Read(off + kChunkHeaderBytes, p_out, std::min(cb_chunk, cb_cap)) < 0) {
                return false;
            }
            *p_out_chunk = {off + kChunkHeaderBytes, cb_chunk};
            return true;
        }
        if (cb_chunk == 0) {
            return false;
        }
        off += kChunkHeaderBytes + cb_chunk;
    }
    return false;
}

u32 PcmType(u32 c_bits) {
    switch (c_bits) {
    case 8:
        return 0x11;
    case 16:
        return 0x12;
    case 24:
        return 0x14;
    case 32:
        return 0x16;
    default:
        return 0;
    }
}

u32 FloatType(u32 c_bits) {
    return c_bits == 32 ? 0x18 : c_bits == 64 ? 0x1a : 0;
}

// The fmt chunk's waveform type (spec 10 §1.4 step 2); nBlockAlign plays no part.
s32 ReadFormatType(const u8* p_fmt, u32 cb_fmt, u32* p_out_type) {
    const u32 tag = LoadLe16(p_fmt);
    const u32 c_bits = LoadLe16(p_fmt + 0xe);
    u32 type = 0;
    if (tag == FORMAT_TAG_EXTENSIBLE) {
        if (cb_fmt < kExtensibleFormatBytes) {
            return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Invalid extensible fmt chunk size ({})", cb_fmt);
        }
        const u8* p_subformat = p_fmt + 0x18;
        if (std::memcmp(p_subformat, kPcmSubformat, sizeof(kPcmSubformat)) == 0) {
            type = PcmType(c_bits);
        } else if (std::memcmp(p_subformat, kAtrac9Subformat, sizeof(kAtrac9Subformat)) == 0 ||
                   cb_fmt == kFormatChunkCap) {
            type = WAVEFORM_ATRAC9;
        } else {
            return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Unknown waveform subformat");
        }
    } else if (tag == FORMAT_TAG_PCM) {
        type = PcmType(c_bits);
    } else if (tag == FORMAT_TAG_FLOAT) {
        type = FloatType(c_bits);
    } else {
        return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Unknown waveform format tag ({:#x})", tag);
    }
    if (type == 0) {
        return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Invalid waveform bits ({}, tag {:#x})", c_bits, tag);
    }
    *p_out_type = type;
    return ORBIS_OK;
}

// NOTE: the chunk buffers start zeroed, so the bytes a chunk shorter than its cap leaves unread read as
// 0 where the firmware reads what its stack held.
s32 ParseRiff(Reader& reader, OrbisNgs2WaveformInfo* p_info) {
    u8 riff_header[kChunkHeaderBytes];
    const u32 riff_size = reader.Read(0, riff_header, sizeof(riff_header)) < 0 ? 0 : LoadLe32(riff_header + 4);
    u8 fmt[kFormatChunkCap]{};
    Chunk fmt_chunk;
    if (!FindChunk(reader, riff_size, FourCc("fmt "), fmt, sizeof(fmt), &fmt_chunk)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Waveform has no fmt chunk");
    }
    u32 type = 0;
    if (const s32 result = ReadFormatType(fmt, fmt_chunk.cb_payload, &type); result < 0) {
        return result;
    }
    const u32 c_channels = LoadLe16(fmt + 2);
    const u32 c_bits = LoadLe16(fmt + 0xe);
    const bool is_atrac9 = type == WAVEFORM_ATRAC9;
    const u32 config_data = is_atrac9 ? LoadBe32(fmt + 0x2c) : 0;
    p_info->format = {type, c_channels, LoadLe32(fmt + 4), config_data, 0, 0};
    u8 fact[kFactChunkCap]{};
    Chunk fact_chunk;
    const bool has_fact = FindChunk(reader, riff_size, FourCc("fact"), fact, sizeof(fact), &fact_chunk);
    if (is_atrac9 && !has_fact) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "ATRAC9 waveform has no fact chunk");
    }
    p_info->numSamples = has_fact ? LoadLe32(fact) : 0;
    u8 smpl[kSampleChunkCap]{};
    Chunk smpl_chunk;
    if (FindChunk(reader, riff_size, FourCc("smpl"), smpl, sizeof(smpl), &smpl_chunk) && LoadLe32(smpl + 0x1c) != 0) {
        // An .at9 file counts its loop with the codec's delay, which the blocks start with.
        const u32 c_delay = is_atrac9 ? Atrac9FrameSamples(config_data) : 0;
        p_info->loopBeginPosition = LoadLe32(smpl + 0x2c) - c_delay;
        p_info->loopEndPosition = LoadLe32(smpl + 0x30) + 1 - c_delay;
    }
    u8 none[1];
    Chunk data_chunk;
    if (!FindChunk(reader, riff_size, FourCc("data"), none, 0, &data_chunk)) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Waveform has no data chunk");
    }
    p_info->dataOffset = data_chunk.off_payload;
    p_info->dataSize = data_chunk.cb_payload;
    if (!is_atrac9 && p_info->numSamples == 0) {
        const u32 cb_samples = (c_channels * c_bits) >> 3;
        // NOTE: the firmware divides by zero here for 0 channels; that file is refused instead.
        if (cb_samples == 0) {
            return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Invalid waveform channels ({})", c_channels);
        }
        p_info->numSamples = p_info->dataSize / cb_samples;
    }
    return ORBIS_OK;
}

// A VAGp file (spec 10 §1.3): a big-endian header of 0x30 bytes, the data straight after it. Its loop
// comes from the flags of channel 0's blocks when frame 1 has the loop bit, and plays through the blocks
// FillBlocks lays out, not through the sampler's own flags (configData 0).
s32 ParseVag(Reader& reader, OrbisNgs2WaveformInfo* p_info) {
    u8 header[kVagHeaderBytes];
    if (const s32 result = reader.Read(0, header, sizeof(header)); result < 0) {
        return result;
    }
    if (LoadLe32(header) != FourCc("VAGp")) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Unknown waveform format ({:#x})", LoadLe32(header));
    }
    const u32 c_channels = header[0x1e] ? header[0x1e] : 1;
    const u32 cb_data = LoadBe32(header + 0xc);
    const u32 cb_stride = c_channels * kVagBlockBytes;
    const u32 c_total = cb_data / cb_stride * kVagFrameSamples;
    u8 block[kVagBlockBytes];
    if (const s32 result = reader.Read(kVagHeaderBytes + cb_stride, block, sizeof(block)); result < 0) {
        return result;
    }
    p_info->format = {WAVEFORM_VAG, c_channels, LoadBe32(header + 0x10), 0, 0, 0};
    p_info->dataOffset = kVagHeaderBytes;
    p_info->dataSize = cb_data;
    if (!(block[1] & VAG_FLAG_LOOP_BIT)) {
        p_info->numSamples = c_total - kVagFrameSamples;
        return ORBIS_OK;
    }
    p_info->numSamples = c_total;
    for (u32 i_frame = 0; u64(i_frame) * cb_stride < cb_data; ++i_frame) {
        if (const s32 result = reader.Read(kVagHeaderBytes + i_frame * cb_stride, block, sizeof(block)); result < 0) {
            return result;
        }
        const u32 flags = block[1] & 0xf;
        if (flags == VAG_FLAG_LOOP_END) {
            p_info->loopEndPosition = (i_frame + 1) * kVagFrameSamples;
        } else if (flags == VAG_FLAG_LOOP_START) {
            p_info->loopBeginPosition = i_frame * kVagFrameSamples;
        }
    }
    return ORBIS_OK;
}

// A waveform's frame as the parse and GetWaveformFrameInfo give it (0x10a00): c_units units of cb_unit
// bytes and c_unit_samples samples, frameMargin left out. False for the generators and unknown types.
struct FileFrame {
    u32 cb_unit;
    u32 c_unit_samples;
    u32 c_units;
    u32 c_delay_samples;
};

bool GetFileFrame(const OrbisNgs2WaveformFormat& format, FileFrame* p_out_frame) {
    const u32 type = format.waveformType;
    if (type >= WAVEFORM_FIRST_PCM && type <= WAVEFORM_LAST_PCM) {
        *p_out_frame = {format.numChannels * BytesPerSample(type), 1, 1, 0};
        return true;
    }
    if (type == WAVEFORM_VAG) {
        *p_out_frame = {format.numChannels * kVagBlockBytes, kVagFrameSamples, 1, 0};
        return true;
    }
    if (type == WAVEFORM_ATRAC9) {
        const u32 config_data = format.configData;
        const u32 c_frame_samples = Atrac9FrameSamples(config_data);
        *p_out_frame = {((config_data >> 5) & 0x7ff) + 1, c_frame_samples, 1u << ((config_data >> 3) & 3),
                        c_frame_samples};
        return true;
    }
    return false;
}

// 0x10890 past its address checks, in the firmware's 32-bit arithmetic. An ATRAC9 block starts at the
// superframe holding sample i_sample and skips up to it plus one frame, the decoder's delay, which the
// restart of the decoder at a skipping pass discards.
s32 CalcBlock(const OrbisNgs2WaveformFormat& format, u32 i_sample, u32 c_samples, OrbisNgs2WaveformBlock* p_out_block) {
    *p_out_block = {};
    FileFrame frame;
    if (!GetFileFrame(format, &frame)) {
        return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Invalid waveform type ({:#x})", format.waveformType);
    }
    OrbisNgs2WaveformBlock& block = *p_out_block;
    if (format.waveformType == WAVEFORM_VAG) {
        const u32 i_first = i_sample / kVagFrameSamples;
        const u32 i_end = (i_sample + c_samples + kVagFrameSamples - 1) / kVagFrameSamples;
        block.dataOffset = frame.cb_unit * i_first;
        block.dataSize = frame.cb_unit * i_end - block.dataOffset;
        block.numSamples = (i_end - i_first) * kVagFrameSamples;
    } else if (format.waveformType == WAVEFORM_ATRAC9) {
        const u32 c_frame_samples = frame.c_unit_samples;
        // NOTE: the firmware divides by zero for a sample rate index its frame table leaves out.
        if (c_frame_samples == 0) {
            return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "ATRAC9 config without frames ({:#x})",
                        format.configData);
        }
        const u32 c_superframe_samples = c_frame_samples * frame.c_units;
        const u32 cb_superframe = frame.cb_unit * frame.c_units;
        const u32 i_superframe = i_sample / c_superframe_samples;
        block.dataOffset = i_superframe * cb_superframe;
        block.dataSize = (c_superframe_samples - 1 + c_samples + c_frame_samples + i_sample) / c_superframe_samples *
                             cb_superframe -
                         block.dataOffset;
        block.numSkipSamples = i_sample + c_frame_samples - i_superframe * c_superframe_samples;
        block.numSamples = c_samples;
    } else {
        block.dataOffset = frame.cb_unit * i_sample;
        block.dataSize = frame.cb_unit * (i_sample + c_samples) - block.dataOffset;
        block.numSamples = c_samples;
    }
    return ORBIS_OK;
}

// The blocks a game queues to play the waveform (spec 10 §1.6): the part before the loop, the loop
// repeating until an exit request, and the part after it, which that request reaches.
s32 FillBlocks(OrbisNgs2WaveformInfo* p_info) {
    struct Part {
        u32 i_first;
        u32 c_samples;
        bool is_loop;
    };
    Part parts[3];
    u32 c_parts = 0;
    const u32 loop_begin = p_info->loopBeginPosition;
    const u32 loop_end = p_info->loopEndPosition;
    const u32 c_loop = loop_end - loop_begin;
    const u32 c_tail = p_info->numSamples - loop_end;
    if (loop_end < loop_begin || c_loop == 0) {
        parts[c_parts++] = {0, p_info->numSamples, false};
    } else {
        if (loop_begin != 0 || !IsCompiledSdkAtLeast(kSkipEmptyIntroSdkVersion)) {
            parts[c_parts++] = {0, loop_begin, false};
        }
        parts[c_parts++] = {loop_begin, c_loop, true};
        if (c_tail != 0) {
            parts[c_parts++] = {loop_end, c_tail, false};
        }
    }
    for (u32 i_part = 0; i_part < c_parts; ++i_part) {
        OrbisNgs2WaveformBlock& block = p_info->aBlock[i_part];
        if (const s32 result = CalcBlock(p_info->format, parts[i_part].i_first, parts[i_part].c_samples, &block);
            result < 0) {
            return result;
        }
        if (parts[i_part].is_loop) {
            block.numRepeats = kInfiniteRepeats;
        }
        block.dataOffset += p_info->dataOffset;
    }
    p_info->numBlocks = c_parts;
    return ORBIS_OK;
}

// 0xfac0. The game's info gets its fields and aBlock[0..numBlocks) only on success: the blocks past
// them keep what the game left there.
s32 Parse(Reader& reader, OrbisNgs2WaveformInfo* p_out_info) {
    if (!p_out_info) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid waveform info address ({})",
                    static_cast<const void*>(p_out_info));
    }
    u8 tag[4];
    if (const s32 result = reader.Read(0, tag, sizeof(tag)); result < 0) {
        return result;
    }
    OrbisNgs2WaveformInfo info{};
    if (LoadLe32(tag) == FourCc("VAGp")) {
        if (const s32 result = ParseVag(reader, &info); result < 0) {
            return result;
        }
    } else if (LoadLe32(tag) == FourCc("RIFF")) {
        if (const s32 result = ParseRiff(reader, &info); result < 0) {
            return result;
        }
    } else {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Unknown waveform format ({:#x})", LoadLe32(tag));
    }
    FileFrame frame{};
    GetFileFrame(info.format, &frame);
    info.audioUnitSize = frame.cb_unit;
    info.numAudioUnitSamples = frame.c_unit_samples;
    info.numAudioUnitPerFrame = frame.c_units;
    info.audioFrameSize = frame.cb_unit * frame.c_units;
    info.numAudioFrameSamples = frame.c_unit_samples * frame.c_units;
    info.numDelaySamples = frame.c_delay_samples;
    if (const s32 result = FillBlocks(&info); result < 0) {
        return result;
    }
    LOG_DEBUG(Lib_Ngs2, "Parsed waveform: type {:#x}, {} channels, {} Hz, {} samples, data {}+{}, {} blocks",
              info.format.waveformType, info.format.numChannels, info.format.sampleRate, info.numSamples,
              info.dataOffset, info.dataSize, info.numBlocks);
    std::memcpy(p_out_info, &info, offsetof(OrbisNgs2WaveformInfo, aBlock));
    std::copy_n(info.aBlock, info.numBlocks, p_out_info->aBlock);
    return ORBIS_OK;
}

// Every entry point clears the info's first 8 bytes before anything else, on failure too.
void ClearInfoType(OrbisNgs2WaveformInfo* p_out_info) {
    if (p_out_info) {
        p_out_info->format.waveformType = 0;
        p_out_info->format.numChannels = 0;
    }
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
    p_out_config->c_channels = kChannels[(config_data >> 17) & 7];
    p_out_config->cb_frame = ((config_data >> 5) & 0x7ff) + 1;
    p_out_config->c_frames_per_superframe = 1u << ((config_data >> 3) & 3);
    p_out_config->c_frame_samples = Atrac9FrameSamples(config_data);
    return true;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformData(const void* p_data, std::size_t cb_data, OrbisNgs2WaveformInfo* p_out_info) {
    ClearInfoType(p_out_info);
    DataReader reader{p_data, cb_data};
    return Parse(reader, p_out_info);
}

// Neither the path nor the info is checked before the file opens.
s32 PS4_SYSV_ABI sceNgs2ParseWaveformFile(const char* sz_path, u64 off_file, OrbisNgs2WaveformInfo* p_out_info) {
    ClearInfoType(p_out_info);
    if (!sz_path) {
        return Fail(0, ORBIS_KERNEL_ERROR_EFAULT, "Waveform file not opened (no path)");
    }
    char sz_host[kMaxPath];
    if (runtime_file_translate(sz_path, sz_host, sizeof(sz_host)) != 0) {
        return Fail(0, ORBIS_KERNEL_ERROR_EFAULT, "Waveform file not opened ({})", sz_path);
    }
    std::ifstream file{sz_host, std::ios::binary};
    if (!file) {
        return Fail(0, ORBIS_KERNEL_ERROR_EFAULT, "Waveform file not opened ({})", sz_path);
    }
    FileReader reader{file, off_file};
    return Parse(reader, p_out_info);
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformUser(OrbisNgs2ParseReadHandler pfn_read, uintptr_t user_data,
                                          OrbisNgs2WaveformInfo* p_out_info) {
    ClearInfoType(p_out_info);
    if (!pfn_read) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA, "Invalid waveform read handler ({})",
                    reinterpret_cast<const void*>(pfn_read));
    }
    UserReader reader{pfn_read, user_data};
    return Parse(reader, p_out_info);
}

s32 PS4_SYSV_ABI sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* p_format, u32 i_sample, u32 c_samples,
                                          OrbisNgs2WaveformBlock* p_out_block) {
    if (!p_out_block) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS, "Invalid waveform block address ({})",
                    static_cast<const void*>(p_out_block));
    }
    *p_out_block = {};
    if (!p_format) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT, "Invalid waveform format address ({})",
                    static_cast<const void*>(p_format));
    }
    return CalcBlock(*p_format, i_sample, c_samples, p_out_block);
}

// Every answer the game asks for (a non-null address) is cleared first, on failure too.
s32 PS4_SYSV_ABI sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* p_format, u32* p_out_cb_frame,
                                             u32* p_out_c_frame_samples, u32* p_out_c_units_per_frame,
                                             u32* p_out_c_delay_samples) {
    for (u32* p_out : {p_out_cb_frame, p_out_c_frame_samples, p_out_c_units_per_frame, p_out_c_delay_samples}) {
        if (p_out) {
            *p_out = 0;
        }
    }
    if (!p_format) {
        return Fail(0, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT, "Invalid waveform format address ({})",
                    static_cast<const void*>(p_format));
    }
    FileFrame frame;
    if (!GetFileFrame(*p_format, &frame)) {
        return Fail(0, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "Invalid waveform type ({:#x})",
                    p_format->waveformType);
    }
    if (p_out_cb_frame) {
        *p_out_cb_frame = frame.cb_unit * frame.c_units;
    }
    if (p_out_c_frame_samples) {
        *p_out_c_frame_samples = frame.c_unit_samples * frame.c_units;
    }
    if (p_out_c_units_per_frame) {
        *p_out_c_units_per_frame = frame.c_units;
    }
    if (p_out_c_delay_samples) {
        *p_out_c_delay_samples = frame.c_delay_samples;
    }
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
