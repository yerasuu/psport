// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne: drawing the port's key guide icons (button_prompt_art.h). Shapes by signed distance
// with one texel of smoothing, in premultiplied float RGBA; text through stb_truetype (Dear
// ImGui's copy) with the embedded DejaVu Sans, which has Latin, Cyrillic and the arrows.
#include "button_prompt_art.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

extern "C" {
// src/input/runtime_pad.c
int runtime_pad_prompt(const char* sz_input, int should_prefer_mouse, char* sz_out_label, std::size_t cb_label);
int runtime_pad_prompt_device(void);
int runtime_pad_mouse_look(void);
// port/psport_overlay.cpp
extern const unsigned char psport_font_ttf[];
}

namespace PsButtonPromptArt {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;

constexpr int PromptKeyboard = 2; // RUNTIME_PAD_PROMPT_KEYBOARD

struct Color {
    float r, g, b, a = 1.0f;
};
constexpr Color Fill{0.075f, 0.07f, 0.065f, 0.93f};
constexpr Color Frame{0.78f, 0.74f, 0.64f};
constexpr Color Ink{0.95f, 0.92f, 0.85f};
constexpr Color Accent{0.82f, 0.64f, 0.36f};

struct Canvas {
    static constexpr int N = int(Side);
    std::vector<float> pixels = std::vector<float>(std::size_t(N) * N * 4, 0.0f);

    /// Paints `color` where `coverage(x, y)` (texel centres) is above 0.
    template <typename F>
    void Paint(Color color, F&& coverage) {
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                float k = coverage(float(x) + 0.5f, float(y) + 0.5f);
                if (k <= 0.0f) {
                    continue;
                }
                k = std::min(k, 1.0f) * color.a;
                float* p_texel = &pixels[(std::size_t(y) * N + x) * 4];
                p_texel[0] = color.r * k + p_texel[0] * (1.0f - k);
                p_texel[1] = color.g * k + p_texel[1] * (1.0f - k);
                p_texel[2] = color.b * k + p_texel[2] * (1.0f - k);
                p_texel[3] = k + p_texel[3] * (1.0f - k);
            }
        }
    }

    /// Straight-alpha RGBA8, as the game's own (TGA-made) icons.
    void Store(u32* p_out) const {
        for (std::size_t i = 0; i < std::size_t(N) * N; ++i) {
            const float alpha = pixels[i * 4 + 3];
            const auto unit = [&](float value) {
                const float straight = alpha > 0.0f ? std::clamp(value / alpha, 0.0f, 1.0f) : 0.0f;
                return u32(std::lround(straight * 255.0f));
            };
            p_out[i] = unit(pixels[i * 4]) | unit(pixels[i * 4 + 1]) << 8 | unit(pixels[i * 4 + 2]) << 16 |
                       u32(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)) << 24;
        }
    }
};

float Cover(float distance) {
    return std::clamp(0.5f - distance, 0.0f, 1.0f);
}

float RoundRect(float x, float y, float cx, float cy, float hw, float hh, float radius) {
    const float qx = std::abs(x - cx) - (hw - radius), qy = std::abs(y - cy) - (hh - radius);
    const float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
    return outside + std::min(std::max(qx, qy), 0.0f) - radius;
}

float Circle(float x, float y, float cx, float cy, float radius) {
    return std::hypot(x - cx, y - cy) - radius;
}

float Segment(float x, float y, float ax, float ay, float bx, float by, float half_width) {
    const float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    const float t = std::clamp((px * dx + py * dy) / std::max(dx * dx + dy * dy, 1e-6f), 0.0f, 1.0f);
    return std::hypot(px - dx * t, py - dy * t) - half_width;
}

/// A filled shape with a frame of `stroke` texels on its edge.
template <typename D>
void Plate(Canvas& canvas, float stroke, D&& distance) {
    canvas.Paint(Fill, [&](float x, float y) { return Cover(distance(x, y)); });
    canvas.Paint(Frame,
                 [&](float x, float y) { return Cover(std::abs(distance(x, y) + stroke * 0.5f) - stroke * 0.5f); });
}

