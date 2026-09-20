#pragma once
// userland/init/supervision.h — pure supervision policy.
//
// The steady-state supervisor (userland/init/main.cc) and the restart
// regression fixture (tests/kernel/restart.cc) share these decisions. Keeping
// them here — free of capabilities and syscalls — makes the restart, backoff,
// dependency-closure, and exit-matching policy a single definition that the
// fixture can assert directly instead of duplicating as folklore.

#include <cstdint>

namespace supervision {

// A crash loop backs off exponentially. The shift saturates so the delay stays
// bounded (10 ms .. 1.28 s) no matter how many consecutive failures occur.
constexpr uint32_t kFailureCap = 7;
constexpr uint64_t kBaseBackoffNs = 10'000'000ull;

constexpr uint64_t backoff_ns(uint32_t failures) {
    return kBaseBackoffNs << (failures < kFailureCap ? failures : kFailureCap);
}

// What the supervisor should do when one incarnation reports an exit.
//   planned   — init killed it deliberately (dependency restart/stop/reboot)
//   desired   — the manifest or a control request still wants the service up
//   automatic — the manifest restart policy is "always"
//   pending   — a restart was already requested for this service
struct RestartDecision {
    bool restart;      // respawn the service
    bool failed;       // count this exit toward the crash-loop backoff
    uint64_t delay_ns; // 0 => eligible immediately
};

constexpr RestartDecision restart_decision(bool planned, bool desired,
                                           bool automatic, bool pending,
                                           uint32_t failures) {
    // A deliberate stop is policy, not a crash: relaunch promptly (or not at
    // all) and never charge it to the backoff counter.
    if (planned) return {desired, false, 0};
    if (!desired || !(automatic || pending)) return {false, false, 0};
    const uint32_t next = failures < kFailureCap ? failures + 1 : failures;
    return {true, true, backoff_ns(next)};
}

// Bitmask of the services that (transitively) depend on `mask`, given each
// service's own dependency bitmask. The closure is monotone, so `count` passes
// always converge it for a cycle-free graph.
inline uint32_t dependent_closure(const uint32_t* dependencies, uint32_t count,
                                  uint32_t mask) {
    for (uint32_t pass = 0; pass < count; ++pass) {
        for (uint32_t i = 0; i < count; ++i) {
            if (dependencies[i] & mask) mask |= 1u << i;
        }
    }
    return mask;
}

// Index of the service whose generation-tagged identity matches an exit record,
// or `count` when the record belongs to no live service.
inline uint32_t match_identity(const uint64_t* identities, uint32_t count,
                               uint64_t identity) {
    for (uint32_t i = 0; i < count; ++i) {
        if (identities[i] && identities[i] == identity) return i;
    }
    return count;
}

} // namespace supervision
