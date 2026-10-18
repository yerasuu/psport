// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/libraries/avplayer/avplayer_error.h"
#include "core/libraries/avplayer/avplayer_file_streamer.h"
#include "core/libraries/avplayer/avplayer_source.h"
#include "core/libraries/videodec/video_utils.h"
#include "core/memory.h"

#include <magic_enum/magic_enum.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "common/support/avdec.h"

extern "C" int runtime_file_translate(const char* guest, char* out, size_t size);

namespace Libraries::AvPlayer {

static AvPlayerStreamType CodecTypeToStreamType(AVMediaType codec_type) {
    switch (codec_type) {
    case AVMediaType::AVMEDIA_TYPE_VIDEO:
        return AvPlayerStreamType::Video;
    case AVMediaType::AVMEDIA_TYPE_AUDIO:
        return AvPlayerStreamType::Audio;
    case AVMediaType::AVMEDIA_TYPE_SUBTITLE:
        return AvPlayerStreamType::TimedText;
    default:
        LOG_ERROR(Lib_AvPlayer, "Unexpected AVMediaType {}", magic_enum::enum_name(codec_type));
        return AvPlayerStreamType::Unknown;
    }
}

static bool IsStreamSupported(AVStream* stream) {
    const auto codec_id = stream->codecpar->codec_id;
    return codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_HEVC ||
           codec_id == AV_CODEC_ID_AAC;
}

AvPlayerSource::AvPlayerSource(AvPlayerStateCallback& state) : m_state(state) {}

AvPlayerSource::~AvPlayerSource() {
    Stop();
}

bool AvPlayerSource::Init(const AvPlayerInitData& init_data, std::string_view path) {
    m_memory_replacement = init_data.memory_replacement;
    m_max_num_video_framebuffers =
        std::min(std::max(2, init_data.num_output_video_framebuffers), 16);

    AVFormatContext* context = avformat_alloc_context();
    if (context == nullptr) {
        LOG_ERROR(Lib_AvPlayer, "Could not allocate AVFormatContext for {}", path);
        return false;
    }

    const auto attach_custom_io = [&]() {
        context->pb = m_up_data_streamer->GetContext();
        context->flags |= AVFMT_FLAG_CUSTOM_IO;
    };
    // psport: libSceAvPlayer's MP4 parser does not subtract an edit list's media time, so frames
    // keep their sample table timestamps, as games see them on the console (kb #771).
    const auto open_input = [&](const char* sz_url) {
        AVDictionary* p_options = nullptr;
        if (av_dict_set(&p_options, "ignore_editlist", "1", 0) < 0) {
            LOG_WARNING(Lib_AvPlayer, "Could not set ignore_editlist for {}", path);
        }
        const auto res = avformat_open_input(&context, sz_url, nullptr, &p_options);
        av_dict_free(&p_options);
        return res;
    };

    if (init_data.file_replacement.open != nullptr) {
        LOG_INFO(Lib_AvPlayer, "Opening {} through the game-provided file replacement", path);
        m_up_data_streamer = std::make_unique<AvPlayerFileStreamer>(init_data.file_replacement);
        if (!m_up_data_streamer->Init(path)) {
            avformat_free_context(context);
            return false;
        }
        attach_custom_io();
        if (AVPLAYER_IS_ERROR(open_input(nullptr))) {
            return false;
        }
    } else {
        // psport: guest paths are translated by the port's file system (runtime_file.c).
        char host[1024];
        if (runtime_file_translate(std::string(path).c_str(), host, sizeof(host)) != 0) {
            LOG_ERROR(Lib_AvPlayer, "Cannot translate guest path {}", path);
            avformat_free_context(context);
            return false;
        }
        LOG_INFO(Lib_AvPlayer, "Opening {} from host path {}", path, host);
        if (AVPLAYER_IS_ERROR(open_input(host))) {
            return false;
        }
    }
    m_avformat_context = AVFormatContextPtr(context, &ReleaseAVFormatContext);
    ReadEmptyEditDelays(path);

    return true;
}

namespace {

struct Mp4Box {
    u32 type;
    s64 off_end;
};

std::optional<Mp4Box> ReadMp4Box(AVIOContext* p_io, s64 off_limit) {
    const auto off_box = avio_tell(p_io);
    if (off_box < 0 || off_limit - off_box < 8) {
        return std::nullopt;
    }
    u64 cb_box = avio_rb32(p_io);
    const auto type = avio_rb32(p_io);
    u64 cb_header = 8;
    if (cb_box == 1) {
        cb_box = avio_rb64(p_io);
        cb_header += 8;
    } else if (cb_box == 0) {
        cb_box = u64(off_limit - off_box);
    }
    if (p_io->eof_reached || cb_box < cb_header || cb_box > u64(off_limit - off_box)) {
        return std::nullopt;
    }
    return Mp4Box{type, off_box + s64(cb_box)};
}

// libSceAvPlayer (0x2e700) honours only an edit list of exactly an empty edit followed by an edit
// at media time 0; any other edit list leaves the sample times as they are.
std::optional<u64> ReadEmptyEditDuration(AVIOContext* p_io) {
    const auto version = avio_r8(p_io);
    avio_skip(p_io, 3);
    if (version > 1 || avio_rb32(p_io) != 2) {
        return std::nullopt;
    }
    const auto read_entry = [&] {
        const u64 duration = version == 1 ? avio_rb64(p_io) : avio_rb32(p_io);
        const s64 media_time = version == 1 ? s64(avio_rb64(p_io)) : s32(avio_rb32(p_io));
        avio_skip(p_io, 4);
        return std::pair{duration, media_time};
    };
    const auto [empty_duration, empty_media_time] = read_entry();
    const auto media_time = read_entry().second;
    if (p_io->eof_reached || empty_media_time != -1 || media_time != 0 ||
        empty_duration > u64(INT64_MAX)) {
        return std::nullopt;
    }
    return empty_duration;
}

} // namespace

// psport: FFmpeg opens MP4 files with ignore_editlist and then drops the edit list entirely, while
// libSceAvPlayer adds an empty edit's duration to every sample time of its track (0x2bc40 starts
// the decode time at it), so the edit lists are read here.
void AvPlayerSource::ReadEmptyEditDelays(std::string_view path) {
    const auto p_context = m_avformat_context.get();
    m_empty_edit_delays.assign(p_context->nb_streams, 0);
    if (std::string_view(p_context->iformat->name).find("mp4") == std::string_view::npos) {
        return;
    }
    const auto p_io = p_context->pb;
    const auto off_resume = avio_tell(p_io);
    const auto cb_file = avio_size(p_io);
    const auto off_file_end = cb_file > 0 ? cb_file : INT64_MAX;
    if (off_resume < 0 || avio_seek(p_io, 0, SEEK_SET) < 0) {
        LOG_WARNING(Lib_AvPlayer, "Could not read the edit lists of {}: not seekable", path);
        return;
    }

    std::vector<std::pair<u32, u64>> empty_edits;
    u32 movie_timescale = 0;
    u32 track_id = 0;
    bool has_moov = false;
    auto off_limit = off_file_end;
    while (const auto box = ReadMp4Box(p_io, off_limit)) {
        switch (box->type) {
        case MKBETAG('m', 'o', 'o', 'v'):
            has_moov = true;
            off_limit = box->off_end;
            continue;
        case MKBETAG('t', 'r', 'a', 'k'):
            track_id = 0;
            continue;
        case MKBETAG('e', 'd', 't', 's'):
            continue;
        case MKBETAG('m', 'v', 'h', 'd'): {
            const auto version = avio_r8(p_io);
            avio_skip(p_io, 3 + (version == 1 ? 16 : 8));
            movie_timescale = avio_rb32(p_io);
            break;
        }
        case MKBETAG('t', 'k', 'h', 'd'): {
            const auto version = avio_r8(p_io);
            avio_skip(p_io, 3 + (version == 1 ? 16 : 8));
            track_id = avio_rb32(p_io);
            break;
        }
        case MKBETAG('e', 'l', 's', 't'):
            if (const auto duration = ReadEmptyEditDuration(p_io); duration && *duration != 0) {
                empty_edits.emplace_back(track_id, *duration);
            }
            break;
        default:
            break;
        }
        if (avio_seek(p_io, box->off_end, SEEK_SET) < 0) {
            break;
        }
    }
    if (avio_seek(p_io, off_resume, SEEK_SET) < 0) {
        LOG_ERROR(Lib_AvPlayer, "Could not seek {} back to {} after reading its edit lists", path,
                  off_resume);
    }
    if (!has_moov) {
        LOG_WARNING(Lib_AvPlayer, "Could not read the edit lists of {}: no moov box found", path);
        return;
    }
    if (!empty_edits.empty() && (movie_timescale == 0 || movie_timescale > u32(INT32_MAX))) {
        LOG_WARNING(Lib_AvPlayer, "Ignoring the empty edits of {}: invalid movie timescale {}",
                    path, movie_timescale);
        return;
    }

    for (const auto& [empty_track_id, duration] : empty_edits) {
        for (u32 i_stream = 0; i_stream < p_context->nb_streams; ++i_stream) {
            const auto p_stream = p_context->streams[i_stream];
            if (u32(p_stream->id) != empty_track_id) {
                continue;
            }
            const auto delay = av_rescale_q(s64(duration), AVRational{1, s32(movie_timescale)},
                                            p_stream->time_base);
            if (delay < 0) {
                LOG_WARNING(Lib_AvPlayer,
                            "Ignoring the empty edit of stream {} of {}: {}/{} s overflows its "
                            "time base",
                            i_stream, path, duration, movie_timescale);
                continue;
            }
            m_empty_edit_delays[i_stream] = delay;
            LOG_INFO(Lib_AvPlayer, "Stream {} of {} starts {} ms late: empty edit of {}/{} s",
                     i_stream, path,
                     av_rescale_q(m_empty_edit_delays[i_stream], p_stream->time_base,
                                  AVRational{1, 1000}),
                     duration, movie_timescale);
        }
    }
}

// psport: demuxers without a header (MPEG-TS, FLV, raw streams) add their streams only while
// probing, after the edit lists were read, so the delays are extended to the final stream count.
bool AvPlayerSource::FindStreams() {
    const auto has_info = avformat_find_stream_info(m_avformat_context.get(), nullptr) >= 0;
    m_empty_edit_delays.resize(m_avformat_context->nb_streams, 0);
    if (has_info) {
        m_streams.reserve(m_avformat_context->nb_streams);
        for (u32 idx = 0; idx < m_avformat_context->nb_streams; ++idx) {
            if (!IsStreamSupported(m_avformat_context->streams[idx])) {
                continue;
            }
            m_streams.push_back({idx, CreateStreamInfo(idx)});
        }
    }
    m_duration = DurationMillis();
    return m_streams.size() > 0;
}

s32 AvPlayerSource::GetStreamCount() {
    LOG_INFO(Lib_AvPlayer, "Stream Count: {}", m_streams.size());
    return m_streams.size();
}

static u64 TimestampToMillis(s64 timestamp, AVRational time_base) {
    if (timestamp == AV_NOPTS_VALUE || timestamp <= 0 || time_base.num <= 0 || time_base.den <= 0) {
        return 0;
    }

    const auto millis = av_rescale_q(timestamp, time_base, AVRational{1, 1000});
    return millis > 0 ? u64(millis) : 0;
}

static u64 StreamDurationMillis(const AVFormatContext& context, const AVStream& stream) {
    const auto stream_duration = TimestampToMillis(stream.duration, stream.time_base);
    if (stream_duration != 0) {
        return stream_duration;
    }
    return TimestampToMillis(context.duration, AVRational{1, AV_TIME_BASE});
}

AvPlayerStreamInfo AvPlayerSource::CreateStreamInfo(u32 stream_index) {
    AvPlayerStreamInfo info = {};
    const auto p_stream = m_avformat_context->streams[stream_index];
    info.type = CodecTypeToStreamType(p_stream->codecpar->codec_type);
    info.start_time = 0; // start_time is currently unused and defaults to 0.
    info.duration = StreamDurationMillis(*m_avformat_context, *p_stream);
    const auto p_lang_node = av_dict_get(p_stream->metadata, "language", nullptr, 0);
    if (p_lang_node != nullptr) {
        LOG_INFO(Lib_AvPlayer, "Stream {} language = {}", stream_index, p_lang_node->value);
    } else {
        LOG_WARNING(Lib_AvPlayer, "Stream {} language is unknown", stream_index);
    }
    switch (info.type) {
    case AvPlayerStreamType::Video: {
        LOG_INFO(Lib_AvPlayer, "Stream {} is a video stream.", stream_index);
        info.details.video.aspect_ratio =
            f32(p_stream->codecpar->width) / p_stream->codecpar->height;
        const auto width = Common::AlignUp<u32>(p_stream->codecpar->width, 16);
        const auto height = Common::AlignUp<u32>(p_stream->codecpar->height, 16);
        info.details.video.width = width;
        info.details.video.height = height;
        if (p_lang_node != nullptr) {
            std::memcpy(info.details.video.language_code, p_lang_node->value,
                        std::min(strlen(p_lang_node->value), size_t(3)));
        }
        break;
    }
    case AvPlayerStreamType::Audio: {
        LOG_INFO(Lib_AvPlayer, "Stream {} is an audio stream.", stream_index);
        info.details.audio.channel_count = p_stream->codecpar->ch_layout.nb_channels;
        info.details.audio.sample_rate = p_stream->codecpar->sample_rate;
        info.details.audio.size = 0; // sceAvPlayerGetStreamInfo() is expected to set this to 0
        if (p_lang_node != nullptr) {
            std::memcpy(info.details.audio.language_code, p_lang_node->value,
                        std::min(strlen(p_lang_node->value), size_t(3)));
        }
        break;
    }
    case AvPlayerStreamType::TimedText: {
        info.details.subs.font_size = 12;
        info.details.subs.text_size = 12;
        if (p_lang_node != nullptr) {
            std::memcpy(info.details.subs.language_code, p_lang_node->value,
                        std::min(strlen(p_lang_node->value), size_t(3)));
        }
        break;
    }
    default: {
        LOG_ERROR_COUNTED(Lib_AvPlayer, "Stream {} of media type {} reported without details",
                          stream_index, magic_enum::enum_name(p_stream->codecpar->codec_type));
        break;
    }
    }
    return info;
}

bool AvPlayerSource::GetStreamInfo(u32 stream_index, AvPlayerStreamInfo& info) {
    if (stream_index >= m_streams.size()) {
        LOG_ERROR(Lib_AvPlayer, "Could not get stream {} info.", stream_index);
        return false;
    }
    info = m_streams[stream_index].info;
    return true;
}

bool AvPlayerSource::EnableStream(u32 stream_index) {
    if (m_avformat_context == nullptr || stream_index >= m_streams.size()) {
        return false;
    }
    stream_index = m_streams[stream_index].ffmpeg_index;
    const auto stream = m_avformat_context->streams[stream_index];
    switch (stream->codecpar->codec_type) {
    case AVMediaType::AVMEDIA_TYPE_VIDEO: {
        m_video_stream_index = stream_index;
        LOG_INFO(Lib_AvPlayer, "Video stream {} enabled", stream_index);
        break;
    }
    case AVMediaType::AVMEDIA_TYPE_AUDIO: {
        m_audio_stream_index = stream_index;
        LOG_INFO(Lib_AvPlayer, "Audio stream {} enabled", stream_index);
        break;
    }
    default:
        LOG_WARNING(Lib_AvPlayer, "Unknown stream type {} for stream {}",
                    magic_enum::enum_name(stream->codecpar->codec_type), stream_index);
        break;
    }
    return true;
}

void AvPlayerSource::SetLooping(bool is_looping) {
    m_is_looping = is_looping;
}

std::optional<bool> AvPlayerSource::HasFrames(u32 num_frames) {
    return m_video_packets.Size() > num_frames || m_is_eof;
}

bool AvPlayerSource::Start(u64 time) {
    std::unique_lock lock(m_state_mutex);

    if (!m_video_stream_index && !m_audio_stream_index) {
        LOG_ERROR(Lib_AvPlayer, "Could not start playback. No streams.");
        return false;
    }
    if (m_video_stream_index) {
        const auto stream_index = m_video_stream_index.value();
        const auto stream = m_avformat_context->streams[stream_index];
        const auto ts = std::max<s64>(
            0, av_rescale_q(time, AVRational{1, 1000}, stream->time_base) -
                   EmptyEditDelay(stream_index));
        avformat_seek_file(m_avformat_context.get(), stream_index, 0, ts,
                           time == 0 ? stream->duration : ts, 0);
        const auto decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            return false;
        }
        m_video_codec_context =
            AVCodecContextPtr(avcodec_alloc_context3(decoder), &ReleaseAVCodecContext);
        if (avcodec_parameters_to_context(m_video_codec_context.get(), stream->codecpar) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not copy stream {} avcodec parameters to context.",
                      m_video_stream_index.value());
            return false;
        }
        if (avcodec_open2(m_video_codec_context.get(), decoder, nullptr) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not open avcodec for video stream {}.",
                      m_video_stream_index.value());
            return false;
        }
        const auto pitch = Common::AlignUp<u32>(m_video_codec_context->width, 64);
        const auto height = Common::AlignUp<u32>(m_video_codec_context->height, 16);
        const auto size = (pitch * height * 3) / 2;
        for (u64 index = 0; index < m_max_num_video_framebuffers; ++index) {
            GuestBuffer buffer(m_memory_replacement, 0x100, size, true);
            if (buffer.GetBuffer() == nullptr) {
                LOG_ERROR_COUNTED(Lib_AvPlayer,
                                  "Game allocator gave no video frame buffer of {:#x} bytes", size);
                return false;
            }
            m_video_buffers.Push(std::move(buffer));
        }
    }
    if (m_audio_stream_index) {
        const auto stream_index = m_audio_stream_index.value();
        const auto stream = m_avformat_context->streams[stream_index];
        const auto ts = std::max<s64>(
            0, av_rescale_q(time, AVRational{1, 1000}, stream->time_base) -
                   EmptyEditDelay(stream_index));
        avformat_seek_file(m_avformat_context.get(), stream_index, 0, ts,
                           time == 0 ? stream->duration : ts, 0);
        if (time != 0) {
            m_last_audio_ts = time;
        }
        const auto decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            return false;
        }
        m_audio_codec_context =
            AVCodecContextPtr(avcodec_alloc_context3(decoder), &ReleaseAVCodecContext);
        if (avcodec_parameters_to_context(m_audio_codec_context.get(), stream->codecpar) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not copy stream {} avcodec parameters to context.",
                      m_audio_stream_index.value());
            return false;
        }
        if (avcodec_open2(m_audio_codec_context.get(), decoder, nullptr) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not open avcodec for audio stream {}.",
                      m_audio_stream_index.value());
            return false;
        }
        constexpr u8 max_channels = 8;
        constexpr size_t max_sample_size = sizeof(s32);
        const auto size = max_channels * max_sample_size * 1024;
        for (u64 index = 0; index < (m_max_num_video_framebuffers * 2); ++index) {
            GuestBuffer buffer(m_memory_replacement, 0x10, size, false);
            if (buffer.GetBuffer() == nullptr) {
                LOG_ERROR_COUNTED(Lib_AvPlayer,
                                  "Game allocator gave no audio frame buffer of {:#x} bytes", size);
                return false;
            }
            m_audio_buffers.Push(std::move(buffer));
        }
    }
    m_demuxer_thread.Run([this](std::stop_token stop) { this->DemuxerThread(stop); });
    m_video_decoder_thread.Run([this](std::stop_token stop) { this->VideoDecoderThread(stop); });
    m_audio_decoder_thread.Run([this](std::stop_token stop) { this->AudioDecoderThread(stop); });
    m_start_time = std::chrono::high_resolution_clock::now() - std::chrono::milliseconds(time);
    m_last_video_ts = time;
    return true;
}

