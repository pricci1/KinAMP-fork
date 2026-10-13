// Speed prototype, 2026-10-12. GPL-3.0-or-later.
#pragma once
#include "third_party/sonic/sonic.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

// Decoder-thread PCM output and media/output timeline; position() is called
// on the control thread with the sink's presented output time, NOT decode time.
class PcmTempo {
public:
    explicit PcmTempo(std::atomic<bool>& stop);
    ~PcmTempo();
    std::atomic<double> speed{1.0};
    void reset(int64_t media_ns);
    bool open(const char* fifo, int sample_rate, int channels);
    bool write(short* pcm, int frames);
    bool finish();
    int64_t position(int64_t output_ns);
private:
    struct Epoch {
        int64_t output_start, media_start, input, output;
        double speed;
        bool closed;
    };
    std::atomic<bool>& stop;
    sonicStream stream = nullptr;
    int fd = -1, rate = 48000, channels = 2;
    int64_t origin = 0, submitted_bytes = 0;
    double active_speed = 1.0;
    std::mutex mutex;
    std::deque<Epoch> epochs;
    bool drain();
    bool emit(short* pcm, int frames);
    bool close_epoch();
    bool new_epoch(double value);
};
