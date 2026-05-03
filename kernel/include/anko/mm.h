#pragma once
// anko/mm.h — memory management data layouts.
//
// Spec ref: §5.2 Memory Management, §5.7 Arenas
//
// V0 memory model:
//   - 4 KiB pages, 48-bit VA, TTBR0=user TTBR1=kernel.
//   - Simple physical page allocator (bitmap over discovered RAM).
//   - No demand paging — all V0 memory is eagerly mapped.
//   - Kernel-owned metadata allocated from kernel heap.
//   - Shared physical pages mapped into multiple address spaces by capability.
//   - Arenas: registered writable regions for snapshot-capable services.

#include "anko/types.h"

constexpr usize PAGE_SIZE  = 4096;
constexpr usize PAGE_SHIFT = 12;
constexpr VirtAddr USER_STACK_TOP = 0x3FFF'0000ull;

// ---------------------------------------------------------------------------
// Physical page allocator state.
// V0: single flat bitmap over the RAM region discovered from DTB.
// Allocation and free are O(n) bitmap scan — acceptable for V0.
// ---------------------------------------------------------------------------
struct PhysAllocator {
    PhysAddr ram_base;
    usize    ram_size;
    u8*      bitmap;       // 1 bit per page; 1=free, 0=used
    usize    page_count;
    usize    next_hint;    // next page to check on alloc (not a guarantee)
};

// ---------------------------------------------------------------------------
// VmSpace — one per process.
// Holds the root page-table physical address and tracks mappings for teardown.
// V0: no COW, no demand paging.
// ---------------------------------------------------------------------------
struct VmSpace {
    PhysAddr pgd_phys;    // physical address of L0 (PGD) page table, 4KB aligned
    u32      pid;         // owning process
    // V0: no mapping list needed; all maps are permanent until process death.
};

struct MemoryObject {
    PhysAddr phys_base;
    u64      size;
};

// ---------------------------------------------------------------------------
// Arena — registered writable memory region (spec §5.7).
//
// Rules from spec:
//   - Writable service memory for IPC or restart must live in a registered arena.
//   - Ring buffers must be in registered arenas.
//   - Kernel tracks arena ownership per-process.
//   - Registration quota fixed at process creation.
//   - V0: no growable arenas, no snapshot persistence.
//     The arena system exists so later snapshotting has the right shape.
// ---------------------------------------------------------------------------
struct Arena {
    VirtAddr base_va;    // base virtual address in the owning process
    u64      size;
    u32      owner_pid;
    u32      rights;     // CapRights bitmask
    u32      flags;
    u8       pad[36];    // pad to 64 bytes for cache friendliness
};
static_assert(sizeof(Arena) == 64);

// Per-process arena table — fixed quota for V0.
constexpr u32 kArenaTableSize = 16;

struct ArenaTable {
    Arena entries[kArenaTableSize];
    u32   count;
};

namespace mm {

void phys_init(PhysAddr ram_base, usize ram_size);
PhysAddr alloc_page();
void free_page(PhysAddr pa);

void heap_init();
void* heap_alloc(usize size, usize align);

void vm_init();
void vm_enable_kernel();
VmSpace vm_space_create(u32 pid);
bool vm_map(VmSpace& space, VirtAddr va, PhysAddr pa, u32 rights);
bool vm_map_range(VmSpace& space, VirtAddr va, PhysAddr pa, u64 size, u32 rights);
bool vm_map_device_range(VmSpace& space, VirtAddr va, PhysAddr pa, u64 size, u32 rights);
void vm_unmap(VmSpace& space, VirtAddr va);
void vm_space_destroy(VmSpace& space);
void vm_activate(PhysAddr pgd_phys);
// Make data written by the kernel visible to instruction fetch before a page
// is entered as executable code. On AArch64, D-cache and I-cache are not
// automatically coherent for freshly-copied code once caches are enabled.
void sync_icache_range(PhysAddr pa, usize len);
bool user_range_mapped(const VmSpace& space, VirtAddr va, u64 size, u32 rights);
bool user_va_to_pa(const VmSpace& space, VirtAddr va, u32 rights, PhysAddr* out_pa);
u64 vm_debug_pte(const VmSpace& space, VirtAddr va);

bool arena_register(ArenaTable& table, VirtAddr base, u64 size, u32 owner_pid,
                    u32 rights, u32 flags, u32* out_index);

} // namespace mm
