#pragma once
// Process BootInfo helpers.
//
// BootInfo is the programmer-facing replacement for fixed cap-slot folklore.
// The only fixed slot is slot 0: a one-page MxBootInfo arena granted by init.
// Every other service capability is found by name and type from that page.

#include "libk/str.h"
#include "midix/abi/boot.h"
#include "midix/abi/cap.h"
#include "mochi/endpoint.h"
#include "mochi/va_layout.h"
#include "mochi/syscall.h"

namespace mochi::boot {

constexpr mx_vaddr_t kDefaultBootInfoVa = va::kBootInfo;

constexpr CapHandle cap_slot(uint32_t idx) {
    return (static_cast<CapHandle>(1) << 32) | idx;
}

constexpr CapHandle kBootInfoCap = cap_slot(0);

static inline uint32_t slen(const char* s) {
    return static_cast<uint32_t>(libk::str::length(s));
}

static inline bool streq(const char* a, const char* b) {
    return libk::str::equal(a, b);
}

static inline bool valid(const MxBootInfo* info) {
    if (!info || info->magic != MX_BOOT_MAGIC || info->version != MX_BOOT_VERSION ||
        info->entry_count > MX_BOOT_MAX_ENTRIES) {
        return false;
    }
    for (uint16_t i = 0; i < info->entry_count; ++i) {
        if (libk::str::length_n(info->entries[i].name, MX_BOOT_NAME_MAX) == MX_BOOT_NAME_MAX) {
            return false;
        }
        for (uint16_t j = 0; j < i; ++j) {
            if (streq(info->entries[i].name, info->entries[j].name)) {
                return false;
            }
        }
    }
    return true;
}

[[noreturn]] static inline void invalid_boot() {
    constexpr char message[] = "invalid or incompatible BootInfo\n";
    sys_debug_puts(message, sizeof(message) - 1);
    mx_exit(MX_ERR_BAD_PROTOCOL);
    while (true) {
        mx_yield();
    }
}

static inline MxBootInfo* map(mx_vaddr_t va = kDefaultBootInfoVa) {
    mx_i64_t type = 0;
    if (mx_cap_identify(kBootInfoCap, &type) != MX_OK || type != MX_CAP_ARENA ||
        mx_map_page(kBootInfoCap, va, MX_RIGHT_READ) != MX_OK) {
        invalid_boot();
    }
    auto* info = reinterpret_cast<MxBootInfo*>(va);
    if (!valid(info)) {
        invalid_boot();
    }
    return info;
}

static inline const MxBootEntry* find(const MxBootInfo* info, const char* name) {
    if (!valid(info) || !name) {
        return nullptr;
    }
    for (uint16_t i = 0; i < info->entry_count; ++i) {
        if (streq(info->entries[i].name, name)) {
            return &info->entries[i];
        }
    }
    return nullptr;
}

static inline const MxBootEntry* find_type(const MxBootInfo* info, const char* name,
                                           MxBootEntryType type) {
    const MxBootEntry* entry = find(info, name);
    if (!entry || entry->type != static_cast<mx_u32_t>(type)) {
        return nullptr;
    }
    return entry;
}

static inline mx_handle_t handle(const MxBootInfo* info, const char* name) {
    const MxBootEntry* entry = find(info, name);
    return entry ? entry->handle : MX_NULL_HANDLE;
}

static inline mx_handle_t require_handle(const MxBootInfo* info, const char* name) {
    mx_handle_t h = handle(info, name);
    if (h == MX_NULL_HANDLE) {
        invalid_boot();
    }
    return h;
}

// Map a named MX_BOOT_ENTRY_RING into this process at `va` and report its ring
// order (from the entry's aux0, or `default_order`). Returns the mapped page, or
// nullptr if the entry is absent or the map fails. Centralises the
// find_type + order + mx_map_page dance the console, tty and shell all repeat.
static inline RingPage* map_ring(const MxBootInfo* info, const char* name, mx_vaddr_t va,
                                 uint32_t default_order, uint32_t* order_out = nullptr) {
    const MxBootEntry* entry = find_type(info, name, MX_BOOT_ENTRY_RING);
    if (!entry || entry->handle == MX_NULL_HANDLE) return nullptr;
    const auto order = entry->aux0 ? entry->aux0 : default_order;
    if (order >= sizeof(mx_u32_t) * 8) return nullptr;
    if (mx_map_page(entry->handle, va, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) return nullptr;
    if (order_out) *order_out = static_cast<uint32_t>(order);
    return reinterpret_cast<RingPage*>(va);
}

// Declarative mapping inputs; addresses and protocol names remain service policy.
struct RingMapping {
    const char* name;
    mx_vaddr_t address;
    const char* doorbell;
    uint32_t default_order = MX_DEFAULT_RING_ORDER;
};

inline Ring map_ring(const MxBootInfo* info, const RingMapping& mapping) {
    Ring result{};
    result.doorbell = handle(info, mapping.doorbell);
    result.ring = map_ring(info, mapping.name, mapping.address,
                           mapping.default_order, &result.order);
    return result;
}

// Map both halves, requiring the one geometry SpscEndpoint represents. On
// partial failure release only mappings created here and return an empty view.
inline SpscEndpoint map_endpoint(const MxBootInfo* info, const RingMapping& tx_spec,
                                 const RingMapping& rx_spec) {
    if (tx_spec.address == rx_spec.address) return {};
    const Ring tx = map_ring(info, tx_spec);
    const Ring rx = map_ring(info, rx_spec);
    if (tx.valid() && rx.valid() && tx.order == rx.order && tx.doorbell && rx.doorbell) {
        return {tx.ring, rx.ring, tx.order, tx.doorbell, rx.doorbell};
    }
    if (tx.ring) (void)sys_unmap_page(tx_spec.address);
    if (rx.ring) (void)sys_unmap_page(rx_spec.address);
    return {};
}

// Services publish readiness only after their endpoints and local state exist.
static inline void ready(const MxBootInfo* info) {
    const auto db = handle(info, "service.ready");
    if (db != MX_NULL_HANDLE) (void)sys_notify(db);
}

} // namespace mochi::boot
