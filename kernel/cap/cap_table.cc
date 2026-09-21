// kernel/cap/cap_table.cc — capability table operations.
//
// Spec ref: §5.5 Capability Tables, §4 Capabilities
//
// V0: dense flat array of CapEntry[kCapTableSize] per process.
// Lookup: extract index from handle, check generation — O(1).
// Revocation: increment generation in the entry, handle becomes stale — O(1).
//
// Safety rules from spec §11:
//   - No ambient authority.
//   - No device access without a capability.
//   - No kernel object accessible without a valid handle.

#include "anko/cap.h"
#include "anko/panic.h"
#include "anko/spinlock.h"

namespace cap {

// Capability operations rejected so far (invalid/revoked handles, type
// mismatches). Monotonic; surfaced by the panic report.
static u64 g_denials = 0;

static void note_denial() { __atomic_add_fetch(&g_denials, 1ull, __ATOMIC_RELAXED); }

u64 denial_count() { return __atomic_load_n(&g_denials, __ATOMIC_RELAXED); }

static u32 sanitize_rights(CapType type, u32 rights) {
    if ((rights & RIGHT_MAP) && (rights & RIGHT_WRITE) && (rights & RIGHT_EXEC)) {
        return 0;
    }
    switch (type) {
    case CAP_SHARED_RING:
    case CAP_ARENA:
    case CAP_UART:
    case CAP_MMIO:
    case CAP_IRQ:
        if (rights & RIGHT_EXEC) {
            return 0;
        }
        break;
    default:
        break;
    }
    return rights;
}

static u32 nospec_index(u32 idx, u32 limit) {
    const u32 in_range = idx < limit;
    return idx & (0u - in_range);
}

static CapHandle alloc_locked(CapTable& table, CapType type, void* target, u32 rights) {
    if (!target || type == CAP_NULL) {
        return kNullCap;
    }
    rights = sanitize_rights(type, rights);
    if (rights == 0) {
        return kNullCap;
    }

    for (u32 i = 0; i < kCapTableSize; ++i) {
        const u32 idx = (table.next_free + i) % kCapTableSize;
        CapEntry& e = table.entries[idx];
        if (e.type_tag != CAP_NULL) {
            continue;
        }

        u32 gen = e.generation;
        if (gen == 0) {
            gen = 1;
        }

        object::Record* record = object::acquire(target);
        if (!record) {
            return kNullCap;
        }
        e.object = record;
        e.target = target;
        e.rights_mask = rights;
        e.generation = gen;
        e.type_tag = static_cast<u16>(type);
        e.flags = 0;

        table.next_free = (idx + 1) % kCapTableSize;
        return cap_make(idx, gen);
    }
    return kNullCap;
}

void init() {
    // TODO: nothing global needed for V0 — cap tables are per-process heap objects
}

// Allocate a new slot in a process's cap table.
// Returns the new handle, or kNullCap if the table is full.
CapHandle alloc(CapTable& table, CapType type, void* target, u32 rights) {
    kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
    return alloc_locked(table, type, target, rights);
}

// Validation and retention happen together under the table lock. No caller
// receives a pointer into mutable table storage.
Ref lookup(CapTable& table, CapHandle handle, CapType expected_type) {
    Ref e = lookup_any(table, handle);
    if (!e || e->type_tag != expected_type) {
        note_denial();
        return nullptr;
    }
    return e;
}

Ref lookup_any(CapTable& table, CapHandle handle) {
    kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
    const u32 idx = cap_index(handle);
    if (idx >= kCapTableSize) {
        note_denial();
        return nullptr;
    }
    const CapEntry& e = table.entries[nospec_index(idx, kCapTableSize)];
    if (e.type_tag == CAP_NULL || !e.generation || e.generation != cap_generation(handle)) {
        note_denial();
        return nullptr;
    }
    ANKO_ASSERT(e.target && e.object);
    return Ref(e);
}

static u32 next_generation(u32 g) {
    ++g;
    if (g == 0) {
        g = 1;
    }
    return g;
}

static void clear_entry(CapEntry& e) {
    e.target = nullptr;
    e.object = nullptr;
    e.rights_mask = 0;
    e.type_tag = CAP_NULL;
    e.flags = 0;
}

// Revoke a capability. Increments the generation so all existing handles are stale.
void revoke(CapTable& table, CapHandle handle) {
    object::Record* record = nullptr;
    {
        kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
        const u32 idx = cap_index(handle);
        if (idx >= kCapTableSize) {
            return;
        }
        CapEntry& e = table.entries[nospec_index(idx, kCapTableSize)];
        if (e.generation != cap_generation(handle) || e.type_tag == CAP_NULL) {
            return;
        }
        record = e.object;
        e.generation = next_generation(e.generation);
        clear_entry(e);
    }
    object::release(record);
}

void revoke_target(CapTable& table, void* target) {
    if (!target) {
        return;
    }
    for (u32 i = 0; i < kCapTableSize; ++i) {
        object::Record* record = nullptr;
        {
            kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
            CapEntry& e = table.entries[i];
            if (e.type_tag == CAP_NULL || e.target != target) {
                continue;
            }
            record = e.object;
            e.generation = next_generation(e.generation);
            clear_entry(e);
        }
        object::release(record);
    }
}

void clear(CapTable& table) {
    for (u32 i = 0; i < kCapTableSize; ++i) {
        CapHandle handle;
        {
            kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
            handle = cap_make(i, table.entries[i].generation);
        }
        revoke(table, handle);
    }
}

bool reserve_slots(CapTable& table, u32 count) {
    if (count == 0) {
        return true;
    }
    kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
    u32 free_count = 0;
    for (u32 i = 0; i < kCapTableSize; ++i) {
        if (table.entries[i].type_tag == CAP_NULL && ++free_count >= count) {
            return true;
        }
    }
    return false;
}

// Duplicate a capability with a reduced rights mask (spec sys_cap_dup).
CapHandle dup(CapTable& table, CapHandle src, u32 rights_mask) {
    kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
    const u32 idx = cap_index(src);
    const u32 safe_idx = nospec_index(idx, kCapTableSize);
    if (idx >= kCapTableSize) {
        return kNullCap;
    }
    CapEntry* src_e = &table.entries[safe_idx];
    if (src_e->type_tag == CAP_NULL) {
        return kNullCap;
    }
    if (src_e->generation != cap_generation(src) || src_e->generation == 0) {
        return kNullCap;
    }
    ANKO_ASSERT(src_e->target != nullptr);
    if ((src_e->rights_mask & RIGHT_DUP) == 0) {
        return kNullCap;
    }

    const u32 new_rights = src_e->rights_mask & rights_mask;
    if (new_rights == 0) {
        return kNullCap;
    }
    return alloc_locked(table, static_cast<CapType>(src_e->type_tag), src_e->target, new_rights);
}

} // namespace cap
