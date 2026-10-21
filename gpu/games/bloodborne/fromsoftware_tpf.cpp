// SPDX-License-Identifier: GPL-2.0-or-later
// The textures of a FromSoftware TPF packed in a DCX (PS4 releases, zlib "DFLT"), for glyph tables
// (games/common/glyphs.h): extractor "fromsoftware.dcx_tpf". From bbport's bbport_button_prompts
// (72e5c8c).
#include <cstring>
#include <string>
#include <vector>

#include <miniz.h>

#include "../common/glyphs.h"

namespace PsGlyphs {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

u32 Be32(const u8* p_bytes) {
    return u32(p_bytes[0]) << 24 | u32(p_bytes[1]) << 16 | u32(p_bytes[2]) << 8 | p_bytes[3];
}
u32 Le32(const u8* p_bytes) {
    return u32(p_bytes[0]) | u32(p_bytes[1]) << 8 | u32(p_bytes[2]) << 16 | u32(p_bytes[3]) << 24;
}

/// The TPF inside a DCX (zlib "DFLT"): "DCX\0", "DCS\0" at 0x18 with the sizes (big endian),
/// "DCP\0" "DFLT", "DCA\0" and its size at 0x44, then the zlib stream.
bool Unpack(std::span<const u8> dcx, std::vector<u8>& out) {
    if (dcx.size() < 0x50 || std::memcmp(dcx.data(), "DCX\0", 4) != 0 ||
        std::memcmp(dcx.data() + 0x18, "DCS\0", 4) != 0 || std::memcmp(dcx.data() + 0x28, "DFLT", 4) != 0 ||
        std::memcmp(dcx.data() + 0x44, "DCA\0", 4) != 0) {
        return false;
    }
    const u32 cb_unpacked = Be32(dcx.data() + 0x1c);
    const std::size_t off_stream = 0x44 + std::size_t(Be32(dcx.data() + 0x48));
    if (cb_unpacked == 0 || cb_unpacked > (256u << 20) || off_stream >= dcx.size()) {
        return false;
    }
    out.resize(cb_unpacked);
    mz_ulong cb_out = cb_unpacked;
    return mz_uncompress(out.data(), &cb_out, dcx.data() + off_stream, mz_ulong(dcx.size() - off_stream)) == MZ_OK &&
           cb_out == cb_unpacked;
}

std::string EntryName(const std::vector<u8>& tpf, std::size_t off_name, bool is_utf16) {
    std::string name;
    const std::size_t cb_char = is_utf16 ? 2 : 1;
    for (std::size_t off = off_name; off + cb_char <= tpf.size() && name.size() <= 64; off += cb_char) {
        const u32 code = is_utf16 ? u32(tpf[off]) | u32(tpf[off + 1]) << 8 : tpf[off];
        if (code == 0) {
            break;
        }
        name.push_back(code < 0x80 ? char(code) : '?');
    }
    return name;
}

bool Extract(std::span<const u8> file, const EntryVisitor& visit, std::string& error) {
    std::vector<u8> tpf;
    if (!Unpack(file, tpf)) {
        error = "is not a DCX (DFLT) file that unpacks";
        return false;
    }
    // TPF (PS4): "TPF\0", count at 8, name encoding at 0xe (1 or 2: UTF-16), then 36-byte entries
    // from 0x10: data offset, size, format, type, levels, flags, width, height, two words, the
    // name's offset, a word, the DXGI format.
    if (tpf.size() < 0x10 || std::memcmp(tpf.data(), "TPF\0", 4) != 0) {
        error = "holds no TPF";
        return false;
    }
    const u32 c_entries = Le32(tpf.data() + 8);
    const bool is_utf16 = tpf[0xe] == 1 || tpf[0xe] == 2;
    constexpr std::size_t CbEntry = 36;
    for (u32 i = 0; i < c_entries && 0x10 + (std::size_t(i) + 1) * CbEntry <= tpf.size(); ++i) {
        const u8* p_entry = tpf.data() + 0x10 + std::size_t(i) * CbEntry;
        const u64 off_data = Le32(p_entry), cb_data = Le32(p_entry + 4), off_name = Le32(p_entry + 24);
        if (off_name >= tpf.size() || off_data + cb_data > tpf.size()) {
            continue;
        }
        visit(EntryName(tpf, off_name, is_utf16), std::span<const u8>(tpf.data() + off_data, cb_data));
    }
    return true;
}
} // namespace

extern const Extractor FromSoftwareDcxTpf = {.sz_id = "fromsoftware.dcx_tpf", .pfn_extract = Extract};

} // namespace PsGlyphs