/// The embedded font, or null when stb_truetype cannot read it (then icons go without text).
const stbtt_fontinfo* Font() {
    static const stbtt_fontinfo* const s_p_font = [] {
        static stbtt_fontinfo s_font;
        return stbtt_InitFont(&s_font, psport_font_ttf, stbtt_GetFontOffsetForIndex(psport_font_ttf, 0)) != 0
                   ? &s_font
                   : nullptr;
    }();
    return s_p_font;
}

std::u32string Decode(std::string_view utf8) {
    std::u32string out;
    for (std::size_t i = 0; i < utf8.size();) {
        u32 code = u8(utf8[i]);
        int c_extra = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : code >= 0xc0 ? 1 : 0;
        code &= c_extra == 3 ? 0x07 : c_extra == 2 ? 0x0f : c_extra == 1 ? 0x1f : 0x7f;
        ++i;
        for (; c_extra > 0 && i < utf8.size(); --c_extra, ++i) {
            code = code << 6 | (u8(utf8[i]) & 0x3f);
        }
        out.push_back(char32_t(code));
    }
    return out;
}

/// `text` centred on (cx, cy), at most max_w wide and max_h tall: its ink, not the font's line
/// height, because key caps hold capitals and digits.
void DrawText(Canvas& canvas, std::string_view text, float cx, float cy, float max_w, float max_h, Color ink) {
    const stbtt_fontinfo* p_font = Font();
    if (!p_font || text.empty()) {
        return;
    }
    const std::u32string glyphs = Decode(text);
    const auto measure = [&](float scale, int& x0, int& y0, int& x1, int& y1) {
        float pen = 0.0f;
        x0 = y0 = 1 << 20;
        x1 = y1 = -(1 << 20);
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            int advance = 0, bearing = 0, gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
            stbtt_GetCodepointHMetrics(p_font, int(glyphs[i]), &advance, &bearing);
            stbtt_GetCodepointBitmapBoxSubpixel(p_font, int(glyphs[i]), scale, scale, pen - std::floor(pen), 0.0f,
                                                &gx0, &gy0, &gx1, &gy1);
            const int ox = int(std::floor(pen));
            if (gx1 > gx0) {
                x0 = std::min(x0, ox + gx0);
                x1 = std::max(x1, ox + gx1);
                y0 = std::min(y0, gy0);
                y1 = std::max(y1, gy1);
            }
            pen += float(advance) * scale;
            if (i + 1 < glyphs.size()) {
                pen += float(stbtt_GetCodepointKernAdvance(p_font, int(glyphs[i]), int(glyphs[i + 1]))) * scale;
            }
        }
        return x1 > x0;
    };
    // The ink box at a reference size, then the scale that fits it.
    float scale = stbtt_ScaleForPixelHeight(p_font, 100.0f);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!measure(scale, x0, y0, x1, y1)) {
        return;
    }
    scale *= std::min(max_w / float(x1 - x0), max_h / float(y1 - y0));
    measure(scale, x0, y0, x1, y1);
    const float left = std::round(cx - float(x1 - x0) * 0.5f) - float(x0);
    const float base = std::round(cy - float(y1 - y0) * 0.5f) - float(y0);
    float pen = 0.0f;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(p_font, int(glyphs[i]), &advance, &bearing);
        const float shift = pen - std::floor(pen);
        int width = 0, height = 0, gx = 0, gy = 0;
        unsigned char* p_bitmap = stbtt_GetCodepointBitmapSubpixel(p_font, scale, scale, shift, 0.0f, int(glyphs[i]),
                                                                   &width, &height, &gx, &gy);
        if (p_bitmap) {
            const int ox = int(left + std::floor(pen)) + gx, oy = int(base) + gy;
            canvas.Paint(ink, [&](float x, float y) {
                const int bx = int(x) - ox, by = int(y) - oy;
                return bx >= 0 && by >= 0 && bx < width && by < height ? float(p_bitmap[by * width + bx]) / 255.0f
                                                                        : 0.0f;
            });
            stbtt_FreeBitmap(p_bitmap, nullptr);
        }
        pen += float(advance) * scale;
        if (i + 1 < glyphs.size()) {
            pen += float(stbtt_GetCodepointKernAdvance(p_font, int(glyphs[i]), int(glyphs[i + 1]))) * scale;
        }
    }
}

