// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "videodec2_impl.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_utils.h"
#include "videodec_error.h"

#include "common/support/avdec.h"

namespace Libraries::Videodec2 {

VdecDecoder::VdecDecoder(const OrbisVideodec2DecoderConfigInfo& config_info,
                         const OrbisVideodec2DecoderMemoryInfo& memory_info) {
    const AVCodec* codec = avcodec_find_decoder(
        config_info.codec_type == OrbisVideodec2CodecType::Avc ? AV_CODEC_ID_H264
                                                               : AV_CODEC_ID_HEVC);
    // bbport: a decoder FFmpeg cannot open is reported by sceVideodec2CreateDecoder.
    if (!codec) {
        LOG_ERROR(Lib_Vdec2, "No FFmpeg decoder for codec type {}",
                  static_cast<u32>(config_info.codec_type));
        return;
    }

    m_codec_context = avcodec_alloc_context3(codec);
    if (!m_codec_context) {
        return;
    }
    m_codec_context->width = config_info.max_frame_width;
    m_codec_context->height = config_info.max_frame_height;
    m_codec_context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    if (const int ret = avcodec_open2(m_codec_context, codec, nullptr); ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Could not open the FFmpeg decoder: {}", av_err2str(ret));
        avcodec_free_context(&m_codec_context);
    }
}

VdecDecoder::~VdecDecoder() {
    avcodec_free_context(&m_codec_context);
    sws_freeContext(m_sws_context);
}

s32 VdecDecoder::Decode(const OrbisVideodec2InputData& input_data,
                        OrbisVideodec2FrameBuffer& frame_buffer,
                        OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    if (!input_data.au_data) {
        LOG_ERROR(Lib_Vdec2, "ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER");
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER;
    }
    if (input_data.au_size == 0) {
        LOG_ERROR(Lib_Vdec2, "ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE");
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR(Lib_Vdec2, "Failed to allocate packet");
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    packet->data = (u8*)input_data.au_data;
    packet->size = input_data.au_size;
    packet->pts = input_data.pts_data;
    packet->dts = input_data.dts_data;
    packet->opaque = reinterpret_cast<void*>(input_data.attached_data);

    int ret = avcodec_send_packet(m_codec_context, packet);
    if (ret == AVERROR_EOF) {
        // Attempt to flush buffers and try again.
        avcodec_flush_buffers(m_codec_context);
        ret = avcodec_send_packet(m_codec_context, packet);
    }
    // bbport: one picture out per call, so the decoder can still hold one when the next access
    // unit comes; it is output now, then the access unit goes in.
    if (ret == AVERROR(EAGAIN)) {
        if (const s32 result = ReceiveFrame(frame_buffer, output_info); result != ORBIS_OK) {
            av_packet_free(&packet);
            return result;
        }
        ret = avcodec_send_packet(m_codec_context, packet);
    }
    av_packet_free(&packet);
    if (ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Error sending packet to decoder: {}", av_err2str(ret));
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }
    if (frame_buffer.is_accepted) {
        return ORBIS_OK;
    }
    return ReceiveFrame(frame_buffer, output_info);
}

s32 VdecDecoder::Flush(OrbisVideodec2FrameBuffer& frame_buffer,
                       OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    avcodec_send_packet(m_codec_context, nullptr);
    return ReceiveFrame(frame_buffer, output_info);
}

s32 VdecDecoder::Reset() {
    avcodec_flush_buffers(m_codec_context);
    return ORBIS_OK;
}

s32 VdecDecoder::ReceiveFrame(OrbisVideodec2FrameBuffer& frame_buffer,
                              OrbisVideodec2OutputInfo& output_info) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR(Lib_Vdec2, "Failed to allocate frame");
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    const int ret = avcodec_receive_frame(m_codec_context, frame);
    s32 result = ORBIS_OK;
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        LOG_TRACE(Lib_Vdec2, "AVERROR_EOF or AVERROR(EAGAIN)");
    } else if (ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Error receiving frame from decoder: {}", av_err2str(ret));
        result = ORBIS_VIDEODEC2_ERROR_API_FAIL;
    } else {
        result = WriteFrame(*frame, frame_buffer, output_info);
    }
    av_frame_free(&frame);
    return result;
}

