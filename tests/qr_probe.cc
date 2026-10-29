// tests/qr_probe.cc — host harness for kernel/qr.cc.
//
// Encodes a payload file and prints the module matrix so tests/test_qr.py can
// compare it, mask for mask, against the segno reference implementation.
//
// usage: qr_probe PAYLOAD_FILE LEVEL   (LEVEL in L M Q H)

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "anko/qr.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s PAYLOAD_FILE LEVEL\n", argv[0]);
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
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        data.insert(data.end(), buf, buf + n);
    }
    std::fclose(f);

    anko::qr::Level level = anko::qr::Level::L;
    switch (argv[2][0]) {
    case 'L': level = anko::qr::Level::L; break;
    case 'M': level = anko::qr::Level::M; break;
    case 'Q': level = anko::qr::Level::Q; break;
    case 'H': level = anko::qr::Level::H; break;
    default: std::fprintf(stderr, "bad level\n"); return 2;
    }

    static anko::qr::Code code;
    if (!anko::qr::encode(data.data(), static_cast<u32>(data.size()), level, code)) {
        std::fprintf(stderr, "encode failed (payload %zu bytes, level %c)\n",
                     data.size(), argv[2][0]);
        return 1;
    }

    std::printf("version=%u mask=%u size=%u\n", code.version, code.mask, code.size);
    for (u32 y = 0; y < code.size; ++y) {
        for (u32 x = 0; x < code.size; ++x) {
            std::putchar(anko::qr::dark(code, x, y) ? '1' : '0');
        }
        std::putchar('\n');
    }
    return 0;
}
