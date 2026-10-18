#include "mochi/drivers/hi6523.h"
#include <cassert>
#include <cstdio>
using namespace mochi::hi6523;
struct Bus {
    mx_u8_t status = 0x40, faults = 0;
    unsigned calls = 0, fail_at = 0, feeds = 0, clears = 0;
    bool transaction(libk::const_bytes out, libk::bytes in) {
        assert(out.size == 1 && out.data[0] == 0x06 && in.size == 7);
        if (++calls == fail_at) return false;
        for (unsigned i = 0; i < in.size; ++i) in.data[i] = 0;
        in.data[0] = status;
        in.data[6] = faults;
        return true;
    }
    bool write(mx_u8_t reg, mx_u8_t value) {
        if (++calls == fail_at) return false;
        if (reg == 0x0c) {
            assert(value && !(value & ~0x1c));
            faults &= ~value;
            ++clears;
        } else {
            assert(reg == 0xe8 && value == 1);
            ++feeds;
        }
        return true;
    }
};
int main() {
    // Ramp-up UVP/SCP is latched while the actual boost output is now healthy.
    for (unsigned faults = 0; faults < 256; ++faults) {
        Bus bus;
        bus.faults = faults;
        const auto health = maintain_source(bus, true);
        assert(health.faults == faults && health.status == 0x40);
        if (faults & 0x03) {
            assert(health.failure == SourceFailure::HardFault && !bus.feeds && !bus.clears);
        } else {
            assert(health.ok() && bus.feeds == 1);
            assert(bus.clears == ((faults & 0x1c) ? 1u : 0u));
        }
        bus = {};
        bus.status = 0;
        bus.faults = faults;
        assert(maintain_source(bus, true).failure == SourceFailure::BoostStopped && !bus.feeds);
        bus = {};
        bus.faults = faults;
        const auto ongoing = maintain_source(bus);
        assert(ongoing.ok() == !(faults & 0x1f)); // Recurrence has no startup grace.
    }
    for (unsigned fail_at = 1; fail_at <= 3; ++fail_at) {
        Bus bus;
        bus.faults = 0x14;
        bus.fail_at = fail_at;
        const auto health = maintain_source(bus, true);
        const SourceFailure expected[] = {SourceFailure::Read, SourceFailure::Clear, SourceFailure::Feed};
        assert(health.failure == expected[fail_at - 1] && !bus.feeds);
    }
    Bus recovered;
    recovered.faults = 0x08; // Actual phone failure: healthy boost + latched OCP.
    assert(maintain_source(recovered, true).ok());
    for (unsigned second = 0; second < 60; ++second) assert(maintain_source(recovered).ok());
    assert(recovered.feeds == 61 && recovered.clears == 1 && !recovered.faults);
    recovered.faults = 0x08;
    assert(maintain_source(recovered).failure == SourceFailure::HardFault);
    std::puts("hi6523 source health probe ok");
}
