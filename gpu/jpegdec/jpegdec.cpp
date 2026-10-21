// SPDX-License-Identifier: GPL-2.0-or-later
// libSceJpegDec: the frame header is read here, the image decoded by FFmpeg's MJPEG decoder (its
// lowres mode does the 1/2, 1/4 and 1/8 scaling) and converted by libswscale into the game's
// buffer. The working memory the game hands to sceJpegDecCreate holds the decoder's state and its
// address is the handle; FFmpeg's own buffers live in host memory for the length of one decode.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
#include "common/logging/log.h"
#include "common/types.h"
#include "core/libraries/libs.h"

namespace Libraries::JpegDec {

constexpr s32 ORBIS_JPEG_DEC_ERROR_INVALID_ADDR = 0x80650001;
constexpr s32 ORBIS_JPEG_DEC_ERROR_INVALID_SIZE = 0x80650002;
constexpr s32 ORBIS_JPEG_DEC_ERROR_INVALID_PARAM = 0x80650003;
constexpr s32 ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE = 0x80650004;
constexpr s32 ORBIS_JPEG_DEC_ERROR_INVALID_COEF_MEMORY = 0x80650005;
constexpr s32 ORBIS_JPEG_DEC_ERROR_INVALID_WORK_MEMORY = 0x80650006;
constexpr s32 ORBIS_JPEG_DEC_ERROR_INVALID_DATA = 0x80650010;
constexpr s32 ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA = 0x80650011;
constexpr s32 ORBIS_JPEG_DEC_ERROR_DECODE_ERROR = 0x80650012;

struct CreateParam {
    u32 this_size, decode_mode, max_image_width;
};
struct ParseParam {
    const u8* jpeg_mem_addr;
    u32 jpeg_mem_size;
    u16 decode_option, down_scale;
};
struct DecodeParam {
    const u8* jpeg_mem_addr;
    u8* image_mem_addr;
    u8* coefficient_mem_addr;
    u32 jpeg_mem_size, image_mem_size, coefficient_mem_size;
    u16 decode_option, down_scale, pixel_format, alpha_value;
    u32 image_pitch;
};
struct ImageInfo {
    u32 image_width, image_height;
    u16 color_space, c_components;
    u8 horizontal_sampling[4], vertical_sampling[4];
    u32 cb_coefficient, suggested_mode;
    u32 output_width, output_height;
};
static_assert(sizeof(ParseParam) == 16 && sizeof(DecodeParam) == 0x30 && sizeof(ImageInfo) == 0x24);

enum ColorSpace : u16 { Grayscale = 2, YCbCr = 3, Cmyk = 4 };
// The mode only scales the work memory; what it selects is not known, so every stream decodes in
// either and the suggestion is the larger one for progressive streams.
enum DecodeMode : u32 { DecodeModeSmall = 1, DecodeModeLarge = 2 };
enum PixelFormat : u16 { R8G8B8A8 = 0, B8G8R8A8 = 1, Y8U8Y8V8 = 9, U8Y8V8Y8 = 10, Y8 = 11 };
constexpr u32 kMaxImageWidth = 0xffff;
constexpr u64 kCbMaxImage = 0x7fffffff;

struct Decoder {
    Decoder* p_self;
    u32 decode_mode, max_image_width;
};

struct JpegHeader {
    u32 width = 0, height = 0;
    u8 c_components = 0;
    u8 horizontal_sampling[4] = {}, vertical_sampling[4] = {};
    bool is_progressive = false;
};

static u32 RoundWorkWidth(u32 width) {
    return ((width + 31) & ~31u) | 0x10;
}

static bool IsFrameHeader(u8 marker) {
    return marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
}

static s32 ParseFrameHeader(const u8* p_data, u32 cb_data, JpegHeader& header) {
    if (cb_data < 4 || p_data[0] != 0xff || p_data[1] != 0xd8) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
    for (u64 off_marker = 2; off_marker + 4 <= cb_data;) {
        if (p_data[off_marker] != 0xff) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
        const u8 marker = p_data[off_marker + 1];
        if (marker == 0xff) {
            ++off_marker;
            continue;
        }
        if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
            off_marker += 2;
            continue;
        }
        if (marker == 0xd9 || marker == 0xda) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
        const u32 cb_segment = u32(p_data[off_marker + 2]) << 8 | p_data[off_marker + 3];
        if (cb_segment < 2 || off_marker + 2 + cb_segment > cb_data) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
        if (IsFrameHeader(marker)) {
            if (marker != 0xc0 && marker != 0xc1 && marker != 0xc2) return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
            const u8* p_segment = p_data + off_marker + 4;
            if (cb_segment < 8) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
            header.height = u32(p_segment[1]) << 8 | p_segment[2];
            header.width = u32(p_segment[3]) << 8 | p_segment[4];
            header.c_components = p_segment[5];
            if (p_segment[0] != 8 || !header.height || (header.c_components != 1 && header.c_components != 3 &&
                                                        header.c_components != 4))
                return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
            if (!header.width || cb_segment < 8u + 3u * header.c_components) return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
            for (u32 i = 0; i < header.c_components; ++i) {
                header.horizontal_sampling[i] = p_segment[7 + i * 3] >> 4;
                header.vertical_sampling[i] = p_segment[7 + i * 3] & 0xf;
                if (!header.horizontal_sampling[i] || !header.vertical_sampling[i])
                    return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
            }
            header.is_progressive = marker == 0xc2;
            return 0;
        }
        off_marker += 2 + cb_segment;
    }
    return ORBIS_JPEG_DEC_ERROR_INVALID_DATA;
}

