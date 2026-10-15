// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne's supported game files: the game with the 1.09 update merged in. The retail releases
// (the US CUSA00900, the European Game of the Year edition CUSA03173, the Asian The Old Hunters
// Edition CUSA03023, ...) share the same 1.09 executable. Other versions start and then fail inside
// the game's code (the base game 1.00 faults at guest offset 0x20348b8): the profile's hooks and the
// presets' patches use the addresses of this one executable. The shaders must also unpack: an
// extraction tool that leaves parts of compressed sectors stale (LibOrbisPkg PkgTool on .NET 6 or
// newer, issue #81) makes the game hang while it loads them, with no error.
#include "launch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <miniz.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace PsLaunch {
namespace {
constexpr const char* SupportedVersion = "01.09";
constexpr const char* SupportedImage = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
/// The 1.09 executable with Lance McDonald's 60 fps patch applied to it, as some dumps ship it
/// (seen on US CUSA00900): one of its edits inverts a branch in the menu code (+0xf6d90d) and the
/// gestures menu crashes the game. psport has its own 60 fps; the clean 1.09 eboot.bin is needed.
constexpr const char* Lance60FpsImage = "bbc91f4dff6bc3118b039464289e39878a8fdfc802a07fab9d5fe84d935a676f";
/// The game loads these first; a broken extraction damages most files, these among them.
constexpr const char* CheckedFolder = "dvdroot_ps4/shader";

/// A DFLT .dcx file that does not unpack to its stated size or fails zlib's checksum. Other
/// compressions are not checked.
bool IsBrokenDcx(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<unsigned char> data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (data.size() < 0x2c || std::memcmp(data.data(), "DCX\0", 4) || std::memcmp(data.data() + 0x18, "DCS\0", 4) ||
        std::memcmp(data.data() + 0x28, "DFLT", 4)) {
        return false;
    }
    const auto big_endian = [&](std::size_t off_read) {
        return std::size_t(data[off_read]) << 24 | std::size_t(data[off_read + 1]) << 16 | std::size_t(data[off_read + 2]) << 8 |
               data[off_read + 3];
    };
    const std::size_t cb_unpacked = big_endian(0x1c), cb_packed = big_endian(0x20);
    const std::size_t off_stream = std::min<std::size_t>(0x4c, data.size());
    const std::size_t cb_stream = std::min(cb_packed, data.size() - off_stream);
    std::size_t cb_out = 0;
    void* p_out = tinfl_decompress_mem_to_heap(data.data() + off_stream, cb_stream, &cb_out, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (!p_out) {
        return true;
    }
    mz_free(p_out);
    return cb_out != cb_unpacked;
}

bool HasBrokenDcx(const std::string& folder) {
    DIR* p_dir = opendir(folder.c_str());
    if (!p_dir) {
        return false;
    }
    std::vector<std::string> entries;
    while (const dirent* p_entry = readdir(p_dir)) {
        if (std::strcmp(p_entry->d_name, ".") && std::strcmp(p_entry->d_name, "..")) {
            entries.emplace_back(p_entry->d_name);
        }
    }
    closedir(p_dir);
    for (const std::string& name : entries) {
        const std::string path = folder + "/" + name;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode) ? HasBrokenDcx(path) : name.ends_with(".dcx") && IsBrokenDcx(path)) {
            return true;
        }
    }
    return false;
}
} // namespace

bool CheckFiles(const PsGameFiles& game, std::string& kind, std::string& advice) {
    if (const char* sz_skip = std::getenv("PS_SKIP_GAME_CHECK"); sz_skip && !std::strcmp(sz_skip, "1")) {
        return true;
    }
    unsigned char digest[32];
    game.pfn_eboot_sha256(game.sz_game_dir, digest);
    char sz_image[65];
    for (int i = 0; i < 32; ++i) {
        std::snprintf(sz_image + 2 * i, 3, "%02x", digest[i]);
    }
    const std::string found = std::string("Found ") + (game.sz_serial[0] ? game.sz_serial : "?") + " version " +
                              (game.sz_version[0] ? game.sz_version : "?") + " (sce_sys/param.sfo).";
    if (!std::strcmp(sz_image, SupportedImage)) {
        if (!HasBrokenDcx(std::string(game.sz_game_dir) + "/" + CheckedFolder)) {
            return true;
        }
        kind = "damaged_files";
        advice = found + " The game files are damaged: the shaders in " + CheckedFolder +
                 " do not unpack, and the game would hang while loading them. The extraction tool left parts of "
                 "compressed sectors stale (LibOrbisPkg PkgTool on .NET 6 or newer, issue #81): extract the game "
                 "and the 1.09 update again with a fixed tool. tools/dcx_check.py GAME_DIR checks every file.";
    } else if (!std::strcmp(sz_image, Lance60FpsImage)) {
        kind = "patched_eboot";
        advice = found + " This eboot.bin has the 60 fps patch by Lance McDonald applied to it, which crashes the "
                         "game when the gestures menu opens (psport has its own 60 fps): copy eboot.bin from a clean "
                         "dump of the 1.09 update into the game folder, replacing this one.";
    } else if (std::strcmp(game.sz_version, SupportedVersion)) {
        kind = "missing_update";
        advice = found + " psport needs the 1.09 update merged into the game folder: copy everything from the "
                         "dumped 1.09 update (e.g. CUSA03173-patch) into the game folder, replacing files (eboot.bin "
                         "and sce_sys too).";
    } else {
        kind = "wrong_eboot";
        advice = found + " param.sfo is from 1.09 but eboot.bin is not: copy eboot.bin from the dumped 1.09 update "
                         "into the game folder, replacing the old one.";
    }
    return false;
}
} // namespace PsLaunch