void AvPlayerSource::Stop() {
    std::unique_lock lock(m_state_mutex);

    m_video_decoder_thread.Stop();
    m_audio_decoder_thread.Stop();
    m_demuxer_thread.Stop();

    m_current_audio_frame.reset();
    m_current_video_frame.reset();

    m_video_buffers.Clear();
    m_audio_buffers.Clear();
    m_audio_packets.Clear();
    m_video_packets.Clear();
    m_audio_frames.Clear();
    m_video_frames.Clear();

    m_last_audio_ts.reset();
    m_last_video_ts = 0;
    m_last_data_time.reset();
    m_start_time.reset();
    m_pause_time = {};
    m_pause_duration = {};

    m_is_paused = false;
    m_is_eof = false;
}

bool AvPlayerSource::JumpToTime(u64 time) {
    const bool is_paused = m_is_paused;
    Stop();
    if (!Start(time)) {
        return false;
    }
    if (is_paused) {
        Pause();
    }
    return true;
}

void AvPlayerSource::Pause() {
    m_pause_time = std::chrono::high_resolution_clock::now();
    m_is_paused = true;
}

void AvPlayerSource::Resume() {
    m_pause_duration += std::chrono::high_resolution_clock::now() - m_pause_time;
    m_is_paused = false;
}

