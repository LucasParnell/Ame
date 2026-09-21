// tests/lzma_probe.cc — host harness for kernel/lzma.cc.
//
// usage: lzma_probe INPUT_FILE OUTPUT_FILE
// Compresses INPUT_FILE and writes the raw LZMA1 stream to OUTPUT_FILE.
// Tests/test_lzma.py decodes it with Python's `lzma` (FORMAT_RAW, same filter
// parameters) and compares against the original.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "anko/lzma.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s INPUT OUTPUT\n", argv[0]);
        return 2;
    }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<unsigned char> data;
    unsigned char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);

    std::vector<unsigned char> out(anko::lzma::bound(static_cast<u32>(data.size())) + 16);
    const u32 written = anko::lzma::compress(data.data(), static_cast<u32>(data.size()),
                                             out.data(), static_cast<u32>(out.size()));
    if (written == 0) {
        std::fprintf(stderr, "compress failed (in=%zu cap=%zu)\n", data.size(), out.size());
        return 1;
    }
    std::FILE* g = std::fopen(argv[2], "wb");
    if (!g) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 2;
    }
    std::fwrite(out.data(), 1, written, g);
    std::fclose(g);
    std::printf("in=%zu out=%u\n", data.size(), written);
    return 0;
}
