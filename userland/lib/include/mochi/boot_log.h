#pragma once
// Map the standard service log sink from named BootInfo entries.

#include "mochi/boot.h"
#include "mochi/log.h"
#include "midix/abi/ring.h"

namespace mochi {

static inline bool log_map_from_boot(const MxBootInfo* boot,
                                     mx_vaddr_t ring_va,
                                     const char* component) {
    const MxBootEntry* ring_entry =
        mochi::boot::find_type(boot, "log.ring", MX_BOOT_ENTRY_RING);
    const mx_handle_t log_db = mochi::boot::handle(boot, "log.doorbell");
    if (!ring_entry || ring_entry->handle == MX_NULL_HANDLE ||
        log_db == MX_NULL_HANDLE) {
        return false;
    }

    const uint32_t order = ring_entry->aux0
        ? static_cast<uint32_t>(ring_entry->aux0)
        : MX_DEFAULT_RING_ORDER;
    if (mx_map_page(ring_entry->handle, ring_va,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) {
        return false;
    }

    mochi::log_set_sink(reinterpret_cast<RingPage*>(ring_va),
                        order,
                        log_db,
                        component);
    return true;
}

} // namespace mochi
