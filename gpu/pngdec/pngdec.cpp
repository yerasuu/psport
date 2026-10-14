// SPDX-License-Identifier: GPL-2.0-or-later
// libScePngDec: PNG chunks, unfiltering and Adam7 here, zlib streams through miniz's inflater. The
// working memory the game hands to scePngDecCreate holds the decoder's state and its address is
// the handle; the inflated rows live in host memory for the length of one decode.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>
#include <miniz.h>
#include "common/types.h"
#include "core/libraries/libs.h"

namespace Libraries::PngDec {

constexpr s32 ORBIS_PNG_DEC_ERROR_INVALID_ADDR = 0x80690001;
constexpr s32 ORBIS_PNG_DEC_ERROR_INVALID_SIZE = 0x80690002;
constexpr s32 ORBIS_PNG_DEC_ERROR_INVALID_PARAM = 0x80690003;
constexpr s32 ORBIS_PNG_DEC_ERROR_INVALID_HANDLE = 0x80690004;
constexpr s32 ORBIS_PNG_DEC_ERROR_INVALID_DATA = 0x80690010;
constexpr s32 ORBIS_PNG_DEC_ERROR_UNSUPPORT_DATA = 0x80690011;
constexpr s32 ORBIS_PNG_DEC_ERROR_DECODE_ERROR = 0x80690012;

struct CreateParam {
    u32 this_size, attribute, max_image_width;
};
struct ParseParam {
    const u8* png_mem_addr;
    u32 png_mem_size, reserved;
};
struct DecodeParam {
    const u8* png_mem_addr;
    u8* image_mem_addr;
    u32 png_mem_size, image_mem_size;
    u16 pixel_format, alpha_value;
    u32 image_pitch;
};
struct ImageInfo {
    u32 image_width, image_height;
    u16 color_space, bit_depth;
    u32 image_flag;
};
static_assert(sizeof(DecodeParam) == 32 && sizeof(ImageInfo) == 16);

enum ColorSpace : u16 { Grayscale = 2, Rgb = 3, Clut = 4, GrayscaleAlpha = 18, Rgba = 19 };
enum ImageFlag : u32 { Adam7Interlace = 1, TrnsChunkExist = 2 };
enum PixelFormat : u16 { R8G8B8A8 = 0, B8G8R8A8 = 1 };
constexpr u32 kAttributeBitDepth16 = 1;

constexpr u32 kDecoderMagic = 0x504e4744;
struct Decoder {
    u32 magic, attribute, max_image_width;
};

struct PngImage {
    u32 width = 0, height = 0;
    u8 bit_depth = 0, color_type = 0, interlace_method = 0;
    const u8* p_palette = nullptr;
    u32 c_palette_entries = 0;
    const u8* p_transparency = nullptr;
    u32 cb_transparency = 0;
    std::vector<u8> compressed;
};

static u32 ReadBe32(const u8* p_bytes) {
    return u32(p_bytes[0]) << 24 | u32(p_bytes[1]) << 16 | u32(p_bytes[2]) << 8 | p_bytes[3];
}

static int CountChannels(u8 color_type) {
    switch (color_type) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
    }
}

static bool IsValidDepth(u8 color_type, u8 bit_depth) {
    switch (color_type) {
    case 0: return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8 || bit_depth == 16;
    case 3: return bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8;
    case 2: case 4: case 6: return bit_depth == 8 || bit_depth == 16;
    default: return false;
    }
}

// Reads the chunks up to IEND; with should_stop_at_data, stops at the first IDAT once IHDR is known.
static s32 ParseChunks(const u8* p_data, u32 cb_data, PngImage& image, bool should_stop_at_data) {
    static constexpr u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (cb_data < 8 + 25 || std::memcmp(p_data, signature, 8)) return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    bool has_header = false;
    for (u64 off_chunk = 8; off_chunk + 12 <= cb_data;) {
        const u32 cb_chunk = ReadBe32(p_data + off_chunk);
        const u8* p_chunk_type = p_data + off_chunk + 4;
        const u8* p_chunk_data = p_data + off_chunk + 8;
        if (off_chunk + 12 + u64(cb_chunk) > cb_data) return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
        if (!std::memcmp(p_chunk_type, "IHDR", 4)) {
            if (cb_chunk != 13) return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
            image.width = ReadBe32(p_chunk_data);
            image.height = ReadBe32(p_chunk_data + 4);
            image.bit_depth = p_chunk_data[8];
            image.color_type = p_chunk_data[9];
            image.interlace_method = p_chunk_data[12];
            if (!image.width || !image.height || !IsValidDepth(image.color_type, image.bit_depth) ||
                p_chunk_data[10] || p_chunk_data[11] || image.interlace_method > 1)
                return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
            has_header = true;
        } else if (!has_header) {
            return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
        } else if (!std::memcmp(p_chunk_type, "PLTE", 4)) {
            if (cb_chunk % 3 || cb_chunk > 768) return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
            image.p_palette = p_chunk_data;
            image.c_palette_entries = cb_chunk / 3;
        } else if (!std::memcmp(p_chunk_type, "tRNS", 4)) {
            image.p_transparency = p_chunk_data;
            image.cb_transparency = cb_chunk;
        } else if (!std::memcmp(p_chunk_type, "IDAT", 4)) {
            if (should_stop_at_data) return 0;
            image.compressed.insert(image.compressed.end(), p_chunk_data, p_chunk_data + cb_chunk);
        } else if (!std::memcmp(p_chunk_type, "IEND", 4)) {
            break;
        }
        off_chunk += 12 + u64(cb_chunk);
    }
    if (!has_header) return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    if (image.color_type == 3 && !image.p_palette) return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    if (!should_stop_at_data && image.compressed.empty()) return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    return 0;
}

