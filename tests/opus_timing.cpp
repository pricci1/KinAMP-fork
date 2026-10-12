// Diagnostic only, 2026-10-12. GPL-3.0-or-later; see LICENSE.
// Emits bounded aggregates, never paths, tags, PCM or packet payloads.
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>
#include <time.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
#include <opus.h>
}

static volatile sig_atomic_t stopped = 0;
static void stop(int) { stopped = 1; }
static int64_t now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
static int interrupted(void* deadline) {
    return stopped || now() >= *static_cast<int64_t*>(deadline);
}
static void number(int64_t value) {
    if (value == AV_NOPTS_VALUE) std::printf("null");
    else std::printf("%lld", static_cast<long long>(value));
}

struct Histogram {
    // Bound memory/output independently of input size or timestamp diversity.
    std::map<int64_t, int64_t> bins;
    int64_t pairs = 0, sum = 0, nonzero = 0, unbinned = 0;
    int64_t minimum = 0, maximum = 0, first = AV_NOPTS_VALUE;
    void add(int64_t delta, int64_t index) {
        if (!pairs) minimum = maximum = delta;
        minimum = std::min(minimum, delta);
        maximum = std::max(maximum, delta);
        ++pairs;
        sum += delta;
        if (delta) {
            ++nonzero;
            if (first == AV_NOPTS_VALUE) first = index;
        }
        auto found = bins.find(delta);
        if (found != bins.end()) ++found->second;
        else if (bins.size() < 32) bins[delta] = 1;
        else ++unbinned;
    }
    void print() const {
        std::printf("{\"pairs\":%lld,\"sum_samples\":%lld,\"nonzero_pairs\":%lld,\"first_nonzero_index\":",
                    (long long)pairs, (long long)sum, (long long)nonzero);
        number(first);
        std::printf(",\"min_samples\":"); number(pairs ? minimum : AV_NOPTS_VALUE);
        std::printf(",\"max_samples\":"); number(pairs ? maximum : AV_NOPTS_VALUE);
        std::printf(",\"bins\":[");
        bool comma = false;
        for (const auto& bin : bins) {
            std::printf("%s{\"delta_samples\":%lld,\"pairs\":%lld}", comma ? "," : "",
                        (long long)bin.first, (long long)bin.second);
            comma = true;
        }
        std::printf("],\"unbinned_pairs\":%lld}", (long long)unbinned);
    }
};

struct Skip {
    int64_t records = 0, leading = 0, discard = 0;
    void add(const uint8_t* data, size_t size) {
        if (data && size >= 10) {
            ++records;
            leading += AV_RL32(data);
            discard += AV_RL32(data + 4);
        }
    }
    void print() const {
        std::printf("{\"records\":%lld,\"leading_samples\":%lld,\"discard_samples\":%lld}",
                    (long long)records, (long long)leading, (long long)discard);
    }
};

struct Resources {
    AVFormatContext* fmt = avformat_alloc_context();
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    ~Resources() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&fmt);
    }
};

static bool bound(const char* text, long maximum, long& value) {
    char* end = nullptr;
    errno = 0;
    value = std::strtol(text, &end, 10);
    return !errno && end != text && !*end && value > 0 && value <= maximum;
}

