#include "libk/counter_scale.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>

int main() {
    using u64 = std::uint64_t;
    libk::CounterScale scale;
    assert(scale.nanoseconds(12345) == 12345);
    u64 random = 0x91ac3b6d42ef0987;
    auto next = [&] { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
    auto check = [&](std::uint32_t frequency) {
        scale.configure(frequency);
        const u64 fixed[] = {0, 1, 2, 3, 63, 64, 65, 999999999, 1000000000,
                             ~u64{0}, ~u64{0} - 1, u64{1} << 63, (u64{1} << 63) - 1};
        auto compare = [&](u64 ticks) {
            const u64 expected = static_cast<u64>(static_cast<unsigned __int128>(ticks) * 1000000000 / frequency);
            assert(scale.nanoseconds(ticks) == expected);
        };
        for (auto ticks : fixed) compare(ticks);
        for (unsigned i = 0; i < 32; ++i) compare(next());
    };
    // Powers of two stress reciprocal underestimation and exact correction.
    for (unsigned bit = 0; bit < 32; ++bit) {
        check(std::uint32_t{1} << bit);
        if (bit) check((std::uint32_t{1} << bit) - 1);
    }
    for (auto frequency : {1920000u, 19200000u, 24000000u, 62500000u, 1000000000u, 4294967295u})
        check(frequency);
    for (std::uint32_t frequency = 1; frequency <= 10000; ++frequency) check(frequency);
    for (unsigned i = 0; i < 10000; ++i) check(static_cast<std::uint32_t>(next()) | 1);
    scale.configure(0); assert(scale.nanoseconds(~u64{0}) == ~u64{0});
}
