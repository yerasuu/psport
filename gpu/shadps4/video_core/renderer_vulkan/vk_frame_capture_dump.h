// SPDX-License-Identifier: GPL-2.0-or-later
// psport: PS_CAPTURE_DUMP, the image dump of frame captures (vk_frame_capture.h). At the end of
// every pass of a captured frame its attachments are blitted on the GPU into a small image
// (at most 480x270, aspect kept), copied to a host buffer and written as PNG files next to the
// frame file, with a sidecar index; a format the blit cannot take is read from the summaries'
// 64x64 grid of texels instead. PS_CAPTURE_DUMP_TEX=<one PS_SKIP_DRAWS entry> adds, at the end of
// the pass of each draw it selects, the images that draw samples; BC1 to BC5 are copied whole and
// decoded on the CPU (vk_frame_capture_bc.h). Channels are shown as the shader wrote (targets,
// through the inverse of the colour buffer's export swizzle) or reads them (textures, through the
// view's mapping), not in stored order.

#pragma once

#include <span>
#include <string>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
struct Image;
}

namespace Vulkan {
class Runtime;
}

namespace Vulkan::CaptureDump {

/// PS_CAPTURE_DUMP: 0 off; 1 colour attachment 0 of every pass (its depth attachment when it
/// has no colour one); 2 every colour attachment and the depth attachment.
u32 Level();

/// The attachment or sampled texture an image comes from, as the sidecar index lists it.
struct Source {
    u32 i_pass{};
    u32 i_color{};
    bool is_depth{};
    VAddr va_target{};
    vk::Format format{};
    u32 width{};
    u32 height{};
    u32 i_level{};
    u32 i_layer{};
    /// Where each shown channel comes from among the stored ones.
    vk::ComponentMapping mapping{};
    bool is_texture{};
    u32 i_draw{};
    u32 i_binding{};
    std::string detail; ///< a texture's view, binding and identity
};

/// Frame start: the image list, counts and staging buffer offset start over.
void StartFrame(Runtime& runtime);

/// False once the frame holds its budget of images; the target is then listed as not dumped.
bool HasRoom(const Source& source);

/// A line of the sidecar index and the log ahead of the images (a selected draw, its samplers).
void Note(std::string line);

/// A target that cannot be dumped (copy obstacle, image gone), listed with its reason.
void Skip(const Source& source, std::string reason);

enum class BlitResult : u32 { Recorded, Fallback, Skipped };

/// Records the blit and the copy of the small image into the staging buffer, in stream order;
/// BC1 to BC5 are copied whole instead, for the CPU to decode. Fallback: the format cannot be
/// blitted into the dump's image; `out_reason` says why and the caller reads the target through
/// the summaries' grid. `is_wait_allowed`: a full staging buffer may be drained here (the GPU
/// waited for); otherwise the image is skipped.
BlitResult RecordBlit(Runtime& runtime, VideoCore::Image& image, const Source& source,
                      bool is_wait_allowed, std::string& out_reason);

/// The grid fallback: a slot for the image, filled once the frame's readbacks are on the host.
/// Returns the slot's index for FillGrid / FailGrid.
u32 BeginGrid(const Source& source, std::string reason);
/// `rgba`: c_columns * c_rows texels of 4 decoded channels each, row by row.
void FillGrid(u32 i_item, std::span<const float> rgba, u32 c_columns, u32 c_rows);
void FailGrid(u32 i_item, std::string reason);

/// Frame end, where the GPU may be waited for: drains the staging buffer and writes
/// `<stem>_p<pass>_c<k>.png` (`_d` for depth), `<stem>_tex_p<pass>_d<draw>_b<binding>.png` (and
/// `_a.png`, its alpha, when not constant) and `<stem>_dump.txt`. Returns the header line of the
/// frame file.
std::string WriteFrame(Runtime& runtime, const std::string& stem);

} // namespace Vulkan::CaptureDump
