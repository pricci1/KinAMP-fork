// Isolated KinAMP prototype, 2026-10-12. GPL-3.0-or-later; see LICENSE.
#ifndef KINAMP_OPUS_MP4_H
#define KINAMP_OPUS_MP4_H

#include <cstdint>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

// Owns only the local MP4 demux/decode state, never the PCM FIFO or audio sink.
// FFmpeg handles dOps, gain, pre-skip and packet discard padding. All output is
// 48-kHz interleaved signed 16-bit stereo on the track's presentation timeline.
class OpusMp4 {
public:
    OpusMp4() = default;
    ~OpusMp4();
    OpusMp4(const OpusMp4&) = delete;
    OpusMp4& operator=(const OpusMp4&) = delete;
    bool open(const char* path);
    int seek(int64_t sample);
    // Positive: stereo frames in pcm; zero: EOF; negative: AVERROR.
    int read(std::vector<int16_t>& pcm);
    AVFormatContext* format() const { return fmt; }
    int64_t duration_samples() const { return duration; }

private:
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int stream = -1;
    bool draining = false;
    int64_t origin = 0;
    int64_t duration = 0;
    int64_t target = 0;
};
#endif