// A progressive image keeps every block's 64 16-bit coefficients until its last scan.
static u32 CountCoefficientBytes(const JpegHeader& header) {
    if (!header.is_progressive) return 0;
    u32 max_horizontal = 1, max_vertical = 1;
    for (u32 i = 0; i < header.c_components; ++i) {
        max_horizontal = std::max<u32>(max_horizontal, header.horizontal_sampling[i]);
        max_vertical = std::max<u32>(max_vertical, header.vertical_sampling[i]);
    }
    const u64 c_mcu_columns = (header.width + 8 * max_horizontal - 1) / (8 * max_horizontal);
    const u64 c_mcu_rows = (header.height + 8 * max_vertical - 1) / (8 * max_vertical);
    u64 c_blocks = 0;
    for (u32 i = 0; i < header.c_components; ++i)
        c_blocks += c_mcu_columns * header.horizontal_sampling[i] * c_mcu_rows * header.vertical_sampling[i];
    const u64 cb_coefficients = c_blocks * 64 * sizeof(s16);
    return cb_coefficients > UINT32_MAX ? UINT32_MAX : u32(cb_coefficients);
}

static bool IsValidDownScale(u16 down_scale) {
    return down_scale == 1 || down_scale == 2 || down_scale == 4 || down_scale == 8;
}

static void DescribeImage(const JpegHeader& header, u16 down_scale, ImageInfo* p_out_info) {
    std::memset(p_out_info, 0, sizeof(*p_out_info));
    p_out_info->image_width = header.width;
    p_out_info->image_height = header.height;
    p_out_info->color_space = header.c_components == 1 ? Grayscale : header.c_components == 3 ? YCbCr : Cmyk;
    p_out_info->c_components = header.c_components;
    std::memcpy(p_out_info->horizontal_sampling, header.horizontal_sampling, 4);
    std::memcpy(p_out_info->vertical_sampling, header.vertical_sampling, 4);
    p_out_info->cb_coefficient = CountCoefficientBytes(header);
    p_out_info->suggested_mode = header.is_progressive ? DecodeModeLarge : DecodeModeSmall;
    p_out_info->output_width = (header.width + down_scale - 1) / down_scale;
    p_out_info->output_height = (header.height + down_scale - 1) / down_scale;
}

static bool IsValidHandle(const Decoder* p_handle) {
    return p_handle && !(reinterpret_cast<uintptr_t>(p_handle) & 0xf) && p_handle->p_self == p_handle;
}

s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const CreateParam* p_param) {
    if (!p_param) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    if (p_param->this_size != sizeof(CreateParam)) return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    if (p_param->decode_mode < DecodeModeSmall || p_param->decode_mode > DecodeModeLarge || !p_param->max_image_width ||
        p_param->max_image_width > kMaxImageWidth)
        return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    const u32 work_width = RoundWorkWidth(p_param->max_image_width);
    return s32(work_width * 6 + 0x7360 + work_width * (p_param->decode_mode == DecodeModeLarge ? 0xa0 : 0x30));
}

s32 PS4_SYSV_ABI sceJpegDecCreate(const CreateParam* p_param, void* p_memory, u32 cb_memory, Decoder** pp_out_handle) {
    const s32 cb_needed = sceJpegDecQueryMemorySize(p_param);
    if (cb_needed < 0) return cb_needed;
    if (!p_memory) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    if (cb_memory < u32(cb_needed)) return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    if (!pp_out_handle) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    auto* p_decoder = reinterpret_cast<Decoder*>((reinterpret_cast<uintptr_t>(p_memory) + 0xf) & ~uintptr_t(0xf));
    *p_decoder = {p_decoder, p_param->decode_mode, RoundWorkWidth(p_param->max_image_width)};
    *pp_out_handle = p_decoder;
    return 0;
}

