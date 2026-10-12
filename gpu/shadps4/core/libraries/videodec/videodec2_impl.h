// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "videodec2.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace Libraries::Videodec2 {

class VdecDecoder {
public:
    VdecDecoder(const OrbisVideodec2DecoderConfigInfo& configInfo,
                const OrbisVideodec2DecoderMemoryInfo& memoryInfo);
    ~VdecDecoder();

    bool IsOpen() const {
        return m_codec_context != nullptr;
    }

    s32 Decode(const OrbisVideodec2InputData& inputData, OrbisVideodec2FrameBuffer& frameBuffer,
               OrbisVideodec2OutputInfo& outputInfo);
    s32 Flush(OrbisVideodec2FrameBuffer& frameBuffer, OrbisVideodec2OutputInfo& outputInfo);
    s32 Reset();

private:
    s32 ReceiveFrame(OrbisVideodec2FrameBuffer& frameBuffer, OrbisVideodec2OutputInfo& outputInfo);
    s32 WriteFrame(AVFrame& frame, OrbisVideodec2FrameBuffer& frameBuffer,
                   OrbisVideodec2OutputInfo& outputInfo);
    void WriteAvcPictureInfo(const AVFrame& frame, u32 width, u32 height,
                             OrbisVideodec2AvcPictureInfo& info) const;
    void WriteHevcPictureInfo(const AVFrame& frame, u32 width, u32 height,
                              OrbisVideodec2HevcPictureInfo& info) const;
    AVFrame* ConvertNV12Frame(AVFrame& frame);

private:
    AVCodecContext* m_codec_context = nullptr;
    SwsContext* m_sws_context = nullptr;
};

} // namespace Libraries::Videodec2
