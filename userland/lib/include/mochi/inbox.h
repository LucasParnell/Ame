#pragma once
#include "mochi/ring.h"

namespace mochi {

// A compact column of incoming ring addresses, all notified through one
// doorbell. Arming an inbox touches only ring pointers and shared queue headers;
// message payloads, peer capabilities and endpoint reply state stay cold.
struct InboxWait {
    RingPage* const* rings;
    uint32_t count;
    uint32_t order;
    CapHandle doorbell;

    // Sample expected BEFORE inspecting non-ring events (exits or generation
    // changes). Their publishers notify unconditionally. Ring producers instead
    // claim the one-shot sleep flag: arm every input, recheck every head, then
    // compare-and-sleep in the kernel. Both kinds of wake survive that gap.
    // deadline == 0 means no timer; expiry is a normal wake to recheck policy.
    mx_status_t wait(uint64_t expected, uint64_t deadline = 0) const {
        if (!doorbell) return MX_ERR_INVALID;
        for (uint32_t i = 0; i < count; ++i) {
            if (!rings[i]) continue;
            auto& flags = rings[i]->flags;
            flags.doorbell_counter_snapshot = expected;
            // Pair RMWs with ring_publish(), rather than relying on a flag
            // store followed by an unrelated head load. If the producer's
            // exchange wins first, this acquire observes its publication; if
            // ours wins first, the producer sees 1 and notifies the doorbell.
            (void)__atomic_exchange_n(&flags.receiver_about_to_sleep, 1u, __ATOMIC_ACQ_REL);
        }
        bool pending = false;
        for (uint32_t i = 0; i < count; ++i)
            pending |= Ring{rings[i], order, doorbell}.depth() != 0;
        const auto status = pending ? MX_OK : mx_doorbell_wait_until(doorbell, expected, deadline);
        for (uint32_t i = 0; i < count; ++i)
            Ring{rings[i], order, doorbell}.clear_sleep();
        return status;
    }
};
} // namespace mochi
