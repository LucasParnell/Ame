#pragma once
// Map the standard service log sink from named BootInfo entries.

#include "mochi/boot.h"
#include "mochi/log.h"
#include "midix/abi/ring.h"

namespace mochi {

static inline bool log_map_from_boot(const MxBootInfo* boot,
                                     mx_vaddr_t ring_va,
                                     const char* component) {
    const Ring log = boot::map_ring(boot, {"log.ring", ring_va, "log.doorbell"});
    if (!log.valid() || log.doorbell == MX_NULL_HANDLE) return false;
    log_set_sink(log.ring, log.order, log.doorbell, component);
    return true;
}

} // namespace mochi