bool AvPlayerSource::GetVideoData(AvPlayerFrameInfo& video_info) {
    AvPlayerFrameInfoEx info{};
    if (!GetVideoData(info)) {
        return false;
    }
    video_info = {};
    video_info.timestamp = u64(info.timestamp);
    video_info.p_data = reinterpret_cast<u8*>(info.p_data);
    video_info.details.video.aspect_ratio = info.details.video.aspect_ratio;
    video_info.details.video.width = info.details.video.width;
    video_info.details.video.height = info.details.video.height;
    return true;
}

bool AvPlayerSource::GetVideoData(AvPlayerFrameInfoEx& video_info) {
    if (!IsActive() || m_is_paused) {
        return false;
    }

    if (m_video_frames.Size() == 0) {
        return false;
    }

    const auto clock_ms = PlaybackClockMillis();
    const auto& new_frame = m_video_frames.Front();
    if (m_state.GetSyncMode() == AvPlayerAvSyncMode::Default) {
        if (new_frame.info.timestamp > clock_ms) {
            return false;
        }
    }

    if (m_current_video_frame.has_value()) {
        m_video_buffers.Push(std::move(m_current_video_frame->buffer));
        m_current_video_frame.reset();
        m_video_buffers_cv.Notify();
    }

    auto frame = m_video_frames.Pop();
    video_info = frame->info;
    m_last_video_ts = frame->info.timestamp;

    m_current_video_frame = std::move(frame);
    return true;
}