s32 PS4_SYSV_ABI sceJpegDecDelete(Decoder* p_handle) {
    if (!IsValidHandle(p_handle)) return ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE;
    p_handle->p_self = nullptr;
    return 0;
}

s32 PS4_SYSV_ABI sceJpegDecParseHeader(const ParseParam* p_param, ImageInfo* p_out_info) {
    if (!p_param || !p_param->jpeg_mem_addr) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    if (!p_param->jpeg_mem_size) return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    if (p_param->decode_option > 1 || !IsValidDownScale(p_param->down_scale)) return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    if (!p_out_info) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    JpegHeader header;
    const s32 result = ParseFrameHeader(p_param->jpeg_mem_addr, p_param->jpeg_mem_size, header);
    if (result) return result;
    DescribeImage(header, p_param->down_scale, p_out_info);
    return 0;
}

struct DecodeState {
    AVCodecContext* p_codec = nullptr;
    AVPacket* p_packet = nullptr;
    AVFrame* p_source = nullptr;
    SwsContext* p_scaler = nullptr;
    ~DecodeState() {
        sws_freeContext(p_scaler);
        av_frame_free(&p_source);
        av_packet_free(&p_packet);
        avcodec_free_context(&p_codec);
    }
};

static AVPixelFormat TargetFormat(u16 pixel_format) {
    switch (pixel_format) {
    case R8G8B8A8: return AV_PIX_FMT_RGBA;
    case B8G8R8A8: return AV_PIX_FMT_BGRA;
    case Y8U8Y8V8: return AV_PIX_FMT_YUYV422;
    case U8Y8V8Y8: return AV_PIX_FMT_UYVY422;
    default: return AV_PIX_FMT_GRAY8;
    }
}

static s32 DecodeImage(const DecodeParam& param, u32 output_width, u32 output_height, u32 cb_pitch) {
    static const AVCodec* s_p_codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!s_p_codec) return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
    DecodeState state;
    state.p_codec = avcodec_alloc_context3(s_p_codec);
    state.p_packet = av_packet_alloc();
    state.p_source = av_frame_alloc();
    if (!state.p_codec || !state.p_packet || !state.p_source)
        return ORBIS_JPEG_DEC_ERROR_DECODE_ERROR;
    state.p_codec->lowres = param.down_scale == 8 ? 3 : param.down_scale == 4 ? 2 : param.down_scale == 2 ? 1 : 0;
    if (state.p_codec->lowres > s_p_codec->max_lowres) return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
    if (avcodec_open2(state.p_codec, s_p_codec, nullptr) < 0 || av_new_packet(state.p_packet, s32(param.jpeg_mem_size)) < 0)
        return ORBIS_JPEG_DEC_ERROR_DECODE_ERROR;
    std::memcpy(state.p_packet->data, param.jpeg_mem_addr, param.jpeg_mem_size);
    s32 result = avcodec_send_packet(state.p_codec, state.p_packet);
    if (result >= 0) result = avcodec_receive_frame(state.p_codec, state.p_source);
    if (result == AVERROR_PATCHWELCOME) return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
    if (result < 0 || u32(state.p_source->width) != output_width || u32(state.p_source->height) != output_height)
        return ORBIS_JPEG_DEC_ERROR_DECODE_ERROR;

    const AVFrame* p_source = state.p_source;
    state.p_scaler = sws_getContext(p_source->width, p_source->height, AVPixelFormat(p_source->format), s32(output_width),
                                    s32(output_height), TargetFormat(param.pixel_format), SWS_BILINEAR, nullptr, nullptr,
                                    nullptr);
    if (!state.p_scaler) return ORBIS_JPEG_DEC_ERROR_DECODE_ERROR;
    // JPEG's YCbCr is full range, and the YCbCr and luma outputs keep it.
    int* p_inverse_table = nullptr;
    int* p_table = nullptr;
    int source_range = 0, target_range = 0, brightness = 0, contrast = 0, saturation = 0;
    if (sws_getColorspaceDetails(state.p_scaler, &p_inverse_table, &source_range, &p_table, &target_range, &brightness,
                                 &contrast, &saturation) >= 0)
        sws_setColorspaceDetails(state.p_scaler, p_inverse_table, p_source->color_range != AVCOL_RANGE_MPEG, p_table, 1,
                                 brightness, contrast, saturation);
    u8* p_planes[4] = {param.image_mem_addr, nullptr, nullptr, nullptr};
    const int cb_strides[4] = {s32(cb_pitch), 0, 0, 0};
    if (sws_scale(state.p_scaler, p_source->data, p_source->linesize, 0, p_source->height, p_planes, cb_strides) !=
        s32(output_height))
        return ORBIS_JPEG_DEC_ERROR_DECODE_ERROR;
    if (param.pixel_format == R8G8B8A8 || param.pixel_format == B8G8R8A8)
        for (u32 y = 0; y < output_height; ++y)
            for (u32 x = 0; x < output_width; ++x)
                param.image_mem_addr[size_t(cb_pitch) * y + size_t(x) * 4 + 3] = u8(param.alpha_value);
    return 0;
}

