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

// Preserve the rotating slot policy while examining at most 17 bitmap words,
// rather than scanning up to 1024 separate capability cache lines.
static u32 free_slot(const CapTable& table) {
    if (table.used_count == kCapTableSize) return kCapTableSize;
    const u32 start = table.next_free;
    const u32 first = start / 64, bit = start % 64;
    for (u32 step = 0; step <= kCapBitmapWords; ++step) {
        const u32 word = (first + step) % kCapBitmapWords;
        u64 free = ~table.occupied[word];
        if (!step) free &= ~0ull << bit;
        else if (step == kCapBitmapWords) free &= ~(~0ull << bit);
        if (free) return word * 64 + __builtin_ctzll(free);
    }
    return kCapTableSize;
}

static CapHandle alloc_locked(CapTable& table, CapType type, void* target, u32 rights,
                              object::Record* cached = nullptr) {
    if (!target || type == CAP_NULL) {
        return kNullCap;
    }
    rights = sanitize_rights(type, rights);
    if (rights == 0) {
        return kNullCap;
    }

    {
        const u32 idx = free_slot(table);
        if (idx == kCapTableSize) return kNullCap;
        CapEntry& e = table.entries[idx];
        ANKO_ASSERT(e.type_tag == CAP_NULL);

        u32 gen = e.generation;
        if (gen == 0) {
            gen = 1;
        }

        object::Record* record = cached;
        if (record) object::retain(record);
        else record = object::acquire(target);
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
        table.occupied[idx / 64] |= 1ull << (idx % 64);
        ++table.used_count;
        return cap_make(idx, gen);
    }
}

void init() {
    // TODO: nothing global needed for V0 — cap tables are per-process heap objects
}

// Allocate a new slot in a process's cap table.
// Returns the new handle, or kNullCap if the table is full.
CapHandle alloc(CapTable& table, CapType type, void* target, u32 rights) {
    return alloc(table, type, target, rights, nullptr);
}

CapHandle alloc(CapTable& table, CapType type, void* target, u32 rights, object::Record* record) {
    kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
    return alloc_locked(table, type, target, rights, record);
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
        table.occupied[idx / 64] &= ~(1ull << (idx % 64));
        --table.used_count;
    }
    object::release(record);
}

static void clear_matching(CapTable& table, void* target, bool all) {
    // A bounded pointer batch keeps the kernel stack small. Detach one bitmap
    // word under the lock, then run destructors outside it: callbacks may take
    // other table locks or allocate capabilities themselves.
    for (u32 word = 0; word < kCapBitmapWords; ++word) {
        object::Record* records[64];
        u32 count = 0;
        {
            kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
            u64 occupied = table.occupied[word];
            while (occupied) {
                const u32 bit = __builtin_ctzll(occupied);
                occupied &= occupied - 1;
                CapEntry& e = table.entries[word * 64 + bit];
                if (!all && e.target != target) continue;
                records[count++] = e.object;
                e.generation = next_generation(e.generation);
                clear_entry(e);
                table.occupied[word] &= ~(1ull << bit);
                --table.used_count;
            }
        }
        for (u32 i = 0; i < count; ++i) object::release(records[i]);
    }
}

void revoke_target(CapTable& table, void* target) {
    if (target) clear_matching(table, target, false);
}

void clear(CapTable& table) {
    clear_matching(table, nullptr, true);
}

bool reserve_slots(CapTable& table, u32 count) {
    if (count == 0) {
        return true;
    }
    kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
    return count <= kCapTableSize - table.used_count;
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
    return alloc_locked(table, static_cast<CapType>(src_e->type_tag), src_e->target,
                         new_rights, src_e->object);
}

} // namespace cap