static void DescribeImage(const PngImage& image, ImageInfo* p_out_info) {
    static constexpr u16 color_spaces[7] = {Grayscale, 0, Rgb, Clut, GrayscaleAlpha, 0, Rgba};
    p_out_info->image_width = image.width;
    p_out_info->image_height = image.height;
    p_out_info->color_space = color_spaces[image.color_type];
    p_out_info->bit_depth = image.bit_depth;
    p_out_info->image_flag =
        (image.interlace_method ? u32(Adam7Interlace) : 0u) | (image.p_transparency ? u32(TrnsChunkExist) : 0u);
}

static u8 PredictPaeth(int left, int up, int up_left) {
    const int estimate = left + up - up_left;
    const int left_distance = estimate > left ? estimate - left : left - estimate;
    const int up_distance = estimate > up ? estimate - up : up - estimate;
    const int up_left_distance = estimate > up_left ? estimate - up_left : up_left - estimate;
    if (left_distance <= up_distance && left_distance <= up_left_distance) return u8(left);
    return u8(up_distance <= up_left_distance ? up : up_left);
}

// Undoes the filters of one pass in place; rows hold their filter byte first.
static bool UnfilterRows(u8* p_rows, u32 c_rows, size_t cb_row, size_t cb_pixel) {
    const u8* p_previous_row = nullptr;
    for (u32 y = 0; y < c_rows; ++y) {
        u8* p_row = p_rows + y * (cb_row + 1);
        const u8 filter_type = *p_row++;
        for (size_t i = 0; i < cb_row; ++i) {
            const int left = i >= cb_pixel ? p_row[i - cb_pixel] : 0;
            const int up = p_previous_row ? p_previous_row[i] : 0;
            const int up_left = p_previous_row && i >= cb_pixel ? p_previous_row[i - cb_pixel] : 0;
            switch (filter_type) {
            case 0: break;
            case 1: p_row[i] = u8(p_row[i] + left); break;
            case 2: p_row[i] = u8(p_row[i] + up); break;
            case 3: p_row[i] = u8(p_row[i] + ((left + up) >> 1)); break;
            case 4: p_row[i] = u8(p_row[i] + PredictPaeth(left, up, up_left)); break;
            default: return false;
            }
        }
        p_previous_row = p_row;
    }
    return true;
}

static u32 ReadSample(const u8* p_row, u32 i_sample, u8 bit_depth) {
    if (bit_depth == 16) return u32(p_row[i_sample * 2]) << 8 | p_row[i_sample * 2 + 1];
    if (bit_depth == 8) return p_row[i_sample];
    const u32 off_bit = i_sample * bit_depth;
    return (p_row[off_bit / 8] >> (8 - bit_depth - off_bit % 8)) & ((1u << bit_depth) - 1);
}

struct OutputImage {
    u8* p_base;
    u32 cb_pitch;
    bool is_wide, is_bgr;
    u16 opaque_alpha, missing_alpha;
};

// Takes RGBA at 16 bits per channel and writes it at the output's width.
static void StorePixel(const OutputImage& output, u32 x, u32 y, u32 red, u32 green, u32 blue,
                       u32 alpha) {
    if (output.is_bgr) std::swap(red, blue);
    if (output.is_wide) {
        u16* p_pixel = reinterpret_cast<u16*>(output.p_base + size_t(output.cb_pitch) * y) + size_t(x) * 4;
        p_pixel[0] = u16(red), p_pixel[1] = u16(green), p_pixel[2] = u16(blue);
        p_pixel[3] = u16(alpha);
    } else {
        u8* p_pixel = output.p_base + size_t(output.cb_pitch) * y + size_t(x) * 4;
        p_pixel[0] = u8(red >> 8), p_pixel[1] = u8(green >> 8), p_pixel[2] = u8(blue >> 8);
        p_pixel[3] = u8(alpha >> 8);
    }
}

