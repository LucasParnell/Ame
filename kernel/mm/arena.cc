#include "anko/mm.h"
#include "anko/spinlock.h"
#include "anko/panic.h"

namespace mm {
namespace {
bool valid_range(VirtAddr base, u64 size) {
    return size && !((base | size) & (PAGE_SIZE - 1)) && base >= 0x10000000 && base < 0x40000000 &&
           size <= 0x40000000 - base;
}
int free_slot(ArenaTable& table) {
    for (u32 i = 0; i < kArenaTableSize; ++i) {
        if (!table.entries[i].size) {
            return i;
        }
    }
    return -1;
}
bool overlaps(ArenaTable& table, u64 base, u64 size) {
    for (const auto& a : table.entries) {
        if (a.size && a.base_va && base < a.base_va + a.size && a.base_va < base + size) {
            return true;
        }
    }
    return false;
}
} // namespace

void destroy_arena(void* target) {
    auto* arena = static_cast<Arena*>(target);
    if (arena->flags & kArenaFlagOwnsPages) {
        for (u64 off = 0; off < arena->size; off += PAGE_SIZE) {
            mm::free_page_for_identity((static_cast<u64>(arena->owner_generation) << 32) |
                                           arena->owner_pid,
                                       arena->phys_base + off);
        }
    }
    mm::slab_free(arena, sizeof(Arena), alignof(Arena));
}

bool arena_register(ArenaTable& table, VirtAddr base, PhysAddr pa, u64 size, u32 pid, u32 rights,
                    u32 flags, u32* index) {
    kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
    if (!valid_range(base, size) || (pa & (PAGE_SIZE - 1)) || overlaps(table, base, size)) {
        return false;
    }
    const int slot = free_slot(table);
    if (slot < 0) {
        return false;
    }
    auto& a = table.entries[slot];
    a = {};
    a.base_va = base;
    a.phys_base = pa;
    a.size = size;
    a.owner_pid = pid;
    a.rights = rights;
    a.flags = flags;
    if (static_cast<u32>(slot + 1) > table.count) {
        table.count = slot + 1;
    }
    if (index) {
        *index = slot;
    }
    return true;
}

bool arena_claim_va(ArenaTable& table, Arena& arena, VirtAddr base, u64 size, bool* fresh) {
    if (fresh) {
        *fresh = false;
    }
    if (size != arena.size) {
        return false;
    }
    {
        kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
        for (auto& a : table.entries) {
            if (a.size == size && a.base_va == base && a.phys_base == arena.phys_base) {
                return true;
            }
        }
    }
    if (!arena_register(table, base, arena.phys_base, size, arena.owner_pid, arena.rights,
                        arena.flags, nullptr)) {
        return false;
    }
    if (fresh) {
        *fresh = true;
    }
    return true;
}

void arena_retain_mapping(ArenaTable& table, VirtAddr base, object::Record* record) {
    kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
    for (u32 i = 0; i < table.count; ++i) {
        if (!table.entries[i].size || table.entries[i].base_va != base) {
            continue;
        }
        if (!table.backing[i]) {
            object::retain(record);
            table.backing[i] = record;
        } else {
            ANKO_ASSERT(table.backing[i] == record);
        }
        table.unmapped[i] = 0;
        return;
    }
    ANKO_ASSERT(false);
}

void arena_unregister(ArenaTable& table, u32 index) {
    object::Record* record = nullptr;
    {
        kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
        if (index >= table.count) {
            return;
        }
        record = table.backing[index];
        table.backing[index] = nullptr;
        table.entries[index] = {};
        table.unmapped[index] = 0;
        while (table.count && !table.entries[table.count - 1].size) {
            --table.count;
        }
    }
    object::release(record);
}

void arena_release_va(ArenaTable& table, Arena&, VirtAddr base) {
    u32 index = kArenaTableSize;
    {
        kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
        for (u32 i = 0; i < table.count; ++i) {
            if (table.entries[i].base_va == base) {
                index = i;
                break;
            }
        }
    }
    arena_unregister(table, index);
}

void arena_unmap_page(ArenaTable& table, VirtAddr va) {
    // All dynamically created user arenas contain at most 64 pages. Larger
    // platform DMA mappings are pinned until address-space destruction.
    u32 complete = kArenaTableSize;
    {
        kernel::LockGuard guard(*reinterpret_cast<kernel::SpinLock*>(&table.lock));
        for (u32 i = 0; i < table.count; ++i) {
            auto& a = table.entries[i];
            if (!a.size || va < a.base_va || va - a.base_va >= a.size) {
                continue;
            }
            const u64 pages = a.size / PAGE_SIZE;
            if (pages > 64) {
                break;
            }
            table.unmapped[i] |= 1ull << ((va - a.base_va) / PAGE_SIZE);
            const u64 all = pages == 64 ? ~0ull : (1ull << pages) - 1;
            if (table.unmapped[i] == all) {
                complete = i;
            }
            break;
        }
    }
    arena_unregister(table, complete);
}

void arena_clear(ArenaTable& table) {
    for (u32 i = 0; i < kArenaTableSize; ++i) {
        arena_unregister(table, i);
    }
}
} // namespace mm
