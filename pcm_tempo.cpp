// Speed prototype, 2026-10-12. GPL-3.0-or-later.
#include "pcm_tempo.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

PcmTempo::PcmTempo(std::atomic<bool>& stop) : stop(stop) {}
PcmTempo::~PcmTempo() { reset(0); }

void PcmTempo::reset(int64_t media_ns) {
    if (stream) sonicDestroyStream(stream);
    stream = nullptr;
    if (fd >= 0) ::close(fd);
    fd = -1;
    std::lock_guard<std::mutex> lock(mutex);
    epochs.clear();
    origin = media_ns;
    submitted_bytes = 0;
}

bool PcmTempo::new_epoch(double value) {
    active_speed = value;
    if (value != 1.0) {
        stream = sonicCreateStream(rate, channels);
        if (!stream) return false;
        sonicSetSpeed(stream, value);
        sonicSetPitch(stream, 1.0f);
        sonicSetRate(stream, 1.0f);
    }
    std::lock_guard<std::mutex> lock(mutex);
    int64_t output_start = 0, media_start = 0;
    if (!epochs.empty()) {
        const Epoch& prev = epochs.back();
        output_start = prev.output_start + prev.output;
        media_start = prev.media_start + prev.input;
    }
    epochs.push_back({output_start, media_start, 0, 0, value, false});
    return true;
}

bool PcmTempo::open(const char* fifo, int sample_rate, int channel_count) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        rate = sample_rate;
        channels = channel_count;
    }
    if (!new_epoch(speed.load())) return false;
    while (!stop && fd < 0) {
        fd = ::open(fifo, O_WRONLY | O_NONBLOCK);
        if (fd < 0 && errno != ENXIO && errno != EINTR) return false;
        if (fd < 0) usleep(10000);
    }
#ifdef F_SETPIPE_SZ
    // Linux otherwise queues up to 64 KiB (~340 ms at stereo 48 kHz).
    // Older target kernels may refuse; correctness does not depend on this
    // latency optimization because all queued bytes retain their epoch.
    if (fd >= 0) fcntl(fd, F_SETPIPE_SZ, 4096);
#endif
    return fd >= 0;
}

bool PcmTempo::emit(short* pcm, int frames) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        epochs.back().output += frames;
    }
    const char* bytes = reinterpret_cast<const char*>(pcm);
    size_t remaining = frames * channels * sizeof(short);
    while (!stop && remaining) {
        pollfd pfd = {fd, POLLOUT, 0};
        int ready = poll(&pfd, 1, 50);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        if (!ready) continue;
        ssize_t n = ::write(fd, bytes, remaining);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) return false;
        bytes += n;
        remaining -= n;
        std::lock_guard<std::mutex> lock(mutex);
        submitted_bytes += n;
    }
    return !stop;
}

bool PcmTempo::drain() {
    short pcm[2048 * 2];
    int n;
    while (!stop && (n = sonicReadShortFromStream(stream, pcm, 2048)) > 0) {
        if (!emit(pcm, n)) return false;
    }
    return !stop;
}

bool PcmTempo::close_epoch() {
    if (stream) {
        if (!sonicFlushStream(stream) || !drain()) return false;
        sonicDestroyStream(stream);
        stream = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex);
    epochs.back().closed = true;
    return true;
}

bool PcmTempo::write(short* pcm, int frames) {
    double requested = speed.load();
    if (requested != active_speed) {
        // Old buffered PCM must keep its old rate and precede the new epoch.
        if (!close_epoch() || !new_epoch(requested)) return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        epochs.back().input += frames;
    }
    if (!stream) return emit(pcm, frames); // exact 1x bypass
    return sonicWriteShortToStream(stream, pcm, frames) && drain();
}

bool PcmTempo::finish() {
    bool ok = !stop && close_epoch();
    // Stop/seek discard unplayed stretch buffers rather than draining them.
    if (stream) sonicDestroyStream(stream);
    stream = nullptr;
    if (fd >= 0) ::close(fd);
    fd = -1;
    return ok;
}

int64_t PcmTempo::position(int64_t output_ns) {
    std::lock_guard<std::mutex> lock(mutex);
    if (epochs.empty()) return origin;
    double frame = std::min(double(std::max<int64_t>(0, output_ns)) * rate / 1e9,
                            double(submitted_bytes / (channels * sizeof(short))));
    while (epochs.size() > 1 && frame >= epochs[1].output_start) epochs.pop_front();
    const Epoch& e = epochs.front();
    // Sonic's splice lengths are quantized to pitch periods. Once drained,
    // interpolate exact epoch endpoints; while open use its nominal rate.
    double ratio = e.closed && e.output ? double(e.input) / e.output : e.speed;
    double media = e.media_start + std::min(double(e.input),
                           std::max(0.0, frame - e.output_start) * ratio);
    return origin + int64_t(media * 1e9 / rate);
}