static u32 ReadTransparentKey(const PngImage& image, u32 i_channel, u32 max_sample) {
    return (u32(image.p_transparency[i_channel * 2]) << 8 | image.p_transparency[i_channel * 2 + 1]) & max_sample;
}

static void ConvertRow(const PngImage& image, const OutputImage& output, const u8* p_row, u32 c_pixels,
                       u32 x_start, u32 x_step, u32 y) {
    const u32 c_channels = u32(CountChannels(image.color_type));
    const u32 max_sample = (1u << image.bit_depth) - 1;
    const u8 depth = image.bit_depth;
    const auto widen = [&](u32 sample) { return sample * 65535 / max_sample; };
    for (u32 i = 0; i < c_pixels; ++i) {
        const u32 i_first_sample = i * c_channels;
        u32 red, green, blue, alpha = output.missing_alpha;
        if (image.color_type == 3) {
            const u32 i_palette = ReadSample(p_row, i, depth);
            const u8* p_entry = i_palette < image.c_palette_entries ? image.p_palette + i_palette * 3 : nullptr;
            red = p_entry ? p_entry[0] * 257u : 0;
            green = p_entry ? p_entry[1] * 257u : 0;
            blue = p_entry ? p_entry[2] * 257u : 0;
            if (image.p_transparency)
                alpha = i_palette < image.cb_transparency ? image.p_transparency[i_palette] * 257u
                                                             : output.opaque_alpha;
        } else if (image.color_type == 0 || image.color_type == 4) {
            const u32 gray = ReadSample(p_row, i_first_sample, depth);
            red = green = blue = widen(gray);
            if (image.color_type == 4) alpha = widen(ReadSample(p_row, i_first_sample + 1, depth));
            else if (image.p_transparency && image.cb_transparency >= 2)
                alpha = gray == ReadTransparentKey(image, 0, max_sample) ? 0 : output.opaque_alpha;
        } else {
            const u32 red_sample = ReadSample(p_row, i_first_sample, depth);
            const u32 green_sample = ReadSample(p_row, i_first_sample + 1, depth);
            const u32 blue_sample = ReadSample(p_row, i_first_sample + 2, depth);
            red = widen(red_sample), green = widen(green_sample), blue = widen(blue_sample);
            if (image.color_type == 6) alpha = widen(ReadSample(p_row, i_first_sample + 3, depth));
            else if (image.p_transparency && image.cb_transparency >= 6) {
                const bool is_key = red_sample == ReadTransparentKey(image, 0, max_sample) &&
                                    green_sample == ReadTransparentKey(image, 1, max_sample) &&
                                    blue_sample == ReadTransparentKey(image, 2, max_sample);
                alpha = is_key ? 0 : output.opaque_alpha;
            }
        }
        StorePixel(output, x_start + i * x_step, y, red, green, blue, alpha);
    }
}

s32 PS4_SYSV_ABI scePngDecQueryMemorySize(const CreateParam* p_param) {
    if (!p_param) return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    if (p_param->this_size != sizeof(CreateParam)) return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    if (p_param->attribute > kAttributeBitDepth16 || !p_param->max_image_width) return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    return sizeof(Decoder);
}

s32 PS4_SYSV_ABI scePngDecCreate(const CreateParam* p_param, void* p_memory, u32 cb_memory, Decoder** pp_out_handle) {
    if (!p_param || !p_memory || !pp_out_handle) return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    const s32 cb_needed = scePngDecQueryMemorySize(p_param);
    if (cb_needed < 0) return cb_needed;
    if (cb_memory < u32(cb_needed)) return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    auto* p_decoder = static_cast<Decoder*>(p_memory);
    *p_decoder = {kDecoderMagic, p_param->attribute, p_param->max_image_width};
    *pp_out_handle = p_decoder;
    return 0;
}

s32 PS4_SYSV_ABI scePngDecDelete(Decoder* p_handle) {
    if (!p_handle || p_handle->magic != kDecoderMagic) return ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
    p_handle->magic = 0;
    return 0;
}

s32 PS4_SYSV_ABI scePngDecParseHeader(const ParseParam* p_param, ImageInfo* p_out_info) {
    if (!p_param || !p_out_info || !p_param->png_mem_addr) return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    PngImage image;
    const s32 result = ParseChunks(p_param->png_mem_addr, p_param->png_mem_size, image, true);
    if (result) return result;
    DescribeImage(image, p_out_info);
    return 0;
}

