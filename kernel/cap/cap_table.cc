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

namespace cap {

void init() {
    // TODO: nothing global needed for V0 — cap tables are per-process heap objects
}

// Allocate a new slot in a process's cap table.
// Returns the new handle, or kNullCap if the table is full.
CapHandle alloc(CapTable& table, CapType type, void* target, u32 rights) {
    for (u32 i = 0; i < kCapTableSize; ++i) {
        const u32 idx = (table.next_free + i) % kCapTableSize;
        CapEntry& e = table.entries[idx];
        if (e.type_tag != CAP_NULL) continue;

        u32 gen = e.generation;
        if (gen == 0) gen = 1;

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

// Look up a handle. Returns the entry pointer, or nullptr if invalid/revoked.
CapEntry* lookup(CapTable& table, CapHandle handle, CapType expected_type) {
    CapEntry* e = lookup_any(table, handle);
    if (!e) return nullptr;
    if (e->type_tag != static_cast<u16>(expected_type)) return nullptr;
    return e;
}

CapEntry* lookup_any(CapTable& table, CapHandle handle) {
    const u32 idx = cap_index(handle);
    if (idx >= kCapTableSize) return nullptr;

    CapEntry& e = table.entries[idx];
    if (e.type_tag == CAP_NULL) return nullptr;
    if (e.generation != cap_generation(handle)) return nullptr;
    if (e.generation == 0) return nullptr;
    if (!e.target) return nullptr;
    return &e;
}

static u32 next_generation(u32 g) {
    ++g;
    if (g == 0) g = 1;
    return g;
}

static void clear_entry(CapEntry& e) {
    e.target = nullptr;
    e.rights_mask = 0;
    e.type_tag = CAP_NULL;
    e.flags = 0;
}

// Revoke a capability. Increments the generation so all existing handles are stale.
void revoke(CapTable& table, CapHandle handle) {
    const u32 idx = cap_index(handle);
    if (idx >= kCapTableSize) return;

    CapEntry& e = table.entries[idx];
    if (e.generation != cap_generation(handle)) return;
    if (e.type_tag == CAP_NULL) return;

    e.generation = next_generation(e.generation);
    clear_entry(e);
}

// Duplicate a capability with a reduced rights mask (spec sys_cap_dup).
CapHandle dup(CapTable& table, CapHandle src, u32 rights_mask) {
    CapEntry* src_e = lookup_any(table, src);
    if (!src_e) return kNullCap;
    if ((src_e->rights_mask & RIGHT_DUP) == 0) return kNullCap;

    const u32 new_rights = src_e->rights_mask & rights_mask;
    if (new_rights == 0) return kNullCap;
    return alloc(table,
                 static_cast<CapType>(src_e->type_tag),
                 src_e->target,
                 new_rights);
}

} // namespace cap
