#pragma once
// anko/mm.h — memory management data layouts.
//
// Spec ref: §5.2 Memory Management, §5.7 Arenas
//
// V0 memory model:
//   - 4 KiB pages, 48-bit VA; TTBR0 holds userspace and kernel identity mappings.
//     A shared high-half TTBR1 kernel mapping is not installed yet.
//   - Simple physical page allocator (bitmap over discovered RAM).
//   - No demand paging — all V0 memory is eagerly mapped.
//   - Kernel-owned metadata allocated from kernel heap.
//   - Shared physical pages mapped into multiple address spaces by capability.
//   - Arenas: registered writable regions for snapshot-capable services.

#include "anko/types.h"
#include "anko/object.h"

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
    PhysAddr phys_base;  // physical backing when kernel-created; 0 for VA-only registrations
    u64      size;
    u32      owner_pid;
    u32      rights;     // CapRights bitmask
    u32      flags;
    u32      owner_generation;
    u8       pad[24];    // pad to 64 bytes for cache friendliness
};
static_assert(sizeof(Arena) == 64);

constexpr u32 kArenaFlagOwnsPages = 1u << 30;
constexpr u32 kArenaFlagExposePhys = 1u << 31; // kernel-issued DMA arenas only

// Per-process arena table — fixed quota for V0.
// BootInfo pages, shared IPC rings, service log rings, and later terminal
// channels are all represented as registered arenas in V0. init owns many of
// these while wiring the boot graph, so 16 is too small once every service gets
// a BootInfo page before log/IPC ring creation. Keep this bounded, but large
// enough for the bootstrap authority holder.
constexpr u32 kArenaTableSize = 64;

struct ArenaTable {
    Arena entries[kArenaTableSize];
    object::Record* backing[kArenaTableSize];
    u64 unmapped[kArenaTableSize];
    u32   count;
    u32   lock;
};

namespace mm {

void phys_init(PhysAddr ram_base, usize ram_size);
void reserve_phys_range(PhysAddr base, u64 size);
PhysAddr alloc_page();
PhysAddr alloc_page_for(u32 owner_pid);
PhysAddr alloc_pages_contiguous_for(u32 owner_pid, usize count);
PhysAddr alloc_pages_contiguous(usize count); // returns 0 on failure
// Same, but the run must start at an `align_pages`-page boundary (a power of two),
// for callers that need the physical base aligned to the run size -- a 2 MiB
// stage-2 block needs 2 MiB-aligned backing, which alloc_pages_contiguous()
// does not promise.
PhysAddr alloc_pages_contiguous_aligned(usize count, usize align_pages);
void free_page(PhysAddr pa);
void free_page_for(u32 owner_pid, PhysAddr pa);
void free_page_for_identity(u64 identity, PhysAddr pa);
u64 allocated_page_count();
u64 total_page_count();

void heap_init();
void* heap_alloc(usize size, usize align);
void* slab_alloc(usize size, usize align);
void slab_free(void* ptr, usize size, usize align);

bool register_noncached_range(PhysAddr base, u64 size); // boot only
bool range_is_noncached(PhysAddr base, u64 size);
// Map a device MMIO window into the kernel address space (identity, Device
// attributes).  Used by the Phase C SMMU backend and the iommu-testdev
// selftest to reach registers outside the bootstrap identity map.
bool map_kernel_device_mmio(PhysAddr base, u64 size);
void vm_init();
void vm_enable_kernel();
VmSpace vm_space_create(u32 pid);
bool vm_map(VmSpace& space, VirtAddr va, PhysAddr pa, u32 rights);
bool vm_map_owned(VmSpace& space, VirtAddr va, PhysAddr pa, u32 rights);
bool vm_map_range(VmSpace& space, VirtAddr va, PhysAddr pa, u64 size, u32 rights);
bool vm_map_device_range(VmSpace& space, VirtAddr va, PhysAddr pa, u64 size, u32 rights);
void vm_unmap(VmSpace& space, VirtAddr va);
void vm_unmap_owned_range(VmSpace& space, VirtAddr va, u64 size);
void vm_space_destroy(VmSpace& space);
// The process registry has at most 64 live IDs. Reserve ASID zero for the
// kernel and use the mandatory eight-bit ASID range; no ARMv8.1 dependency.
// PID reuse is invalidated before publishing a newly created address space.
constexpr u64 vm_context(PhysAddr root, u32 pid) {
    return root | (static_cast<u64>(pid) << 48);
}
void vm_activate(PhysAddr pgd_phys);
// Make data written by the kernel visible to instruction fetch before a page
// is entered as executable code. On AArch64, D-cache and I-cache are not
// automatically coherent for freshly-copied code once caches are enabled.
void sync_icache_range(PhysAddr pa, usize len);
bool user_range_mapped(const VmSpace& space, VirtAddr va, u64 size, u32 rights);
bool user_va_to_pa(const VmSpace& space, VirtAddr va, u32 rights, PhysAddr* out_pa);
u64 vm_debug_pte(const VmSpace& space, VirtAddr va);

bool arena_register(ArenaTable& table, VirtAddr base, PhysAddr phys_base, u64 size, u32 owner_pid,
                    u32 rights, u32 flags, u32* out_index);
bool arena_claim_va(ArenaTable& table, Arena& arena, VirtAddr base, u64 size,
                    bool* out_new_claim = nullptr);
void arena_release_va(ArenaTable& table, Arena& arena, VirtAddr base);
void arena_unregister(ArenaTable& table, u32 index);
void destroy_arena(void* target);
void arena_retain_mapping(ArenaTable& table, VirtAddr base, object::Record* record);
void arena_unmap_page(ArenaTable& table, VirtAddr va);
void arena_clear(ArenaTable& table);

} // namespace mm
