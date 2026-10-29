#include "anko/mali_shadow.h"
#include "../../boards/huawei-anne/gpu.h"

namespace mali {
namespace {
// Entry layout lives in anko/mali_shadow.h so the mediator cannot drift from
// the builder. The physical half always comes from the host translation, never
// from the guest entry, so a guest cannot name a physical page it holds no
// right to.
constexpr u64 kAddress = kPteAddressMask;
constexpr u64 kAttributes = kPteAttributeMask;
constexpr u64 kEntryWrite = kPteWrite;

u64* allocate(const ShadowOps& ops, Shadow& tree, u64& pa) {
    if (tree.count == kShadowMaxTables) {
        return nullptr;
    }
    const u32 index = tree.count++;
    if (index < tree.capacity) {
        ++tree.reused;
        pa = tree.physical[index];
        return tree.tables[index];
    }
    u64* table = ops.allocate(ops.ctx, &pa);
    if (!table) {
        --tree.count;
        return nullptr;
    }
    if (!pa || (pa & ~kAddress)) {
        ops.release(ops.ctx, table);
        --tree.count;
        return nullptr;
    }
    for (u32 i = 0; i < 512; ++i) {
        table[i] = 0;
    }
    tree.tables[index] = table;
    tree.physical[index] = pa;
    ++tree.capacity;
    // New zero tables must reach the GPU even if the guest table is empty.
    ops.publish(ops.ctx, table, 4096);
    ++tree.published;
    return table;
}
void store(u64& destination, u64 value, bool& dirty) {
    if (destination != value) {
        destination = value;
        dirty = true;
    }
}

bool leaf(const ShadowOps& ops, Shadow& tree, u64 entry, u64 guest, u64& result) {
    u64 host = 0;
    if (++tree.leaves > kShadowMaxLeaves ||
        !ops.translate(ops.ctx, guest, entry & kEntryWrite, &host) || !host || (host & ~kAddress)) {
        return false;
    }
    // Drop software bits, contiguous hints and all unrecognised attributes.
    result = host | (entry & kAttributes) | kPteLeaf;
    return true;
}

bool copy_table(const ShadowOps& ops, Shadow& tree, u64 guest, u32 level, u64* table) {
    // Four levels are required by Mali even for 32-bit VAs. Linux's root can
    // contain just one entry; never read the adjacent allocator object.
    bool dirty = false;
    const u32 entries = level == 0 ? 1 : level == 1 ? 4 : 512;
    const u64* source = ops.map_table ? ops.map_table(ops.ctx, guest) : nullptr;
    if (ops.map_table && !source) {
        return false;
    }
    for (u32 i = 0; i < entries; ++i) {
        u64 entry = 0;
        if (source) {
            entry = __atomic_load_n(source + i, __ATOMIC_ACQUIRE);
        } else if (!ops.read(ops.ctx, guest + i * 8, &entry)) {
            return false;
        }
        const u32 type = entry & 3;
        if (type == 0) {
            store(table[i], 0, dirty);
            continue;
        }
        const u64 address = entry & kAddress;
        // Linux may mark tables with SW_SYNC (bit 55); table flags are rebuilt.
        if (level < 3 && type == 3) {
            u64 pa = 0;
            u64* child = allocate(ops, tree, pa);
            if (!child || !copy_table(ops, tree, address, level + 1, child)) {
                return false;
            }
            store(table[i], pa | 3, dirty);
        } else if (level == 3 && type == kPteLeaf) {
            u64 value = 0;
            if (!leaf(ops, tree, entry, address, value)) {
                return false;
            }
            store(table[i], value, dirty);
        } else if (level == 2 && type == 1) {
            // Expand 2 MiB blocks: the VM's backing may be fragmented. Never
            // assume adjacent guest pages have adjacent host physical pages.
            if (address & ((1ULL << 21) - 1)) {
                return false;
            }
            u64 pa = 0;
            u64* child = allocate(ops, tree, pa);
            if (!child) {
                return false;
            }
            bool child_dirty = false;
            for (u32 p = 0; p < 512; ++p) {
                u64 value = 0;
                if (!leaf(ops, tree, entry, address + p * 4096ULL, value)) {
                    return false;
                }
                store(child[p], value, child_dirty);
            }
            if (child_dirty) {
                ops.publish(ops.ctx, child, 4096);
                ++tree.published;
            }
            store(table[i], pa | 3, dirty);
        } else {
            // A 1 GiB block cannot be appliance RAM (128 MiB); fail boundedly.
            return false;
        }
    }
    for (u32 i = entries; i < 512; ++i) {
        store(table[i], 0, dirty);
    }
    if (dirty) {
        ops.publish(ops.ctx, table, 4096);
        ++tree.published;
    }
    return true;
}
} // namespace

bool shadow_build(const ShadowOps& ops, u64 guest_root, Shadow& tree) {
    if (tree.count || !guest_root || (guest_root & 4095) || (!ops.read && !ops.map_table) ||
        !ops.translate || !ops.allocate || !ops.release || !ops.publish) {
        return false;
    }
    tree.root = 0;
    tree.leaves = 0;
    u64* root = allocate(ops, tree, tree.root);
    if (!root || !copy_table(ops, tree, guest_root, 0, root)) {
        shadow_destroy(ops, tree);
        return false;
    }
    return true;
}

void shadow_recycle(Shadow& tree) {
    tree.root = 0;
    tree.count = 0;
    tree.leaves = 0;
    tree.reused = 0;
    tree.published = 0;
}
void shadow_destroy(const ShadowOps& ops, Shadow& tree) {
    while (tree.capacity) {
        ops.release(ops.ctx, tree.tables[--tree.capacity]);
    }
    shadow_recycle(tree);
}
} // namespace mali