/// A key cap with its label; one character fills the cap, longer names get smaller.
void Key(Canvas& canvas, std::string_view label, float cx, float cy, float hw, float hh) {
    const float stroke = std::max(3.0f, hw * 0.09f);
    const float radius = std::min(hw, hh) * 0.32f;
    Plate(canvas, stroke, [&](float x, float y) { return RoundRect(x, y, cx, cy, hw, hh, radius); });
    const std::size_t c_glyphs = Decode(label).size();
    const float max_h = (hh - stroke) * (c_glyphs <= 1 ? 1.15f : c_glyphs <= 3 ? 0.9f : 0.7f);
    DrawText(canvas, label, cx, cy, (hw - stroke) * 1.55f, max_h, Ink);
}

/// A mouse with the bound button lit: Left, Right, Middle, X1, X2, Wheel Up, Wheel Down; Move: none
/// lit, arrows at its sides (the camera).
void Mouse(Canvas& canvas, std::string_view button) {
    constexpr float cx = 64, cy = 66, hw = 33, hh = 50, radius = 31, stroke = 5;
    const auto body = [&](float x, float y) { return RoundRect(x, y, cx, cy, hw, hh, radius); };
    canvas.Paint(Fill, [&](float x, float y) { return Cover(body(x, y)); });
    constexpr float split = 58; // the buttons' lower edge
    const auto lit = [&](auto&& region) {
        canvas.Paint(Accent, [&](float x, float y) { return std::min(Cover(body(x, y) + stroke), region(x, y)); });
    };
    if (button == "Left") {
        lit([&](float x, float y) { return std::min(Cover(x - cx), Cover(y - split)); });
    } else if (button == "Right") {
        lit([&](float x, float y) { return std::min(Cover(cx - x), Cover(y - split)); });
    }
    canvas.Paint(Frame, [&](float x, float y) { return Cover(Segment(x, y, cx, cy - hh + 2, cx, split, 2.0f)); });
    canvas.Paint(Frame, [&](float x, float y) { return Cover(Segment(x, y, cx - hw, split, cx + hw, split, 2.0f)); });
    const bool is_wheel = button == "Middle" || button == "Wheel Up" || button == "Wheel Down";
    canvas.Paint(Fill, [&](float x, float y) { return Cover(Segment(x, y, cx, 28, cx, 46, 7.5f)); });
    canvas.Paint(is_wheel ? Accent : Frame, [&](float x, float y) {
        const float distance = Segment(x, y, cx, 28, cx, 46, 5.5f);
        return is_wheel ? Cover(distance) : Cover(std::abs(distance + 1.25f) - 1.25f);
    });
    if (button == "Wheel Up" || button == "Wheel Down") {
        DrawText(canvas, button == "Wheel Up" ? "\xe2\x96\xb2" : "\xe2\x96\xbc", cx, 90, 26, 20, Accent);
    }
    if (button == "X1" || button == "X2") {
        const float side_y = button == "X1" ? 92 : 74; // X1 back (lower), X2 forward
        canvas.Paint(Accent, [&](float x, float y) { return Cover(RoundRect(x, y, cx - hw + 1, side_y, 6, 8, 4)); });
    }
    canvas.Paint(Frame, [&](float x, float y) { return Cover(std::abs(body(x, y) + stroke * 0.5f) - stroke * 0.5f); });
    if (button == "Move") {
        DrawText(canvas, "\xe2\x86\x90", 14, cy, 24, 22, Ink);
        DrawText(canvas, "\xe2\x86\x92", 114, cy, 24, 22, Ink);
    }
}

