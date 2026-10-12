// Isolated KinAMP prototype, 2026-10-12. GPL-3.0-or-later; see LICENSE.
#include "opus_mp4.h"
#include <algorithm>

OpusMp4::~OpusMp4() {
    av_packet_free(&packet);
    av_frame_free(&frame);
    swr_free(&swr);
    avcodec_free_context(&codec);
    avformat_close_input(&fmt);
}

bool OpusMp4::open(const char* path) {
    // Probe bytes, not the filename. Even when linked to a general FFmpeg,
    // this path accepts only local MOV/MP4 with an Opus audio stream.
    AVIOContext* io = nullptr;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    int result = avio_open2(&io, path, AVIO_FLAG_READ, nullptr, &options);
    av_dict_free(&options);
    if (result < 0) return false;
    const AVInputFormat* input = nullptr;
    int probed = av_probe_input_buffer2(io, &input, path, nullptr, 0, 0);
    avio_closep(&io);
    if (probed < 0 || input != av_find_input_format("mov")) return false;

    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "enable_drefs", "0", 0);
    result = avformat_open_input(&fmt, path, input, &options);
    av_dict_free(&options);
    if (result < 0 || avformat_find_stream_info(fmt, nullptr) < 0) return false;
    stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream < 0 || fmt->streams[stream]->codecpar->codec_id != AV_CODEC_ID_OPUS) return false;

    AVStream* st = fmt->streams[stream];
    codec = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_OPUS));
    if (!codec || avcodec_parameters_to_context(codec, st->codecpar) < 0) return false;
    codec->pkt_timebase = st->time_base;
    if (avcodec_open2(codec, nullptr, nullptr) < 0) return false;
    if (codec->sample_rate != 48000) return false;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000,
                          &codec->ch_layout, codec->sample_fmt, 48000, 0, nullptr) < 0 ||
        swr_init(swr) < 0) return false;
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    origin = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
    if (st->duration != AV_NOPTS_VALUE) {
        duration = av_rescale_q(st->duration, st->time_base, AVRational{1, 48000});
    }
    return frame && packet;
}

int OpusMp4::seek(int64_t sample) {
    target = std::max<int64_t>(0, sample);
    // Decode at least 80 ms before the requested presentation sample. Seeking
    // directly to the packet containing the target loses Opus predictor state.
    int64_t preroll = std::max<int64_t>(0, target - 3840);
    int64_t timestamp = origin + av_rescale_q(preroll, AVRational{1, 48000}, fmt->streams[stream]->time_base);
    int result = av_seek_frame(fmt, stream, timestamp, AVSEEK_FLAG_BACKWARD);
    if (result < 0) return result;
    avcodec_flush_buffers(codec);
    swr_close(swr);
    result = swr_init(swr);
    av_packet_unref(packet);
    av_frame_unref(frame);
    draining = false;
    return result;
}

int OpusMp4::read(std::vector<int16_t>& pcm) {
    pcm.clear();
    while (true) {
        int result = avcodec_receive_frame(codec, frame);
        if (result == AVERROR_EOF) return 0;
        if (result == AVERROR(EAGAIN)) {
            if (draining) return AVERROR_INVALIDDATA;
            while (true) {
                result = av_read_frame(fmt, packet);
                if (result < 0 || packet->stream_index == stream) break;
                av_packet_unref(packet);
            }
            if (result == AVERROR_EOF) {
                draining = true;
                result = avcodec_send_packet(codec, nullptr);
            } else if (result >= 0) {
                result = avcodec_send_packet(codec, packet);
            }
            av_packet_unref(packet);
            if (result < 0) return result;
            continue;
        }
        if (result < 0) return result;
        if (frame->best_effort_timestamp == AV_NOPTS_VALUE) return AVERROR_INVALIDDATA;
        int64_t begin = av_rescale_q(frame->best_effort_timestamp - origin,
                                    fmt->streams[stream]->time_base, AVRational{1, 48000});
        pcm.resize(frame->nb_samples * 2);
        uint8_t* out = reinterpret_cast<uint8_t*>(pcm.data());
        int count = swr_convert(swr, &out, frame->nb_samples,
                                const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        av_frame_unref(frame);
        if (count < 0) return count;
        // MP4 edit lists may trim the last packet without discard side data.
        // Enforce the presentation duration, not the encoded packet duration.
        int skip = static_cast<int>(std::min<int64_t>(count, std::max<int64_t>(0, target - begin)));
        if (duration > 0) count = static_cast<int>(std::min<int64_t>(count, std::max<int64_t>(0, duration - begin)));
        if (count <= skip) continue;
        pcm.erase(pcm.begin(), pcm.begin() + skip * 2);
        pcm.resize((count - skip) * 2);
        return count - skip;
    }
}