bool AvPlayerSource::GetAudioData(AvPlayerFrameInfo& audio_info) {
    if (!IsActive() || m_is_paused) {
        return false;
    }

    if (m_audio_frames.Size() == 0) {
        return false;
    }

    if (m_current_audio_frame.has_value()) {
        m_audio_buffers.Push(std::move(m_current_audio_frame->buffer));
        m_current_audio_frame.reset();
        m_audio_buffers_cv.Notify();
    }

    auto frame = m_audio_frames.Pop();
    m_last_audio_ts = frame->info.timestamp;
    m_last_data_time = std::chrono::high_resolution_clock::now();
    m_pause_duration = std::chrono::high_resolution_clock::duration(0);

    audio_info = {};
    audio_info.timestamp = frame->info.timestamp;
    audio_info.p_data = reinterpret_cast<u8*>(frame->info.p_data);
    audio_info.details.audio.sample_rate = frame->info.details.audio.sample_rate;
    audio_info.details.audio.size = frame->info.details.audio.size;
    audio_info.details.audio.channel_count = frame->info.details.audio.channel_count;
    m_current_audio_frame = std::move(frame);
    return true;
}

u64 AvPlayerSource::DurationMillis() const {
    if (m_avformat_context == nullptr) {
        return 0;
    }

    u64 duration = 0;
    const auto add_stream_duration = [&](std::optional<s32> stream_index) {
        if (!stream_index.has_value()) {
            return;
        }
        if (stream_index.value() < 0) {
            return;
        }
        const auto index = u32(stream_index.value());
        if (index >= m_avformat_context->nb_streams) {
            return;
        }
        const auto stream = m_avformat_context->streams[index];
        if (stream == nullptr) {
            return;
        }
        duration = std::max(duration, StreamDurationMillis(*m_avformat_context, *stream));
    };

    add_stream_duration(m_video_stream_index);
    add_stream_duration(m_audio_stream_index);

    if (duration == 0) {
        for (u32 index = 0; index < m_avformat_context->nb_streams; ++index) {
            const auto stream = m_avformat_context->streams[index];
            if (stream != nullptr) {
                duration = std::max(duration, StreamDurationMillis(*m_avformat_context, *stream));
            }
        }
    }

    return duration;
}

