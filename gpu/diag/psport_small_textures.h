// SPDX-License-Identifier: GPL-2.0-or-later
// psport: PS_DUMP_SMALL_TEXTURES=<dir> (1: <PS_DATA_DIR>/small_textures) — an inventory of the small
// textures a game uploads, to see how it stores its button glyphs (#706). Each texture uploaded
// from the game's memory whose level 0 fits PS_DUMP_SMALL_TEXTURES_MAX (256) on both sides, or a
// UI-looking atlas up to PS_DUMP_SMALL_TEXTURES_ATLAS (2048; 0: none), is hashed as the texture
// substitution sees it (XXH3 over its guest bytes) and kept once per hash, up to
// PS_DUMP_SMALL_TEXTURES_LIMIT (4096, at most 65536): a row of <dir>/index.tsv and <dir>/<hash>.png,
// its level 0 layer 0 read back through a blit to RGBA8 (none where the device cannot blit the
// format) and encoded on a thread of its own, within PS_DUMP_SMALL_TEXTURES_PENDING_MB (512) of
// readbacks in flight and PS_DUMP_SMALL_TEXTURES_DISK_MB (2048) of PNGs. Only reads: the game's
// texture is a blit source after its upload, as for a composed substitute. On, each upload of a
// candidate hashes its guest bytes on the recording thread. Off, every hook is one load of a
// constant flag.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {
struct Image;
}

namespace PsSmallTextures {

inline const bool g_is_enabled = [] {
    const char* sz_env = std::getenv("PS_DUMP_SMALL_TEXTURES");
    return sz_env && sz_env[0] && !(sz_env[0] == '0' && !sz_env[1]);
}();

/// The recording thread, right after `image`'s upload is recorded (TextureCache::RefreshImage).
void NoteUploadSlow(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime,
                    VideoCore::Image& image);
inline void NoteUpload(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime,
                       VideoCore::Image& image) {
    if (g_is_enabled) {
        NoteUploadSlow(instance, scheduler, runtime, image);
    }
}

/// Any thread that binds textures, for an image a binding samples.
void NoteBindSlow(VideoCore::Image& image);
inline void NoteBind(VideoCore::Image& image) {
    if (g_is_enabled) {
        NoteBindSlow(image);
    }
}

/// A normal exit (psport_gpu_flush_caches with waiting allowed): the index written a last time.
/// Never from a signal handler: it locks, allocates and writes a file.
void WriteIndexAtExit();

} // namespace PsSmallTextures
