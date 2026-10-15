// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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

inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool is_native_coordinates) {
    return {float(output_width) / float(is_native_coordinates ? 1920 : guest_width),
            float(output_height) / float(is_native_coordinates ? 1080 : guest_height)};
}

} // namespace Vulkan::UiComposition