int main(int argc, char** argv) {
    long packet_limit = 100000, seconds = 30;
    bool decode = true;
    if (argc < 2 || argc > 5 ||
        (argc > 2 && !bound(argv[2], 1000000, packet_limit)) ||
        (argc > 3 && !bound(argv[3], 300, seconds)) ||
        (argc > 4 && std::strcmp(argv[4], "decode") && std::strcmp(argv[4], "packets"))) {
        std::fputs("Usage: opus-timing FILE [MAX_PACKETS=100000 [MAX_SECONDS=30 [decode|packets]]]\n"
                   "Limits: 1..1000000 packets, 1..300 seconds. Exit 0 complete, 10 bounded, 2 usage, 3 error.\n", stderr);
        return 2;
    }
    if (argc > 4) decode = !std::strcmp(argv[4], "decode");
    // Suppress FFmpeg diagnostics: they may include input paths or metadata.
    av_log_set_level(AV_LOG_QUIET);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    int64_t deadline = now() + static_cast<int64_t>(seconds) * 1000000000;
    Resources r;
    AVStream* st = nullptr;
    int stream = -1, error = 0;
    bool complete = false, demux_eof = false, drain_sent = false;
    const char* limit_reason = nullptr;
    int64_t read_packets = 0, packets = 0, frames = 0, decoded_samples = 0;
    int64_t toc_samples = 0, duration_samples = 0, invalid_toc = 0;
    int64_t missing_packet_pts = 0, missing_frame_pts = 0, discard_packets = 0;
    int64_t configs[32] = {}, modes[3] = {}, coded_stereo = 0;
    int64_t previous_pts = AV_NOPTS_VALUE, previous_duration = 0, previous_toc = -1;
    int64_t previous_frame_pts = AV_NOPTS_VALUE, previous_frame_samples = 0;
    Histogram packet_duration_delta, packet_toc_delta, frame_delta;
    Skip packet_skip, frame_skip;
    if (!r.fmt || !r.packet || !r.frame) error = AVERROR(ENOMEM);
    if (!error) {
        r.fmt->interrupt_callback = AVIOInterruptCB{interrupted, &deadline};
        AVDictionary* options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        av_dict_set(&options, "enable_drefs", "0", 0);
        error = avformat_open_input(&r.fmt, argv[1], av_find_input_format("mov"), &options);
        av_dict_free(&options);
    }
    // MOV supplies codec parameters and timing from its headers; avoid hidden
    // probe decoding in avformat_find_stream_info(), especially in packets mode.
    if (!error) {
        stream = av_find_best_stream(r.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (stream < 0) error = stream;
        else if (r.fmt->streams[stream]->codecpar->codec_id != AV_CODEC_ID_OPUS) error = AVERROR_INVALIDDATA;
        else st = r.fmt->streams[stream];
    }
    if (!error && decode) {
        const AVCodec* decoder = avcodec_find_decoder_by_name("libopus");
        r.codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
        if (!r.codec) error = AVERROR(ENOMEM);
        else {
            error = avcodec_parameters_to_context(r.codec, st->codecpar);
            r.codec->pkt_timebase = st->time_base;
            r.codec->thread_count = 1;
            if (!error) error = avcodec_open2(r.codec, nullptr, nullptr);
        }
    }
    while (!error) {
        if (interrupted(&deadline)) {
            limit_reason = stopped ? "signal" : "deadline";
            break;
        }
        if (decode) {
            int result = avcodec_receive_frame(r.codec, r.frame);
            if (result == AVERROR_EOF) { complete = true; break; }
            if (result >= 0) {
                int64_t pts = r.frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE) ++missing_frame_pts;
                else if (previous_frame_pts != AV_NOPTS_VALUE) {
                    frame_delta.add(av_rescale_q(pts - previous_frame_pts, st->time_base, AVRational{1, 48000})
                                    - previous_frame_samples, frames);
                }
                previous_frame_pts = pts;
                previous_frame_samples = r.frame->nb_samples;
                decoded_samples += r.frame->nb_samples;
                ++frames;
                AVFrameSideData* side = av_frame_get_side_data(r.frame, AV_FRAME_DATA_SKIP_SAMPLES);
                if (side) frame_skip.add(side->data, side->size);
                av_frame_unref(r.frame);
                continue;
            }
            if (result != AVERROR(EAGAIN) || drain_sent) {
                error = result == AVERROR(EAGAIN) ? AVERROR_INVALIDDATA : result;
                break;
            }
        }
        if (read_packets >= packet_limit) { limit_reason = "packets"; break; }
        int result = av_read_frame(r.fmt, r.packet);
        if (result == AVERROR_EOF) {
            demux_eof = true;
            if (!decode) { complete = true; break; }
            drain_sent = true;
            error = avcodec_send_packet(r.codec, nullptr);
            continue;
        }
        if (result < 0) { error = result; break; }
        ++read_packets;
        if (r.packet->stream_index == stream) {
            int count = opus_packet_get_nb_samples(r.packet->data, r.packet->size, 48000);
            if (count < 0) ++invalid_toc;
            else toc_samples += count;
            if (r.packet->size > 0) {
                int config = r.packet->data[0] >> 3;
                ++configs[config];
                ++modes[config < 12 ? 0 : config < 16 ? 1 : 2];
                if (r.packet->data[0] & 4) ++coded_stereo;
            }
            int64_t pts = r.packet->pts;
            if (pts == AV_NOPTS_VALUE) ++missing_packet_pts;
            else if (previous_pts != AV_NOPTS_VALUE) {
                int64_t delta = av_rescale_q(pts - previous_pts, st->time_base, AVRational{1, 48000});
                packet_duration_delta.add(delta - previous_duration, packets);
                if (previous_toc >= 0) packet_toc_delta.add(delta - previous_toc, packets);
            }
            previous_pts = pts;
            previous_duration = av_rescale_q(r.packet->duration, st->time_base, AVRational{1, 48000});
            previous_toc = count;
            duration_samples += previous_duration;
            if (r.packet->flags & AV_PKT_FLAG_DISCARD) ++discard_packets;
            size_t size = 0;
            const uint8_t* side = av_packet_get_side_data(r.packet, AV_PKT_DATA_SKIP_SAMPLES, &size);
            packet_skip.add(side, size);
            ++packets;
            if (decode) error = avcodec_send_packet(r.codec, r.packet);
        }
        av_packet_unref(r.packet);
    }
    if (error && interrupted(&deadline)) {
        limit_reason = stopped ? "signal" : "deadline";
        error = 0;
    }
    std::printf("{\"status\":\"%s\",\"limit_reason\":", error ? "error" : complete ? "complete" : "bounded");
    if (limit_reason) std::printf("\"%s\"", limit_reason); else std::printf("null");
    std::printf(",\"error\":%d,\"decode\":%s,\"demux_eof\":%s,\"limits\":{\"read_packets\":%ld,\"seconds\":%ld},\"stream\":",
                error, decode ? "true" : "false", demux_eof ? "true" : "false", packet_limit, seconds);
    if (st) {
        std::printf("{\"time_base\":[%d,%d],\"start_pts\":", st->time_base.num, st->time_base.den);
        number(st->start_time); std::printf(",\"duration_ts\":"); number(st->duration); std::printf("}");
    } else std::printf("null");
    std::printf(",\"packets\":{\"read_all_streams\":%lld,\"audio_packets\":%lld,\"toc_samples_total\":%lld,\"duration_samples_total\":%lld,\"invalid_toc\":%lld,\"missing_pts\":%lld,\"discard_flag_packets\":%lld,\"coded_stereo_packets\":%lld,\"toc_mode_counts\":{\"SILK\":%lld,\"hybrid\":%lld,\"CELT\":%lld},\"toc_config_counts\":[",
                (long long)read_packets, (long long)packets, (long long)toc_samples, (long long)duration_samples,
                (long long)invalid_toc, (long long)missing_packet_pts, (long long)discard_packets,
                (long long)coded_stereo, (long long)modes[0], (long long)modes[1], (long long)modes[2]);
    for (int i = 0; i < 32; ++i) std::printf("%s%lld", i ? "," : "", (long long)configs[i]);
    std::printf("],\"skip\":"); packet_skip.print();
    std::printf(",\"pts_delta_minus_duration\":"); packet_duration_delta.print();
    std::printf(",\"pts_delta_minus_toc\":"); packet_toc_delta.print(); std::printf("},\"frames\":");
    if (decode) {
        std::printf("{\"count\":%lld,\"nb_samples_total\":%lld,\"missing_pts\":%lld,\"skip\":",
                    (long long)frames, (long long)decoded_samples, (long long)missing_frame_pts);
        frame_skip.print(); std::printf(",\"pts_delta_minus_nb_samples\":"); frame_delta.print(); std::printf("}");
    } else std::printf("null");
    std::printf("}\n");
    return error ? 3 : complete ? 0 : 10;
}
