#define ANKO_HOST_TEST 1
#include "anko/random.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
extern "C" u64 anko_random_test_uptime() { static u64 ticks = 123456789; return ticks++; }
int main(int argc, char** argv) {
    unsigned char output[32]{};
    assert(!security::random_ready() && !security::random_bytes(output, sizeof(output)));
    assert(!security::random_below(4));
#ifdef ANKO_ALLOW_WEAK_ENTROPY
    if (argc < 2) {
        assert(security::seed_boot_random(0) && security::random_ready() && !security::random_trusted());
        assert(security::random_bytes(output, sizeof(output))); return 0;
    }
#endif
    assert(!security::seed_boot_random(0));
    if (argc < 2) return 0;
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> dtb{std::istreambuf_iterator<char>(file), {}};
    const bool seeded = security::seed_boot_random(reinterpret_cast<u64>(dtb.data()));
    if (argc > 2) { assert(!seeded && !security::random_ready()); return 0; }
    assert(seeded && security::random_ready() && security::random_trusted());
    assert(security::random_bytes(output, sizeof(output)));
    for (auto value : output) std::printf("%02x", value);
    std::puts("");
    for (unsigned n = 0; n < 10000; ++n) {
        assert(security::random_below(3) < 3);
        assert(security::random_below(0x8000000000000001ull) < 0x8000000000000001ull);
    }
    assert(!security::random_bytes(nullptr, 1));
}
