#include <cassert>
#include <cstring>
#include "rpmb.h"

int main() {
    using namespace anne::rpmb;
    // Linux's 40-byte little-endian rpmb_config_info for the saved 4 MiB,
    // reliable-write profile. The 256-byte data count differs from the number
    // of 512-byte MMC transport frames (8192).
    constexpr unsigned char expected[40] = {
        0x00, 0x40, 0, 0, 0, 0, 0, 0,
        0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0,
        0x03, 0x00, 0x00, 0x80, 0, 0, 0, 0,
        0x00, 0x00, 0x02, 0x00, 0, 0, 0, 0,
        0x20, 0, 0, 0, 8, 0, 1, 1,
    };
    assert(std::memcmp(&kAnneConfig, expected, sizeof(expected)) == 0);
    auto legacy = emmc_config({1, 0, 0, {0, 0, 0, 0}});
    assert(legacy.total_blks == 512 && legacy.read_align == 1);
    assert(legacy.write_frame_support == 3 && legacy.region_size[0] == 1);
    auto multi = emmc_config({0x20, 0x10, 1, {8, 8, 8, 8}});
    assert(multi.region_enable == 15 && multi.read_align == 0);
    for (auto size : multi.region_size) assert(size == 8);
    auto sparse = emmc_config({255, 0xef, 0xfe, {0, 1, 0, 0}});
    assert(sparse.total_blks == 130560 && sparse.read_align == 1);
    assert(sparse.region_enable == 1 && sparse.region_size[0] == 0);
    auto zero = emmc_config({0, 0, 1, {0, 0, 0, 0}});
    assert(zero.total_blks == 0 && zero.region_enable == 15);

    constexpr u64 base = 0x34a21000, size = 0x10000;
    assert(contains(base, size, base, size));
    assert(contains(base, size, base + size - 8, 8));
    for (u64 overflow = 1; overflow <= 7; ++overflow)
        assert(!contains(base, size, base + size - 8 + overflow, 8));
    assert(!contains(base, size, base - 1, 8));
    assert(!contains(base, size, ~u64{0} - 7, 16));
    assert(!contains(base, size, base, ~u64{0}));
    assert(!contains(base, size, base + size, 1));
}