u64 AvPlayerSource::CurrentTime() {
    if (!m_start_time.has_value()) {
        return 0;
    }
    // psport: libSceAvPlayer reports the last handed out frame's timestamp whenever a video stream
    // is enabled, audio or not, and it may pass the container duration (kb #771).
    if (m_video_stream_index) {
        return m_last_video_ts;
    }

    if (m_is_eof && !IsActive()) {
        return m_duration;
    }
    if (!IsActive()) {
        return 0;
    }
    return PlaybackClockMillis();
}

u64 AvPlayerSource::PlaybackClockMillis() {
    if (!m_start_time.has_value()) {
        return 0;
    }

    using namespace std::chrono;
    const auto now = m_is_paused.load() ? m_pause_time : high_resolution_clock::now();
    if (m_audio_stream_index) {
        const auto last_audio = m_last_data_time.value_or(now);
        const auto elapsed =
            duration_cast<milliseconds>(now - last_audio - m_pause_duration).count();
        return m_last_audio_ts.value_or(0) + elapsed;
    }

    const auto elapsed =
        duration_cast<milliseconds>(now - m_start_time.value() - m_pause_duration).count();
    return elapsed > 0 ? u64(elapsed) : 0;
}

bool AvPlayerSource::IsActive() {
    return !m_is_eof || m_audio_packets.Size() != 0 || m_video_packets.Size() != 0 ||
           m_video_frames.Size() != 0 || m_audio_frames.Size() != 0;
}

void AvPlayerSource::ReleaseAVPacket(AVPacket* packet) {
    if (packet != nullptr) {
        av_packet_free(&packet);
    }
}

void AvPlayerSource::ReleaseAVFrame(AVFrame* frame) {
    if (frame != nullptr) {
        av_frame_free(&frame);
    }
}

void AvPlayerSource::ReleaseAVCodecContext(AVCodecContext* context) {
    if (context != nullptr) {
        avcodec_free_context(&context);
    }
}

void AvPlayerSource::ReleaseSWRContext(SwrContext* context) {
    if (context != nullptr) {
        swr_free(&context);
    }
}

void AvPlayerSource::ReleaseSWSContext(SwsContext* context) {
    if (context != nullptr) {
        sws_freeContext(context);
    }
}

void AvPlayerSource::ReleaseAVFormatContext(AVFormatContext* context) {
    if (context != nullptr) {
        avformat_close_input(&context);
    }
}