s32 VdecDecoder::WriteFrame(AVFrame& decoded, OrbisVideodec2FrameBuffer& frame_buffer,
                            OrbisVideodec2OutputInfo& output_info) {
    static bool interlaced_logged = false;
    if ((decoded.flags & AV_FRAME_FLAG_INTERLACED) && !interlaced_logged) {
        interlaced_logged = true;
        LOG_ERROR(Lib_Vdec2, "Interlaced video output is not suported.");
    }

    AVFrame* frame = &decoded;
    if (decoded.format != AV_PIX_FMT_NV12) {
        frame = ConvertNV12Frame(decoded);
        if (!frame) {
            return ORBIS_VIDEODEC2_ERROR_API_FAIL;
        }
    }

    const bool is_avc = m_codec_context->codec_id == AV_CODEC_ID_H264;
    const u64 info_size =
        is_avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);
    const auto width = Common::AlignUp<u32>(frame->width, 16);
    const auto pitch = Common::AlignUp<u32>(frame->width, 64);
    const auto height = Common::AlignUp<u32>(frame->height, 16);
    const u64 frame_size = (u64(pitch) * height * 3) / 2;

    // bbport: the console's size error instead of asserting in CopyNV12Data.
    if (frame_buffer.frame_buffer_size < frame_size + info_size) {
        LOG_ERROR(Lib_Vdec2, "Frame buffer of {:#x} bytes for a {}x{} picture",
                  frame_buffer.frame_buffer_size, frame->width, frame->height);
        if (frame != &decoded) {
            av_frame_free(&frame);
        }
        return ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
    }

    Videodec::CopyNV12Data((u8*)frame_buffer.frame_buffer, frame_size, *frame);
    frame_buffer.is_accepted = true;

    output_info.is_valid = true;
    output_info.is_error_frame = false;
    output_info.picture_count = 1; // TODO: 2 pictures for interlaced video
    output_info.codec_type = is_avc ? OrbisVideodec2CodecType::Avc : OrbisVideodec2CodecType::Hevc;
    output_info.frame_width = width;
    output_info.frame_pitch = pitch;
    output_info.frame_height = height;
    output_info.frame_buffer = frame_buffer.frame_buffer;
    output_info.frame_buffer_size = frame_size;

    // Only set frameFormat and framePitchInBytes if the game uses the newer struct version.
    if (output_info.this_size == sizeof(OrbisVideodec2OutputInfo)) {
        output_info.frame_format = 0;
        output_info.frame_pitch_in_bytes = pitch;
    }

    u8* picture_info = (u8*)output_info.frame_buffer + output_info.frame_buffer_size;
    if (is_avc) {
        WriteAvcPictureInfo(*frame, width, height,
                            *reinterpret_cast<OrbisVideodec2AvcPictureInfo*>(picture_info));
    } else {
        WriteHevcPictureInfo(*frame, width, height,
                             *reinterpret_cast<OrbisVideodec2HevcPictureInfo*>(picture_info));
    }

    if (frame != &decoded) {
        av_frame_free(&frame);
    }
    return ORBIS_OK;
}

// bbport: the sequence's parameters as the stream carries them (sizes in macroblocks, cropping in
// chroma sample units) from what FFmpeg reports of it, rather than only the timestamps and pixels.
void VdecDecoder::WriteAvcPictureInfo(const AVFrame& frame, u32 width, u32 height,
                                      OrbisVideodec2AvcPictureInfo& info) const {
    const AVCodecContext& context = *m_codec_context;
    const bool frame_mbs_only = !(frame.flags & AV_FRAME_FLAG_INTERLACED);
    const u32 crop_unit_y = frame_mbs_only ? 2 : 4;

    info = {};
    info.this_size = sizeof(OrbisVideodec2AvcPictureInfo);
    info.is_valid = true;
    info.pts_data = frame.pts;
    info.dts_data = frame.pkt_dts;
    info.attached_data = reinterpret_cast<u64>(frame.opaque);
    info.idr_pictureflag = (frame.flags & AV_FRAME_FLAG_KEY) != 0;

    info.profile_idc = static_cast<u8>(context.profile & 0xff);
    info.level_idc = static_cast<u8>(context.level);
    info.constraint_set1_flag = (context.profile & AV_PROFILE_H264_CONSTRAINED) != 0;
    info.constraint_set3_flag = (context.profile & AV_PROFILE_H264_INTRA) != 0;
    info.pic_width_in_mbs_minus1 = width / 16 - 1;
    info.pic_height_in_map_units_minus1 = (frame_mbs_only ? height / 16 : height / 32) - 1;
    info.frame_mbs_only_flag = frame_mbs_only;

    info.frame_crop_right_offset = (width - frame.width) / 2;
    info.frame_crop_bottom_offset = (height - frame.height) / crop_unit_y;
    info.frame_cropping_flag = info.frame_crop_right_offset || info.frame_crop_bottom_offset;

    if (frame.sample_aspect_ratio.num > 0 && frame.sample_aspect_ratio.den > 0) {
        info.aspect_ratio_info_present_flag = 1;
        info.aspect_ratio_idc = 255;
        info.sar_width = static_cast<u16>(frame.sample_aspect_ratio.num);
        info.sar_height = static_cast<u16>(frame.sample_aspect_ratio.den);
    }

    info.video_full_range_flag = frame.color_range == AVCOL_RANGE_JPEG;
    info.colour_primaries = static_cast<u8>(frame.color_primaries);
    info.transfer_characteristics = static_cast<u8>(frame.color_trc);
    info.matrix_coefficients = static_cast<u8>(frame.colorspace);
    info.colour_description_present_flag = frame.color_primaries != AVCOL_PRI_UNSPECIFIED ||
                                           frame.color_trc != AVCOL_TRC_UNSPECIFIED ||
                                           frame.colorspace != AVCOL_SPC_UNSPECIFIED;
    info.video_format = 5;
    info.video_signal_type_present_flag =
        info.video_full_range_flag || info.colour_description_present_flag;

    if (context.framerate.num > 0 && context.framerate.den > 0) {
        info.timing_info_present_flag = 1;
        info.num_units_in_tick = static_cast<u32>(context.framerate.den);
        info.time_scale = static_cast<u32>(context.framerate.num) * 2;
    }
    info.sequence_parameter_set_present_flag = info.idr_pictureflag;
    info.picture_parameter_set_present_flag = info.idr_pictureflag;
}

