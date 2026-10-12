#include <cassert>
#include "libk/sp805_watchdog.h"

using namespace libk::sp805;
struct Bus {
    u32 regs[1024]{};
    struct Write { u32 offset, value; } writes[512]{};
    u32 count = 0;
    bool unlocked = false;
    Bus() {
        regs[0xfe0 / 4] = 5; regs[0xfe4 / 4] = 0x18; regs[0xfe8 / 4] = 0x14;
        regs[kLoad / 4] = regs[kValue / 4] = 1000; regs[kControl / 4] = 3;
    }
    u32 read(u32 offset) { assert(offset % 4 == 0 && offset < sizeof(regs)); return regs[offset / 4]; }
    void write(u32 offset, u32 value) {
        assert(count < 512); writes[count++] = {offset, value};
        if (offset == kLock) { unlocked = value == kUnlock; return; }
        assert(unlocked && offset == kLoad);
        regs[kLoad / 4] = regs[kValue / 4] = value;
    }
    void advance(u32 ticks) {
        if (!(regs[kControl / 4] & 1)) return;
        if (regs[kValue / 4] > ticks) regs[kValue / 4] -= ticks;
        else { regs[kValue / 4] = 0; regs[kRawStatus / 4] = 1; }
    }
};
int main() {
    Bus active; const auto owner = probe(active);
    assert(owner.status == Status::Active);
    // Repeated keepalive prevents expiry and preserves the inherited policy.
    for (u32 n = 0; n < 120; ++n) {
        active.advance(900); assert(keepalive(active, owner));
        assert(active.read(kRawStatus) == 0 && active.read(kValue) == 1000);
        assert(active.read(kLoad) == owner.load && active.read(kControl) == owner.control && !active.unlocked);
        assert(active.writes[n * 3].offset == kLock && active.writes[n * 3].value == kUnlock);
        assert(active.writes[n * 3 + 1].offset == kLoad);
        assert(active.writes[n * 3 + 2].offset == kLock && active.writes[n * 3 + 2].value == 1);
    }
    Bus expired; expired.advance(1000);
    assert(probe(expired).status == Status::Expired);
    assert(!keepalive(expired, owner) && expired.count == 0);
    Bus unknown; unknown.regs[0xfe0 / 4] = 0;
    assert(probe(unknown).status == Status::Unsupported);
    assert(!keepalive(unknown, probe(unknown)) && unknown.count == 0);
    Bus off; off.regs[kControl / 4] = 0;
    assert(probe(off).status == Status::Off);
    assert(!keepalive(off, probe(off)) && off.count == 0);
    Bus foreign; foreign.regs[kLoad / 4] = 1001;
    assert(!keepalive(foreign, owner) && foreign.count == 0);
    foreign.regs[kLoad / 4] = owner.load; foreign.regs[kControl / 4] = 1;
    assert(!keepalive(foreign, owner) && foreign.count == 0);
    Bus reserved; reserved.regs[kControl / 4] = 7;
    assert(probe(reserved).status == Status::Unsupported);
    assert(!keepalive(reserved, probe(reserved)) && reserved.count == 0);
    Bus zero; zero.regs[kLoad / 4] = 0;
    assert(probe(zero).status == Status::Unsupported);
    assert(!keepalive(zero, probe(zero)) && zero.count == 0);
}