void AvPlayerSource::DemuxerThread(std::stop_token stop) {
    using namespace std::chrono;
    Common::SetCurrentThreadName("shadPS4:AvDemuxer");

    if (!m_audio_stream_index.has_value() && !m_video_stream_index.has_value()) {
        LOG_WARNING(Lib_AvPlayer, "Could not start DEMUXER thread. No streams enabled.");
        return;
    }
    LOG_INFO(Lib_AvPlayer, "Demuxer Thread started");

    bool read_since_loop = false;
    while (!stop.stop_requested()) {
        if (m_video_packets.Size() > 30 &&
            (!m_audio_stream_index.has_value() || m_audio_packets.Size() > 8)) {
            std::this_thread::sleep_for(milliseconds(5));
            continue;
        }
        AVPacketPtr up_packet(av_packet_alloc(), &ReleaseAVPacket);
        const auto res = av_read_frame(m_avformat_context.get(), up_packet.get());
        if (res < 0) {
            if (res == AVERROR_EOF) {
                // psport: a loop that yields no packet, or a seek that fails, would otherwise spin
                // this thread at full CPU forever; end the source as if it did not loop.
                if (m_is_looping && read_since_loop) {
                    LOG_INFO(Lib_AvPlayer, "EOF reached in demuxer. Looping the source...");
                    read_since_loop = false;
                    bool seeked = true;
                    m_state.OnWarning(ORBIS_AVPLAYER_ERROR_WAR_LOOPING_BACK);
                    // psport: no raw avio_seek to byte 0 first: the mov demuxer then reads the
                    // moov atom again as a new root, drops it as a duplicate and returns EOF
                    // forever, so a looping movie froze on its last frame (Atelier Ayesha).
                    if (m_video_stream_index.has_value()) {
                        const auto index = m_video_stream_index.value();
                        seeked &= avformat_seek_file(m_avformat_context.get(), index, 0, 0,
                                                     INT64_MAX, 0) >= 0;
                    }
                    if (m_audio_stream_index.has_value()) {
                        const auto index = m_audio_stream_index.value();
                        seeked &= avformat_seek_file(m_avformat_context.get(), index, 0, 0,
                                                     INT64_MAX, 0) >= 0;
                    }
                    if (seeked) {
                        continue;
                    }
                    LOG_WARNING(Lib_AvPlayer, "Could not seek back to the start. Ending the source.");
                    break;
                } else {
                    LOG_INFO(Lib_AvPlayer, "EOF reached in demuxer. Exiting.");
                    break;
                }
            } else {
                LOG_ERROR(Lib_AvPlayer, "Could not read AV frame: error = {}", res);
                m_state.OnError();
                return;
            }
            break;
        }
        read_since_loop = true;
        if (up_packet->stream_index == m_video_stream_index) {
            m_video_packets.Push(std::move(up_packet));
            m_video_packets_cv.Notify();
        } else if (up_packet->stream_index == m_audio_stream_index) {
            m_audio_packets.Push(std::move(up_packet));
            m_audio_packets_cv.Notify();
        }
    }

    m_is_eof = true;

    m_video_packets_cv.Notify();
    m_audio_packets_cv.Notify();
    m_video_frames_cv.Notify();
    m_audio_frames_cv.Notify();
    m_video_buffers_cv.Notify();
    m_audio_buffers_cv.Notify();

    m_video_decoder_thread.Join();
    m_audio_decoder_thread.Join();
    if (!stop.stop_requested()) {
        m_state.OnEOF();
    }

    LOG_INFO(Lib_AvPlayer, "Demuxer Thread exited normally");
    m_demuxer_thread.Join();
}

AvPlayerSource::AVFramePtr AvPlayerSource::ConvertVideoFrame(const AVFrame& frame) {
    auto nv12_frame = AVFramePtr{av_frame_alloc(), &ReleaseAVFrame};
    nv12_frame->best_effort_timestamp = frame.best_effort_timestamp;
    nv12_frame->pts = frame.pts;
    nv12_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    nv12_frame->format = AV_PIX_FMT_NV12;
    nv12_frame->width = frame.width;
    nv12_frame->height = frame.height;
    nv12_frame->sample_aspect_ratio = frame.sample_aspect_ratio;
    nv12_frame->crop_top = frame.crop_top;
    nv12_frame->crop_bottom = frame.crop_bottom;
    nv12_frame->crop_left = frame.crop_left;
    nv12_frame->crop_right = frame.crop_right;

    av_frame_get_buffer(nv12_frame.get(), 0);

    if (m_sws_context == nullptr) {
        m_sws_context =
            SWSContextPtr(sws_getContext(frame.width, frame.height, AVPixelFormat(frame.format),
                                         nv12_frame->width, nv12_frame->height, AV_PIX_FMT_NV12,
                                         SWS_FAST_BILINEAR, nullptr, nullptr, nullptr),
                          &ReleaseSWSContext);
    }
    const auto res = sws_scale(m_sws_context.get(), frame.data, frame.linesize, 0, frame.height,
                               nv12_frame->data, nv12_frame->linesize);
    if (res < 0) {
        LOG_ERROR(Lib_AvPlayer, "Could not convert to NV12: {}", av_err2str(res));
        return AVFramePtr{nullptr, &ReleaseAVFrame};
    }
    return nv12_frame;
}

s64 AvPlayerSource::EmptyEditDelay(size_t i_stream) const {
    return i_stream < m_empty_edit_delays.size() ? m_empty_edit_delays[i_stream] : 0;
}

static u64 FrameTimestampMillis(const AVFrame& frame, AVRational time_base, s64 empty_edit_delay) {
    auto timestamp = frame.best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame.pts;
    }
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame.pkt_dts;
    }
    if (timestamp == AV_NOPTS_VALUE || timestamp < 0 || time_base.den <= 0 ||
        timestamp > INT64_MAX - empty_edit_delay) {
        return 0;
    }

    const auto millis = av_rescale_q(timestamp + empty_edit_delay, time_base, AVRational{1, 1000});
    return millis > 0 ? u64(millis) : 0;
}

