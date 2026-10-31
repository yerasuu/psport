// SPDX-License-Identifier: GPL-2.0-or-later
// psport: CPU decoding of BC1 to BC5 blocks for PS_CAPTURE_DUMP_TEX (vk_frame_capture_dump.h), after
// the compressed texture was copied out whole. Formats as the Khronos Data Format Specification
// describes them (S3TC and RGTC sections).

#pragma once

#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::CaptureBc {

/// Bytes per 4x4 block of a format the decoder takes; 0 for any other.
u32 BlockBytes(vk::Format format);

/// One 4x4 block into 16 texels of 4 channels, row by row; channels the format lacks read as
/// Vulkan's defaults (0, 0, 1). sRGB formats stay encoded. `format` must have a BlockBytes.
void DecodeBlock(vk::Format format, const u8* p_block, std::span<float, 64> out_rgba);

} // namespace Vulkan::CaptureBc