// Returns the output's width and height packed as (width << 16) | height.
s32 PS4_SYSV_ABI sceJpegDecDecode(Decoder* p_handle, const DecodeParam* p_param, ImageInfo* p_out_info) {
    if (!IsValidHandle(p_handle)) return ORBIS_JPEG_DEC_ERROR_INVALID_HANDLE;
    if (!p_param || !p_param->jpeg_mem_addr || !p_param->image_mem_addr) return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    const u16 pixel_format = p_param->pixel_format;
    const bool is_luma_only = pixel_format == Y8;
    if (!is_luma_only && (reinterpret_cast<uintptr_t>(p_param->image_mem_addr) & 3))
        return ORBIS_JPEG_DEC_ERROR_INVALID_ADDR;
    if (!p_param->jpeg_mem_size || !p_param->image_mem_size) return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    if (p_param->decode_option > 1 || !IsValidDownScale(p_param->down_scale) || p_param->alpha_value > 0xff ||
        (pixel_format != R8G8B8A8 && pixel_format != B8G8R8A8 && pixel_format != Y8U8Y8V8 &&
         pixel_format != U8Y8V8Y8 && !is_luma_only))
        return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    JpegHeader header;
    s32 result = ParseFrameHeader(p_param->jpeg_mem_addr, p_param->jpeg_mem_size, header);
    if (result) return result;
    ImageInfo info;
    DescribeImage(header, p_param->down_scale, &info);
    if (p_out_info) *p_out_info = info;

    const bool is_packed_yuv = pixel_format == Y8U8Y8V8 || pixel_format == U8Y8V8Y8;
    const u32 cb_output_pixel = is_luma_only ? 1 : is_packed_yuv ? 2 : 4;
    const u64 c_row_pixels = is_packed_yuv ? (u64(info.output_width) + 1) & ~u64(1) : info.output_width;
    const u64 cb_output_row = c_row_pixels * cb_output_pixel;
    const u64 cb_pitch = p_param->image_pitch ? p_param->image_pitch : cb_output_row;
    if (p_param->image_pitch) {
        if ((!is_luma_only && (cb_pitch & 3)) || cb_pitch < cb_output_row || cb_pitch * info.output_height > kCbMaxImage ||
            cb_pitch * info.output_height > p_param->image_mem_size)
            return ORBIS_JPEG_DEC_ERROR_INVALID_PARAM;
    } else {
        if (cb_output_row * info.output_height > kCbMaxImage + 1) return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
        if (cb_output_row * info.output_height > p_param->image_mem_size) return ORBIS_JPEG_DEC_ERROR_INVALID_SIZE;
    }
    if (((info.output_width + 31) & ~31u) > p_handle->max_image_width)
        return ORBIS_JPEG_DEC_ERROR_INVALID_WORK_MEMORY;
    if (header.is_progressive &&
        (!p_param->coefficient_mem_addr || p_param->coefficient_mem_size < info.cb_coefficient))
        return ORBIS_JPEG_DEC_ERROR_INVALID_COEF_MEMORY;

    result = DecodeImage(*p_param, info.output_width, info.output_height, u32(cb_pitch));
    if (result) return result;
    return s32(info.output_width << 16 | info.output_height);
}

// Its input-control parameters are not known yet, so it fails visibly instead of guessing them.
s32 PS4_SYSV_ABI sceJpegDecDecodeWithInputControl() {
    static std::atomic<u32> s_c_calls;
    const u32 c_calls = ++s_c_calls;
    if (c_calls <= 8) LOG_ERROR(Lib_Jpeg, "sceJpegDecDecodeWithInputControl is not implemented (call {})", c_calls);
    return ORBIS_JPEG_DEC_ERROR_UNSUPPORT_DATA;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("uNAUmANZMEw", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecQueryMemorySize);
    LIB_FUNCTION("JPh3Zgg0Zwc", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecCreate);
    LIB_FUNCTION("LSinoSQH790", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecParseHeader);
    LIB_FUNCTION("1kzQRoWEgSA", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDecode);
    LIB_FUNCTION("919MhccOiII", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDecodeWithInputControl);
    LIB_FUNCTION("Hwh11+m5KoI", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDelete);
}

} // namespace Libraries::JpegDec