std::optional<Frame> AvPlayerSource::PrepareVideoFrame(GuestBuffer buffer, const AVFrame& frame) {
    if (frame.format != AV_PIX_FMT_NV12) {
        LOG_ERROR_COUNTED(Lib_AvPlayer, "Video frame in pixel format {} instead of NV12",
                          frame.format);
        return std::nullopt;
    }

    auto p_buffer = buffer.GetBuffer();
    if (!Videodec::CopyNV12Data(p_buffer, buffer.Size(), frame)) {
        LOG_ERROR_COUNTED(Lib_AvPlayer, "Video frame of {}x{} does not fit a {:#x}-byte buffer",
                          frame.width, frame.height, buffer.Size());
        return std::nullopt;
    }

    const auto stream_index = m_video_stream_index.value();
    const auto stream = m_avformat_context->streams[stream_index];
    const auto timestamp =
        FrameTimestampMillis(frame, stream->time_base, EmptyEditDelay(stream_index));

    const auto width = Common::AlignUp<u32>(frame.width, 16);
    const auto pitch = Common::AlignUp<u32>(frame.width, 64);
    const auto height = Common::AlignUp<u32>(frame.height, 16);
    Core::Memory::Instance()->InvalidateMemory(reinterpret_cast<VAddr>(p_buffer),
                                               (width * height * 3) / 2);

    return Frame{
        .buffer = std::move(buffer),
        .info =
            {
                .p_data = p_buffer,
                .timestamp = timestamp,
                .details =
                    {
                        .video =
                            {
                                .width = width,
                                .height = height,
                                .aspect_ratio = (float)av_q2d(frame.sample_aspect_ratio),
                                .crop_left_offset = u32(frame.crop_left),
                                .crop_right_offset = u32(frame.crop_right + (pitch - frame.width)),
                                .crop_top_offset = u32(frame.crop_top),
                                .crop_bottom_offset =
                                    u32(frame.crop_bottom + (height - frame.height)),
                                .pitch = pitch,
                                .luma_bit_depth = 8,
                                .chroma_bit_depth = 8,
                            },
                    },
            },
    };
}

void AvPlayerSource::VideoDecoderThread(std::stop_token stop) {
    using namespace std::chrono;
    Common::SetCurrentThreadName("shadPS4:AvVideoDecoder");

    LOG_INFO(Lib_AvPlayer, "Video Decoder Thread started");
    while ((!m_is_eof || m_video_packets.Size() != 0) && !stop.stop_requested()) {
        if (m_video_packets.Size() == 0 &&
            !m_video_packets_cv.Wait(stop, [this] { return m_video_packets.Size() != 0; })) {
            continue;
        }
        const auto packet = m_video_packets.Pop();
        if (!packet.has_value()) {
            continue;
        }

        auto res = avcodec_send_packet(m_video_codec_context.get(), packet->get());
        if (res < 0 && res != AVERROR(EAGAIN)) {
            m_state.OnError();
            LOG_ERROR(Lib_AvPlayer, "Could not send packet to the video codec. Error = {}",
                      av_err2str(res));
            return;
        }
        while (res >= 0) {
            if (m_video_buffers.Size() == 0 &&
                !m_video_buffers_cv.Wait(stop, [this] { return m_video_buffers.Size() != 0; })) {
                break;
            }
            auto up_frame = AVFramePtr(av_frame_alloc(), &ReleaseAVFrame);
            res = avcodec_receive_frame(m_video_codec_context.get(), up_frame.get());
            if (res < 0) {
                if (res == AVERROR_EOF) {
                    LOG_INFO(Lib_AvPlayer, "EOF reached in video decoder");
                    return;
                } else if (res != AVERROR(EAGAIN)) {
                    LOG_ERROR(Lib_AvPlayer,
                              "Could not receive frame from the video codec. Error = {}",
                              av_err2str(res));
                    m_state.OnError();
                    return;
                }
            } else {
                auto buffer = m_video_buffers.Pop();
                if (!buffer.has_value()) {
                    // Video buffers queue was cleared. This means that player was stopped.
                    break;
                }
                auto nv12_frame = AVFramePtr{nullptr, &ReleaseAVFrame};
                if (up_frame->format != AV_PIX_FMT_NV12) {
                    nv12_frame = ConvertVideoFrame(*up_frame);
                    if (nv12_frame == nullptr) {
                        m_state.OnError();
                        return;
                    }
                }
                auto video_frame = PrepareVideoFrame(std::move(buffer.value()),
                                                     nv12_frame ? *nv12_frame : *up_frame);
                if (!video_frame.has_value()) {
                    m_state.OnError();
                    return;
                }
                m_video_frames.Push(std::move(video_frame.value()));
                m_video_frames_cv.Notify();
            }
        }
    }

    LOG_INFO(Lib_AvPlayer, "Video Decoder Thread exited normally");
    m_video_decoder_thread.Join();
}

