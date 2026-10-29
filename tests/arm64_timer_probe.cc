#include "anko/arm64_timer.h"
#include <cassert>
#include <cstdio>
int main() {
    arm64::PhysicalTimerAccess access{};
    const u64 base = (0x18ull << 26) | (1ull << 25) | (3ull << 20) |
                     (3ull << 14) | (14ull << 10) | (2ull << 1);
    for (u32 reg = 0; reg < 3; ++reg) for (u32 rt = 0; rt < 32; ++rt) {
        for (u32 read = 0; read < 2; ++read) {
            assert(arm64::decode_physical_timer(base | (u64{reg} << 17) | (u64{rt} << 5) | read, access));
            assert(access.reg == reg && access.rt == rt && access.read == bool(read));
        }
    }
    assert(!arm64::decode_physical_timer(base | (3ull << 17), access));
    assert(!arm64::decode_physical_timer(base ^ (1ull << 14), access));
    assert(!arm64::decode_physical_timer(base ^ (1ull << 10), access));
    assert(!arm64::decode_physical_timer(base ^ (1ull << 26), access));
    u64 ctl = 0, compare = 150;
    assert(arm64::physical_timer_read(1, ctl, compare, 200) == 0);
    arm64::physical_timer_write(1, ~0ull, 200, ctl, compare);
    assert(ctl == 3 && arm64::physical_timer_read(1, ctl, compare, 200) == 7);
    assert(arm64::physical_timer_read(1, ctl, compare, 100) == 3);
    arm64::physical_timer_write(1, 1, 200, ctl, compare);
    assert(arm64::physical_timer_read(1, ctl, compare, 200) == 5);
    // TVAL is signed 32-bit on write; its upper bits are not timer state.
    arm64::physical_timer_write(0, 0xffffffff, 100, ctl, compare);
    assert(compare == 99 && arm64::physical_timer_read(0, ctl, compare, 100) == 0xffffffff);
    arm64::physical_timer_write(0, 0x100000005ull, 100, ctl, compare);
    assert(compare == 105 && arm64::physical_timer_read(0, ctl, compare, 100) == 5);
    arm64::physical_timer_write(2, 0x123456789ull, 100, ctl, compare);
    assert(arm64::physical_timer_read(2, ctl, compare, 100) == 0x123456789ull);
    std::puts("physical timer emulation probe ok");
}
