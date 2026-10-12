// GPL-3.0-or-later. Offline validation entry point; no audio output.
#include "opus_mp4.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    OpusMp4 decoder;
    if (!decoder.open(argv[1])) return 3;
    int64_t target = std::strtoll(argv[2], nullptr, 10);
    if (decoder.seek(target) < 0) return 4;
    FILE* output = std::fopen(argv[3], "wb");
    if (!output) return 5;
    std::vector<int16_t> pcm;
    int count;
    int64_t frames = 0;
    while ((count = decoder.read(pcm)) > 0) {
        if (std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), output) != pcm.size()) return 6;
        frames += count;
    }
    std::fclose(output);
    std::printf("duration_samples=%lld target=%lld frames=%lld result=%d\n",
                (long long)decoder.duration_samples(), (long long)target, (long long)frames, count);
    return count < 0 ? 7 : 0;
}
