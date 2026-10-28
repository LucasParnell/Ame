#include "../userland/dango/manager/render_clock.h"
#include <cassert>
#include <cstdio>

int main() {
    RenderClockLease clock;
    unsigned reads = 0, boosts = 0, restores = 0;
    mx_i64_t mhz = 360;
    auto rate = [&](mx_u32_t next) -> mx_i64_t {
        if (!next) {
            ++reads;
            return mhz;
        }
        if (next == 480) {
            ++boosts;
        } else {
            assert(next == 360);
            ++restores;
        }
        mhz = next;
        return 0;
    };
    for (unsigned frame = 0; frame < 300; ++frame) {
        clock.begin(frame * 16666666ull, rate);
    }
    assert(reads == 1 && boosts == 1 && restores == 0);
    clock.finish(rate);
    assert(restores == 1 && mhz == 360 && clock.deadline() == ~mx_u64_t{0});
    clock.finish(rate); // failure cleanup/end is idempotent
    assert(restores == 1);
    clock.begin(100, rate);
    clock.expire(5000000099ull, rate);
    assert(mhz == 480);
    clock.expire(5000000100ull, rate);
    assert(mhz == 360 && restores == 2);
    // Leave an existing higher OPP alone; never lower it on acquisition/release.
    mhz = 680;
    clock.begin(100, rate);
    clock.finish(rate);
    assert(mhz == 680 && boosts == 2 && restores == 2);
    // A failed frequency read cannot turn into an invented restore request.
    mhz = -1;
    clock.begin(100, rate);
    clock.finish(rate);
    assert(mhz == -1 && boosts == 2 && restores == 2);
    puts("Render clock lease probe passed");
}
