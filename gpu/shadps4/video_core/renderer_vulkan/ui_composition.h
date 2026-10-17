// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Vulkan::UiComposition {

enum class Background { None, Copy, Temporal };

// UI composition must not require scene depth/camera or an active FSR context.
constexpr Background Choose(bool is_scaled, bool is_ui_draw, bool is_scene_ready, bool is_fsr_active) {
    if (!is_scaled || !is_ui_draw) {
        return Background::None;
    }
    return is_scene_ready && is_fsr_active ? Background::Temporal : Background::Copy;
}

inline bool NativeViewport(float width, float height) {
    return std::abs(std::abs(width) - 1920.0f) < 0.5f &&
           std::abs(std::abs(height) - 1080.0f) < 0.5f;
}

// Where a pass drawn into the output-size image goes: it fills the output. On an output of another
// aspect ratio than 16:9, a game whose UI follows it (Game::Profile::pfn_display_aspect) draws its
// UI for it already: its movies show their wider (taller) view squeezed into the 1920x1080 frame,
// which the output stretches back.
inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool is_native_coordinates) {
    return {float(output_width) / float(is_native_coordinates ? 1920 : guest_width),
            float(output_height) / float(is_native_coordinates ? 1080 : guest_height)};
}

// psport: where the game's 16:9 screen ends in such a frame (normalized, at most 1): x on a wider
// output, y on a taller one.
inline std::array<float, 2> ScreenEdges(uint32_t output_width, uint32_t output_height) {
    const float k = float(output_width) * 1080.0f / (float(output_height) * 1920.0f);
    return {std::min(1.0f, 1.0f / k), std::min(1.0f, k)};
}

// psport: the UI's full-screen fills and backgrounds (the menus' dimming, fades) reach the output's
// edges: their vertices in the corners of the game's screen, or past them, are moved to the
// output's corners by the vertex shader (push constant motion_param: the screen's edges, x in the
// high 16 bits, y in bits 1-15, bit 0 set when clip space +y is the bottom of the screen, the
// draw's viewport y scale positive; EmitUiEdgeStretch). 0: none.
inline uint32_t EdgeParam(float edge_x, float edge_y, bool is_y_down) {
    const auto quantize = [](float f, float max) { return uint32_t(std::lround(std::clamp(f, 0.0f, 1.0f) * max)); };
    return std::max(2u, quantize(edge_x, 65535.0f) << 16 | quantize(edge_y, 32767.0f) << 1) | (is_y_down ? 1u : 0u);
}

} // namespace Vulkan::UiComposition
