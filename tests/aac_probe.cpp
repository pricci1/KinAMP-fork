// GPL-3.0-or-later. Calls the unchanged upstream MP4/FAAD parser without a sink.
#include <cstdio>
extern "C" {
#include "mpeg4/mp4read.h"
#include "faad_compat.h"
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (mp4read_open(argv[1]) != 0) {
        std::fprintf(stderr, "Decoder: Failed to open file with mp4read\n");
        return 3;
    }
    NeAACDecHandle decoder = NeAACDecOpen();
    unsigned long rate = 0;
    unsigned char channels = 0;
    int result = NeAACDecInit2(decoder, mp4config.asc.buf, mp4config.asc.size, &rate, &channels);
    std::printf("AAC rate=%lu channels=%u result=%d\n", rate, channels, result);
    NeAACDecClose(decoder);
    mp4read_close();
    return result < 0 ? 4 : 0;
}
