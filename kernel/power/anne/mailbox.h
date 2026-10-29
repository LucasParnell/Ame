#pragma once
#include <cstdint>
#include "libk/dma.h"

namespace anne::dvfs {
enum class Result { Done, Pending, Fault };
// Mailbox 13, AP little-cluster source bit 0 -> LPM3 destination bit 3.
// Auto-ACK unmasks only LPM3. AP completion is polled, never interrupt-routed.
// Caller serializes this one shared channel. No loop waits for firmware.
struct Mailbox {
    static constexpr unsigned kChannel = 13 * 64;
    bool pending = false, acknowledged = false, failed = false;
    unsigned cluster = 0, index = 0, khz = 0;
    std::uint64_t sent_ns = 0;
    unsigned completed[2]{}, failures[2]{};
    template <typename Io>
    Result request(Io& io, unsigned cpu_cluster, unsigned frequency, unsigned opp_index,
                   std::uint64_t now) {
        if (failed || cpu_cluster > 1 || !frequency || frequency % 1000 || opp_index > 15)
            return Result::Fault;
        if (pending) {
            unsigned mode = 0, source = 0;
            if (!io.read32(kChannel + 0x10, &mode))
                return fail();
            if (!acknowledged && (mode & 0x80)) {
                if (!io.read32(kChannel, &source) || source != 1)
                    return fail();
                // Clear our ACK and release only our completed transaction.
                if (!io.write32(kChannel + 0x18, 1) || !io.write32(kChannel + 0x14, ~0u) ||
                    !io.write32(kChannel, 1))
                    return fail();
                libk::dma::order_mmio();
                acknowledged = true;
            }
            if (acknowledged) {
                unsigned observed = 0;
                if (!io.frequency_index(cluster, observed))
                    return fail();
                if (observed == index) {
                    pending = false;
                    ++completed[cluster];
                    if (cluster == cpu_cluster && frequency == khz)
                        return Result::Done;
                }
            }
            if (pending) {
                if (now >= sent_ns && now - sent_ns >= 50000000)
                    return fail();
                return Result::Pending;
            }
        }
        cluster = cpu_cluster;
        unsigned mode = 0, source = 0, locked = 0, observed = 0;
        if (!io.frequency_index(cpu_cluster, observed))
            return fail();
        if (observed == opp_index)
            return Result::Done;
        if (!io.read32(kChannel + 0x10, &mode))
            return fail();
        // Never seize a channel in SOURCE/DEST state or another processor's ACK.
        if (mode & 0x80) {
            if (!io.read32(kChannel, &source) || source != 1)
                return Result::Pending;
            if (!io.write32(kChannel + 0x18, 1) || !io.write32(kChannel + 0x14, ~0u) ||
                !io.write32(kChannel, 1))
                return fail();
            libk::dma::order_mmio();
            return Result::Pending;
        }
        if (!(mode & 0x10))
            return Result::Pending;
        if (!io.write32(0xa00, 0x1acce551) || !io.read32(0xa00, &locked) || locked)
            return fail();
        if (!io.write32(kChannel, 1) || !io.read32(kChannel, &source))
            return fail();
        if (source != 1)
            return Result::Pending;
        if (!io.write32(kChannel + 0x14, ~8u) || !io.write32(kChannel + 4, 8) ||
            !io.write32(kChannel + 0x10, 1) ||
            !io.write32(kChannel + 0x20, ((cpu_cluster + 1) << 16) | 0x30a) ||
            !io.write32(kChannel + 0x24, frequency / 1000))
            return fail();
        libk::dma::order_mmio();
        if (!io.write32(kChannel + 0x1c, 1))
            return fail();
        libk::dma::order_mmio();
        pending = true;
        acknowledged = false;
        cluster = cpu_cluster;
        khz = frequency;
        index = opp_index;
        sent_ns = now;
        return Result::Pending;
    }

  private:
    Result fail() {
        failed = true;
        ++failures[cluster];
        return Result::Fault;
    }
};
} // namespace anne::dvfs