// Returns the image's width and height packed as (width << 16) | height, 0 when either does not
// fit in 15 bits.
s32 PS4_SYSV_ABI scePngDecDecode(Decoder* p_handle, const DecodeParam* p_param, ImageInfo* p_out_info) {
    if (!p_handle || p_handle->magic != kDecoderMagic) return ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
    if (!p_param || !p_param->png_mem_addr || !p_param->image_mem_addr) return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    if (p_param->pixel_format != R8G8B8A8 && p_param->pixel_format != B8G8R8A8) return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    PngImage image;
    s32 result = ParseChunks(p_param->png_mem_addr, p_param->png_mem_size, image, false);
    if (result) return result;
    if (p_out_info) DescribeImage(image, p_out_info);
    if (image.width > p_handle->max_image_width) return ORBIS_PNG_DEC_ERROR_UNSUPPORT_DATA;

    const bool is_wide = (p_handle->attribute & kAttributeBitDepth16) && image.bit_depth == 16;
    const u32 cb_output_pixel = is_wide ? 8 : 4;
    const u64 cb_output_row = u64(image.width) * cb_output_pixel;
    const u64 cb_pitch = p_param->image_pitch ? p_param->image_pitch : cb_output_row;
    if (cb_pitch < cb_output_row) return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    if (cb_pitch * (image.height - 1) + cb_output_row > p_param->image_mem_size)
        return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;

    size_t cb_inflated = 0;
    u8* p_inflated = static_cast<u8*>(tinfl_decompress_mem_to_heap(
        image.compressed.data(), image.compressed.size(), &cb_inflated, TINFL_FLAG_PARSE_ZLIB_HEADER));
    if (!p_inflated) return ORBIS_PNG_DEC_ERROR_DECODE_ERROR;

    const u32 bits_per_pixel = u32(CountChannels(image.color_type)) * image.bit_depth;
    const size_t cb_pixel = bits_per_pixel < 8 ? 1 : bits_per_pixel / 8;
    const u16 missing_alpha = is_wide ? p_param->alpha_value : u16(p_param->alpha_value * 257u);
    const OutputImage output{p_param->image_mem_addr, u32(cb_pitch), is_wide, p_param->pixel_format == B8G8R8A8,
                               0xffff, missing_alpha};
    // x start, y start, x step, y step of each pass.
    static constexpr u8 adam7_passes[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                                 {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    static constexpr u8 single_pass[1][4] = {{0, 0, 1, 1}};
    const auto* p_passes = image.interlace_method ? adam7_passes : single_pass;
    const int c_passes = image.interlace_method ? 7 : 1;
    size_t off_pass = 0;
    for (int i_pass = 0; i_pass < c_passes && !result; ++i_pass) {
        const u32 x_start = p_passes[i_pass][0], y_start = p_passes[i_pass][1];
        const u32 x_step = p_passes[i_pass][2], y_step = p_passes[i_pass][3];
        if (x_start >= image.width || y_start >= image.height) continue;
        const u32 c_columns = (image.width - x_start + x_step - 1) / x_step;
        const u32 c_rows = (image.height - y_start + y_step - 1) / y_step;
        const size_t cb_row = (size_t(c_columns) * bits_per_pixel + 7) / 8;
        if (off_pass + c_rows * (cb_row + 1) > cb_inflated || !UnfilterRows(p_inflated + off_pass, c_rows, cb_row, cb_pixel)) {
            result = ORBIS_PNG_DEC_ERROR_DECODE_ERROR;
            break;
        }
        for (u32 i_row = 0; i_row < c_rows; ++i_row)
            ConvertRow(image, output, p_inflated + off_pass + i_row * (cb_row + 1) + 1, c_columns, x_start, x_step,
                       y_start + i_row * y_step);
        off_pass += c_rows * (cb_row + 1);
    }
    mz_free(p_inflated);
    if (result) return result;
    return image.width > 32767 || image.height > 32767 ? 0
                                                                 : s32(image.width << 16 | image.height);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("-6srIGbLTIU", "libScePngDec", 1, "libScePngDec", scePngDecQueryMemorySize);
    LIB_FUNCTION("m0uW+8pFyaw", "libScePngDec", 1, "libScePngDec", scePngDecCreate);
    LIB_FUNCTION("U6h4e5JRPaQ", "libScePngDec", 1, "libScePngDec", scePngDecParseHeader);
    LIB_FUNCTION("WC216DD3El4", "libScePngDec", 1, "libScePngDec", scePngDecDecode);
    LIB_FUNCTION("QbD+eENEwo8", "libScePngDec", 1, "libScePngDec", scePngDecDelete);
}

} // namespace Libraries::PngDec
