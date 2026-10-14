// SPDX-License-Identifier: GPL-2.0-or-later
// libScePngEnc over miniz's PNG writer. miniz allocates its own buffers, so the working memory the
// game hands to scePngEncCreate only holds the encoder's state, and its address is the handle.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <miniz.h>
#include "common/types.h"
#include "core/libraries/libs.h"

namespace Libraries::PngEnc {

constexpr s32 ORBIS_PNG_ENC_ERROR_INVALID_ADDR = 0x80690101;
constexpr s32 ORBIS_PNG_ENC_ERROR_INVALID_SIZE = 0x80690102;
constexpr s32 ORBIS_PNG_ENC_ERROR_INVALID_PARAM = 0x80690103;
constexpr s32 ORBIS_PNG_ENC_ERROR_INVALID_HANDLE = 0x80690104;
constexpr s32 ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW = 0x80690110;

struct CreateParam {
    u32 this_size, attribute, max_image_width, max_filter_number;
};
struct EncodeParam {
    const u8* image_addr;
    u8* png_addr;
    u32 image_size, png_mem_size, image_width, image_height, image_pitch;
    u16 pixel_format, color_space, bit_depth, clut_number, filter_type, compression_level;
};
struct OutputInfo {
    u32 data_size, processed_height;
};
static_assert(sizeof(EncodeParam) == 48 && offsetof(EncodeParam, compression_level) == 46);

constexpr u32 kEncoderMagic = 0x504e4745;
struct Encoder {
    u32 magic, max_image_width;
};

enum PixelFormat : u16 { R8G8B8A8 = 0, B8G8R8A8 = 1 };
enum ColorSpace : u16 { Grayscale = 2, Rgb = 3, GrayscaleAlpha = 18, Rgba = 19 };

static bool ValidCreateParam(const CreateParam* param) {
    return param->this_size == sizeof(CreateParam) && param->max_image_width > 0;
}

s32 PS4_SYSV_ABI scePngEncQueryMemorySize(const CreateParam* param) {
    if (!param) return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    if (!ValidCreateParam(param)) return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    return sizeof(Encoder);
}

s32 PS4_SYSV_ABI scePngEncCreate(const CreateParam* param, void* memory, u32 size, Encoder** handle) {
    if (!param || !memory || !handle) return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    if (!ValidCreateParam(param)) return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    if (size < sizeof(Encoder)) return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    auto* encoder = static_cast<Encoder*>(memory);
    *encoder = {kEncoderMagic, param->max_image_width};
    *handle = encoder;
    return 0;
}

s32 PS4_SYSV_ABI scePngEncDelete(Encoder* handle) {
    if (!handle || handle->magic != kEncoderMagic) return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    handle->magic = 0;
    return 0;
}

s32 PS4_SYSV_ABI scePngEncEncode(Encoder* handle, const EncodeParam* param, OutputInfo* info) {
    if (!handle || handle->magic != kEncoderMagic) return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    if (!param || !param->image_addr || !param->png_addr) return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    const u32 width = param->image_width, height = param->image_height;
    int channels;
    switch (param->color_space) {
    case Grayscale: channels = 1; break;
    case GrayscaleAlpha: channels = 2; break;
    case Rgb: channels = 3; break;
    case Rgba: channels = 4; break;
    default: return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    }
    if (!width || !height || width > handle->max_image_width || param->bit_depth != 8 ||
        (param->pixel_format != R8G8B8A8 && param->pixel_format != B8G8R8A8) || param->compression_level > 9 ||
        param->image_pitch < width * 4)
        return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    if (u64(param->image_pitch) * (height - 1) + u64(width) * 4 > param->image_size)
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;

    const int red = param->pixel_format == B8G8R8A8 ? 2 : 0, blue = 2 - red;
    std::vector<u8> pixels(size_t(width) * height * channels);
    u8* out = pixels.data();
    for (u32 y = 0; y < height; ++y) {
        const u8* row = param->image_addr + size_t(param->image_pitch) * y;
        for (u32 x = 0; x < width; ++x, row += 4) {
            if (channels >= 3) {
                *out++ = row[red];
                *out++ = row[1];
                *out++ = row[blue];
            } else {
                *out++ = row[red];
            }
            if (channels == 2 || channels == 4) *out++ = row[3];
        }
    }
    size_t length = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(pixels.data(), int(width), int(height), channels,
                                                           &length, param->compression_level, MZ_FALSE);
    if (!png) return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    s32 result = ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW;
    if (length <= param->png_mem_size) {
        std::memcpy(param->png_addr, png, length);
        result = s32(length);
    }
    mz_free(png);
    if (info) *info = {result < 0 ? 0 : u32(length), result < 0 ? 0 : height};
    return result;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("9030RnBDoh4", "libScePngEnc", 1, "libScePngEnc", scePngEncQueryMemorySize);
    LIB_FUNCTION("7aGTPfrqT9s", "libScePngEnc", 1, "libScePngEnc", scePngEncCreate);
    LIB_FUNCTION("xgDjJKpcyHo", "libScePngEnc", 1, "libScePngEnc", scePngEncEncode);
    LIB_FUNCTION("RUrWdwTWZy8", "libScePngEnc", 1, "libScePngEnc", scePngEncDelete);
}

} // namespace Libraries::PngEnc
