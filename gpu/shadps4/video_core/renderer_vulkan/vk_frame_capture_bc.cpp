// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture_bc.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Vulkan::CaptureBc {

namespace {

u64 LoadWord(const u8* p_bytes, u32 cb_word) {
    u64 word = 0;
    std::memcpy(&word, p_bytes, cb_word);
    return word;
}

/// The colour block of BC1 to BC3. `has_punchthrough`: BC1, whose c0 <= c1 selects three colours
/// and transparent black; BC2 and BC3 always take four colours.
void DecodeColor(const u8* p_block, bool has_punchthrough, bool is_alpha_kept,
                 std::span<float, 64> out_rgba) {
    const u32 c0 = u32(LoadWord(p_block, 2));
    const u32 c1 = u32(LoadWord(p_block + 2, 2));
    const u32 indices = u32(LoadWord(p_block + 4, 4));
    const auto expand = [](u32 color) {
        return std::array<float, 4>{float((color >> 11) & 31) / 31.0f,
                                    float((color >> 5) & 63) / 63.0f, float(color & 31) / 31.0f,
                                    1.0f};
    };
    std::array<std::array<float, 4>, 4> palette{expand(c0), expand(c1)};
    const bool is_four = !has_punchthrough || c0 > c1;
    for (u32 c = 0; c < 3; ++c) {
        if (is_four) {
            palette[2][c] = (2.0f * palette[0][c] + palette[1][c]) / 3.0f;
            palette[3][c] = (palette[0][c] + 2.0f * palette[1][c]) / 3.0f;
        } else {
            palette[2][c] = (palette[0][c] + palette[1][c]) / 2.0f;
            palette[3][c] = 0.0f;
        }
    }
    palette[2][3] = 1.0f;
    palette[3][3] = is_four || !is_alpha_kept ? 1.0f : 0.0f;
    for (u32 i = 0; i < 16; ++i) {
        const auto& color = palette[(indices >> (2 * i)) & 3];
        std::copy(color.begin(), color.end(), out_rgba.begin() + i * 4);
    }
}

/// The 8-byte block of BC3's alpha and of each BC4/BC5 channel, into channel `i_channel`.
void DecodeChannel(const u8* p_block, bool is_signed, u32 i_channel,
                   std::span<float, 64> out_rgba) {
    std::array<float, 8> palette{};
    if (is_signed) {
        palette[0] = std::max(float(s8(p_block[0])), -127.0f) / 127.0f;
        palette[1] = std::max(float(s8(p_block[1])), -127.0f) / 127.0f;
    } else {
        palette[0] = float(p_block[0]) / 255.0f;
        palette[1] = float(p_block[1]) / 255.0f;
    }
    const bool is_eight = is_signed ? s8(p_block[0]) > s8(p_block[1]) : p_block[0] > p_block[1];
    if (is_eight) {
        for (u32 i = 1; i < 7; ++i) {
            palette[i + 1] = (float(7 - i) * palette[0] + float(i) * palette[1]) / 7.0f;
        }
    } else {
        for (u32 i = 1; i < 5; ++i) {
            palette[i + 1] = (float(5 - i) * palette[0] + float(i) * palette[1]) / 5.0f;
        }
        palette[6] = is_signed ? -1.0f : 0.0f;
        palette[7] = 1.0f;
    }
    const u64 indices = LoadWord(p_block + 2, 6);
    for (u32 i = 0; i < 16; ++i) {
        out_rgba[i * 4 + i_channel] = palette[(indices >> (3 * i)) & 7];
    }
}

} // namespace

u32 BlockBytes(vk::Format format) {
    using F = vk::Format;
    switch (format) {
    case F::eBc1RgbUnormBlock:
    case F::eBc1RgbSrgbBlock:
    case F::eBc1RgbaUnormBlock:
    case F::eBc1RgbaSrgbBlock:
    case F::eBc4UnormBlock:
    case F::eBc4SnormBlock:
        return 8;
    case F::eBc2UnormBlock:
    case F::eBc2SrgbBlock:
    case F::eBc3UnormBlock:
    case F::eBc3SrgbBlock:
    case F::eBc5UnormBlock:
    case F::eBc5SnormBlock:
        return 16;
    default:
        return 0;
    }
}

void DecodeBlock(vk::Format format, const u8* p_block, std::span<float, 64> out_rgba) {
    using F = vk::Format;
    for (u32 i = 0; i < 16; ++i) {
        out_rgba[i * 4 + 0] = 0.0f;
        out_rgba[i * 4 + 1] = 0.0f;
        out_rgba[i * 4 + 2] = 0.0f;
        out_rgba[i * 4 + 3] = 1.0f;
    }
    switch (format) {
    case F::eBc1RgbUnormBlock:
    case F::eBc1RgbSrgbBlock:
        DecodeColor(p_block, true, false, out_rgba);
        break;
    case F::eBc1RgbaUnormBlock:
    case F::eBc1RgbaSrgbBlock:
        DecodeColor(p_block, true, true, out_rgba);
        break;
    case F::eBc2UnormBlock:
    case F::eBc2SrgbBlock: {
        DecodeColor(p_block + 8, false, false, out_rgba);
        const u64 alphas = LoadWord(p_block, 8);
        for (u32 i = 0; i < 16; ++i) {
            out_rgba[i * 4 + 3] = float((alphas >> (4 * i)) & 15) / 15.0f;
        }
        break;
    }
    case F::eBc3UnormBlock:
    case F::eBc3SrgbBlock:
        DecodeColor(p_block + 8, false, false, out_rgba);
        DecodeChannel(p_block, false, 3, out_rgba);
        break;
    case F::eBc4UnormBlock:
    case F::eBc4SnormBlock:
        DecodeChannel(p_block, format == F::eBc4SnormBlock, 0, out_rgba);
        break;
    case F::eBc5UnormBlock:
    case F::eBc5SnormBlock:
        DecodeChannel(p_block, format == F::eBc5SnormBlock, 0, out_rgba);
        DecodeChannel(p_block + 8, format == F::eBc5SnormBlock, 1, out_rgba);
        break;
    default:
        break;
    }
}

} // namespace Vulkan::CaptureBc