AvPlayerSource::AVFramePtr AvPlayerSource::ConvertAudioFrame(const AVFrame& frame) {
    auto pcm16_frame = AVFramePtr{av_frame_alloc(), &ReleaseAVFrame};
    pcm16_frame->best_effort_timestamp = frame.best_effort_timestamp;
    pcm16_frame->pts = frame.pts;
    pcm16_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    pcm16_frame->format = AV_SAMPLE_FMT_S16;
    pcm16_frame->ch_layout = frame.ch_layout;
    pcm16_frame->sample_rate = frame.sample_rate;

    if (m_swr_context == nullptr) {
        SwrContext* swr_context = nullptr;
        AVChannelLayout in_ch_layout = frame.ch_layout;
        AVChannelLayout out_ch_layout = frame.ch_layout;
        swr_alloc_set_opts2(&swr_context, &out_ch_layout, AV_SAMPLE_FMT_S16, frame.sample_rate,
                            &in_ch_layout, AVSampleFormat(frame.format), frame.sample_rate, 0,
                            nullptr);
        m_swr_context = SWRContextPtr(swr_context, &ReleaseSWRContext);
        swr_init(m_swr_context.get());
    }
    const auto res = swr_convert_frame(m_swr_context.get(), pcm16_frame.get(), &frame);
    if (res < 0) {
        LOG_ERROR(Lib_AvPlayer, "Could not convert to S16: {}", av_err2str(res));
        return AVFramePtr{nullptr, &ReleaseAVFrame};
    }
    return pcm16_frame;
}

std::optional<Frame> AvPlayerSource::PrepareAudioFrame(GuestBuffer buffer, const AVFrame& frame) {
    if (frame.format != AV_SAMPLE_FMT_S16) {
        LOG_ERROR_COUNTED(Lib_AvPlayer, "Audio frame in sample format {} instead of S16",
                          frame.format);
        return std::nullopt;
    }
    const auto size = u64(frame.ch_layout.nb_channels) * frame.nb_samples * sizeof(u16);
    if (frame.nb_samples > 1024 || size > buffer.Size()) {
        LOG_ERROR_COUNTED(Lib_AvPlayer,
                          "Audio frame of {} samples x {} channels does not fit a {:#x}-byte "
                          "buffer of up to 1024 samples",
                          frame.nb_samples, frame.ch_layout.nb_channels, buffer.Size());
        return std::nullopt;
    }

    auto p_buffer = buffer.GetBuffer();
    std::memcpy(p_buffer, frame.data[0], size);

    const auto stream_index = m_audio_stream_index.value();
    const auto stream = m_avformat_context->streams[stream_index];
    const auto timestamp =
        FrameTimestampMillis(frame, stream->time_base, EmptyEditDelay(stream_index));

    return Frame{
        .buffer = std::move(buffer),
        .info =
            {
                .p_data = p_buffer,
                .timestamp = timestamp,
                .details =
                    {
                        .audio =
                            {
                                .channel_count = u16(frame.ch_layout.nb_channels),
                                .sample_rate = u32(frame.sample_rate),
                                .size = u32(size),
                            },
                    },
            },
    };
}

void AvPlayerSource::AudioDecoderThread(std::stop_token stop) {
    using namespace std::chrono;
    Common::SetCurrentThreadName("shadPS4:AvAudioDecoder");

    LOG_INFO(Lib_AvPlayer, "Audio Decoder Thread started");
    while ((!m_is_eof || m_audio_packets.Size() != 0) && !stop.stop_requested()) {
        if (m_audio_packets.Size() == 0 &&
            !m_audio_packets_cv.Wait(stop, [this] { return m_audio_packets.Size() != 0; })) {
            continue;
        }
        const auto packet = m_audio_packets.Pop();
        if (!packet.has_value()) {
            continue;
        }
        auto res = avcodec_send_packet(m_audio_codec_context.get(), packet->get());
        if (res < 0 && res != AVERROR(EAGAIN)) {
            m_state.OnError();
            LOG_ERROR(Lib_AvPlayer, "Could not send packet to the audio codec. Error = {}",
                      av_err2str(res));
            return;
        }
        while (res >= 0) {
            if (m_audio_buffers.Size() == 0 &&
                !m_audio_buffers_cv.Wait(stop, [this] { return m_audio_buffers.Size() != 0; })) {
                break;
            }

            auto up_frame = AVFramePtr(av_frame_alloc(), &ReleaseAVFrame);
            res = avcodec_receive_frame(m_audio_codec_context.get(), up_frame.get());
            if (res < 0) {
                if (res == AVERROR_EOF) {
                    LOG_INFO(Lib_AvPlayer, "EOF reached in audio decoder");
                    return;
                } else if (res != AVERROR(EAGAIN)) {
                    m_state.OnError();
                    LOG_ERROR(Lib_AvPlayer,
                              "Could not receive frame from the audio codec. Error = {}",
                              av_err2str(res));
                    return;
                }
            } else {
                auto buffer = m_audio_buffers.Pop();
                if (!buffer.has_value()) {
                    // Audio buffers queue was cleared. This means that player was stopped.
                    break;
                }
                auto pcm16_frame = AVFramePtr{nullptr, &ReleaseAVFrame};
                if (up_frame->format != AV_SAMPLE_FMT_S16) {
                    pcm16_frame = ConvertAudioFrame(*up_frame);
                    if (pcm16_frame == nullptr) {
                        m_state.OnError();
                        return;
                    }
                }
                auto audio_frame = PrepareAudioFrame(std::move(buffer.value()),
                                                     pcm16_frame ? *pcm16_frame : *up_frame);
                if (!audio_frame.has_value()) {
                    m_state.OnError();
                    return;
                }
                m_audio_frames.Push(std::move(audio_frame.value()));
                m_audio_frames_cv.Notify();
            }
        }
    }

    LOG_INFO(Lib_AvPlayer, "Audio Decoder Thread exited normally");
    m_audio_decoder_thread.Join();
}

} // namespace Libraries::AvPlayer