/// A round button of another gamepad: A B X Y in their colours, else its name (LS, RS) or sign
/// (Menu, View).
void Round(Canvas& canvas, std::string_view label, bool is_face) {
    constexpr float cx = 64, cy = 64, radius = 56, stroke = 5;
    Plate(canvas, stroke, [&](float x, float y) { return Circle(x, y, cx, cy, radius); });
    if (is_face) {
        const Color ink = label == "A"   ? Color{0.40f, 0.76f, 0.30f}
                          : label == "B" ? Color{0.88f, 0.27f, 0.23f}
                          : label == "X" ? Color{0.27f, 0.55f, 0.92f}
                          : label == "Y" ? Color{0.95f, 0.78f, 0.18f}
                                         : Ink;
        DrawText(canvas, label, cx, cy, 70, 58, ink);
        return;
    }
    if (label == "Menu") {
        for (const float line_y : {48.0f, 64.0f, 80.0f}) {
            canvas.Paint(Ink, [&](float x, float y) { return Cover(Segment(x, y, 44, line_y, 84, line_y, 4.0f)); });
        }
        return;
    }
    if (label == "View") {
        const auto box = [](float bx, float by) {
            return [=](float x, float y) { return RoundRect(x, y, bx, by, 15, 13, 4); };
        };
        const auto back = box(56, 56), front = box(72, 72);
        canvas.Paint(Ink, [&](float x, float y) { return Cover(std::abs(back(x, y) + 2.0f) - 2.0f); });
        canvas.Paint(Fill, [&](float x, float y) { return Cover(front(x, y)); });
        canvas.Paint(Ink, [&](float x, float y) { return Cover(std::abs(front(x, y) + 2.0f) - 2.0f); });
        return;
    }
    DrawText(canvas, label, cx, cy, 78, label.size() <= 2 ? 46 : 34, Ink);
}

/// A shoulder button or a trigger of another gamepad.
void Shoulder(Canvas& canvas, std::string_view label) {
    const bool is_trigger = label == "LT" || label == "RT" || label == "ZL" || label == "ZR";
    const float hw = is_trigger ? 48 : 58, hh = is_trigger ? 54 : 34, radius = is_trigger ? 26 : 16;
    Plate(canvas, 5, [&](float x, float y) { return RoundRect(x, y, 64, 64, hw, hh, radius); });
    DrawText(canvas, label, 64, 64, hw * 1.4f, 42, Ink);
}

bool DrawOn(Canvas& canvas, Layout layout, const std::array<const char*, 4>& inputs) {
    char sz_labels[4][32] = {};
    int kinds[4] = {};
    const bool is_single = layout == Layout::Single;
    for (std::size_t i = 0; i < inputs.size() && inputs[i]; ++i) {
        kinds[i] = runtime_pad_prompt(inputs[i], is_single ? 1 : 0, sz_labels[i], sizeof(sz_labels[i]));
    }
    const int device = runtime_pad_prompt_device();
    if (layout == Layout::Stick && device == PromptKeyboard) {
        if (std::string_view{inputs[0]} == "look_up" && runtime_pad_mouse_look()) {
            Mouse(canvas, "Move");
            return true;
        }
    } else if (layout == Layout::Stick || is_single) {
        switch (kinds[0]) {
        case 'K':
            Key(canvas, sz_labels[0], 64, 64, 56, 48);
            return true;
        case 'M':
            Mouse(canvas, sz_labels[0]);
            return true;
        case 'F':
            Round(canvas, sz_labels[0], true);
            return true;
        case 'S':
            Shoulder(canvas, sz_labels[0]);
            return true;
        case 'C':
            Round(canvas, sz_labels[0], false);
            return true;
        default:
            return false;
        }
    }
    // Key clusters on the keyboard only: on a gamepad the d-pad's icons are the game's.
    if (device != PromptKeyboard || std::none_of(kinds, kinds + 4, [](int kind) { return kind == 'K'; })) {
        return false;
    }
    const auto key = [&](int i, float cx, float cy, float hw, float hh) {
        Key(canvas, kinds[i] == 'K' ? sz_labels[i] : "", cx, cy, hw, hh);
    };
    switch (layout) {
    case Layout::Column:
        key(0, 64, 34, 34, 28);
        key(1, 64, 94, 34, 28);
        break;
    case Layout::Row:
        key(0, 34, 64, 28, 34);
        key(1, 94, 64, 28, 34);
        break;
    case Layout::Single:
    case Layout::Cross:
    case Layout::Stick:
        // The first over the other three.
        key(0, 64, 30, 20, 21);
        key(1, 21, 86, 20, 21);
        key(2, 64, 86, 20, 21);
        key(3, 107, 86, 20, 21);
        break;
    }
    return true;
}

} // namespace

bool Draw(Layout layout, const std::array<const char*, 4>& inputs, std::uint32_t* p_out) {
    Canvas canvas;
    if (!DrawOn(canvas, layout, inputs)) {
        return false;
    }
    canvas.Store(p_out);
    return true;
}

} // namespace PsButtonPromptArt
