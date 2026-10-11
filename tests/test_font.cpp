// SPDX-License-Identifier: GPL-2.0-or-later
// libSceFont as a title drives it, without the game: a library and a renderer over FreeType, the
// European font set (its fallback, without the console's fonts), a glyph for 'A' rendered into a
// buffer as Atelier Ayesha does.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "font/font.h"
#include "font/fontft.h"

namespace {
void* Alloc(void*, u32 size) { return std::malloc(size); }
void Free(void*, void* p) { std::free(p); }
void* Realloc(void*, void* p, u32 size) { return std::realloc(p, size); }
void* Calloc(void*, u32 count, u32 size) { return std::calloc(count, size); }
constexpr u32 FontSetSstStdEuropean = 0x18070044;
constexpr int Side = 64;
} // namespace

int main() {
    using namespace Libraries::Font;
    unsetenv("BB_SYS_FONTS");
    const OrbisFontMemInterface iface{Alloc, Free, Realloc, Calloc, nullptr, nullptr};
    OrbisFontMem memory{};
    assert(sceFontMemoryInit(&memory, nullptr, 0, &iface, nullptr, nullptr, nullptr) == 0);

    // The selections are the driver tables a title passes back as opaque creation parameters.
    auto* library_driver = const_cast<void*>(
        static_cast<const void*>(Libraries::FontFt::sceFontSelectLibraryFt(0)));
    auto* renderer_driver = const_cast<void*>(
        static_cast<const void*>(Libraries::FontFt::sceFontSelectRendererFt(0)));
    OrbisFontLib library{};
    assert(sceFontCreateLibraryWithEdition(&memory, library_driver, 0, &library) == 0);
    assert(sceFontSupportSystemFonts(library) == 0);
    OrbisFontRenderer renderer{};
    assert(sceFontCreateRendererWithEdition(&memory, renderer_driver, 0, &renderer) == 0);

    OrbisFontHandle font{};
    assert(sceFontOpenFontSet(library, FontSetSstStdEuropean, 1, nullptr, &font) == 0);
    assert(sceFontBindRenderer(font, renderer) == 0);
    assert(sceFontSetScalePixel(font, 32.0f, 32.0f) == 0);

    OrbisFontGlyph glyph{};
    assert(sceFontGenerateCharGlyph(font, 'A', nullptr, &glyph) == 0 && glyph);

    std::vector<u8> pixels(Side * Side);
    OrbisFontRenderSurface surface{};
    sceFontRenderSurfaceInit(&surface, pixels.data(), Side, 1, Side, Side);
    OrbisFontGlyphMetrics metrics{};
    OrbisFontRenderOutput result{};
    assert(sceFontGlyphRenderImage(glyph, nullptr, renderer, &surface, 8.0f, 8.0f, &metrics,
                                   &result) == 0);
    std::size_t covered = 0;
    for (const u8 pixel : pixels) {
        covered += pixel != 0;
    }
    std::printf("font test: 'A' covers %zu of %d pixels, %ux%u at (%u, %u)\n", covered, Side * Side,
                result.UpdateRect.w, result.UpdateRect.h, result.UpdateRect.x, result.UpdateRect.y);
    assert(covered > 0);
    assert(metrics.width > 0.0f && metrics.height > 0.0f);
    assert(result.UpdateRect.x + result.UpdateRect.w <= Side);
    assert(result.UpdateRect.y + result.UpdateRect.h <= Side);

    assert(sceFontDeleteGlyph(&memory, &glyph) == 0);
    assert(sceFontUnbindRenderer(font) == 0);
    assert(sceFontDestroyRenderer(&renderer) == 0);
    std::printf("font test: ok\n");
    return 0;
}