void VdecDecoder::WriteHevcPictureInfo(const AVFrame& frame, u32 width, u32 height,
                                       OrbisVideodec2HevcPictureInfo& info) const {
    const AVCodecContext& context = *m_codec_context;

    info = {};
    info.this_size = sizeof(OrbisVideodec2HevcPictureInfo);
    info.is_valid = true;
    info.pts_data = frame.pts;
    info.dts_data = frame.pkt_dts;
    info.attached_data = reinterpret_cast<u64>(frame.opaque);
    info.idr_pictureflag = (frame.flags & AV_FRAME_FLAG_KEY) != 0;
    info.irap_picture_flag = info.idr_pictureflag;

    info.general_profile_idc = static_cast<u8>(context.profile);
    info.general_level_idc = static_cast<u8>(context.level);
    info.pic_width_in_luma_samples = width;
    info.pic_height_in_luma_samples = height;
    info.conf_win_right_offset = (width - frame.width) / 2;
    info.conf_win_bottom_offset = (height - frame.height) / 2;
    info.conformance_window_flag = info.conf_win_right_offset || info.conf_win_bottom_offset;

    info.video_full_range_flag = frame.color_range == AVCOL_RANGE_JPEG;
    info.colour_primaries = static_cast<u8>(frame.color_primaries);
    info.transfer_characteristics = static_cast<u8>(frame.color_trc);
    info.matrix_coeffs = static_cast<u8>(frame.colorspace);
    info.colour_description_present_flag = frame.color_primaries != AVCOL_PRI_UNSPECIFIED ||
                                           frame.color_trc != AVCOL_TRC_UNSPECIFIED ||
                                           frame.colorspace != AVCOL_SPC_UNSPECIFIED;
    info.video_format = 5;
    info.video_signal_type_present_flag =
        info.video_full_range_flag || info.colour_description_present_flag;

    if (context.framerate.num > 0 && context.framerate.den > 0) {
        info.timing_info_present_flag = 1;
        info.num_units_in_tick = static_cast<u32>(context.framerate.den);
        info.time_scale = static_cast<u32>(context.framerate.num);
    }
}

AVFrame* VdecDecoder::ConvertNV12Frame(AVFrame& frame) {
    AVFrame* nv12_frame = av_frame_alloc();
    if (!nv12_frame) {
        return nullptr;
    }
    av_frame_copy_props(nv12_frame, &frame);
    nv12_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    nv12_frame->format = AV_PIX_FMT_NV12;
    nv12_frame->width = frame.width;
    nv12_frame->height = frame.height;

    // bbport: the context follows the stream when its size or pixel format changes.
    m_sws_context = sws_getCachedContext(m_sws_context, frame.width, frame.height,
                                         AVPixelFormat(frame.format), nv12_frame->width,
                                         nv12_frame->height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR,
                                         nullptr, nullptr, nullptr);
    int res = m_sws_context ? av_frame_get_buffer(nv12_frame, 0) : AVERROR(EINVAL);
    if (res >= 0) {
        res = sws_scale(m_sws_context, frame.data, frame.linesize, 0, frame.height,
                        nv12_frame->data, nv12_frame->linesize);
    }
    if (res < 0) {
        LOG_ERROR(Lib_Vdec2, "Could not convert to NV12: {}", av_err2str(res));
        av_frame_free(&nv12_frame);
        return nullptr;
    }

    return nv12_frame;
}

} // namespace Libraries::Videodec2
