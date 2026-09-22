// kernel/mm/phys.cc — physical page allocator.
//
// Spec ref: §5.2 Memory Management
//
// V0: flat bitmap over the single RAM region from DTB.
//   - Alloc: O(n) scan from next_hint; wraps around once.
//   - Free:  O(1) clear bit.
//
// Caller must reserve pages used by the kernel image, DTB blob,
// and the bitmap itself before handing out any pages.

#include "anko/mm.h"
#include "anko/panic.h"
#include "anko/sched.h"
#include "anko/spinlock.h"
#include "libk/string.h"

extern "C" char __text_start[];
extern "C" char __kernel_end[];

namespace mm {

static PhysAllocator g_phys{};
static kernel::SpinLock g_phys_lock{};
static kernel::SpinLock g_heap_lock{};
static kernel::SpinLock g_slab_lock{};
static constexpr usize kMaxBitmapBytes = 128 * 1024;
static u8 g_bitmap[kMaxBitmapBytes];

static PhysAddr g_heap_curr = 0;
static PhysAddr g_heap_end  = 0;

struct FreeNode {
    FreeNode* next;
};

static constexpr usize kSlabClasses[] = {
    64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288
};
static FreeNode* g_slab_free[sizeof(kSlabClasses) / sizeof(kSlabClasses[0])]{};

static inline bool page_in_range(usize page_idx) {
    return page_idx < g_phys.page_count;
}

static inline bool bit_is_free(usize page_idx) {
    return (g_phys.bitmap[page_idx / 8] >> (page_idx % 8)) & 1u;
}

static inline void bit_set_free(usize page_idx) {
    g_phys.bitmap[page_idx / 8] |= static_cast<u8>(1u << (page_idx % 8));
}

static inline void bit_set_used(usize page_idx) {
    g_phys.bitmap[page_idx / 8] &= static_cast<u8>(~(1u << (page_idx % 8)));
}

static void reserve_range_locked(PhysAddr base, PhysAddr end) {
    if (end <= base) return;
    if (base < g_phys.ram_base) base = g_phys.ram_base;
    const PhysAddr ram_end = g_phys.ram_base + g_phys.ram_size;
    if (end > ram_end) end = ram_end;
    if (end <= base) return;

    const usize first = static_cast<usize>((base - g_phys.ram_base) / PAGE_SIZE);
    const usize last  = static_cast<usize>((end  - g_phys.ram_base + PAGE_SIZE - 1) / PAGE_SIZE);
    for (usize i = first; i < last && i < g_phys.page_count; ++i) {
        bit_set_used(i);
    }
}

void reserve_phys_range(PhysAddr base, u64 size) {
    if (size == 0) return;
    PhysAddr end = 0;
    if (__builtin_add_overflow(base, size, &end)) end = ~static_cast<PhysAddr>(0);
    kernel::LockGuard guard(g_phys_lock);
    reserve_range_locked(base, end);
}

void phys_init(PhysAddr ram_base, usize ram_size) {
    g_phys = {};
    g_phys.ram_base = ram_base;
    g_phys.ram_size = ram_size;
    g_phys.page_count = ram_size / PAGE_SIZE;
    g_phys.bitmap = g_bitmap;
    g_phys.next_hint = 0;

    const usize bitmap_bytes = (g_phys.page_count + 7) / 8;
    if (bitmap_bytes > kMaxBitmapBytes) {
        // Clamp to supported bitmap size; keeps allocator safe on huge RAM.
        g_phys.page_count = kMaxBitmapBytes * 8;
        g_phys.ram_size = g_phys.page_count * PAGE_SIZE;
    }

    libk::memset(g_bitmap, 0xFF, kMaxBitmapBytes); // 1 == free

    // Reserve low RAM near DTB/firmware handoff and the kernel image itself.
    // Hekate's Switch memory map keeps the first 16 MiB of DRAM reserved.
    const usize low_reserve =
        g_phys.ram_base == 0x8000'0000ull ? 16ull * 1024ull * 1024ull
                                          : 2ull * 1024ull * 1024ull;
    reserve_range_locked(g_phys.ram_base, g_phys.ram_base + low_reserve);
    reserve_range_locked(reinterpret_cast<PhysAddr>(&__text_start[0]),
                         reinterpret_cast<PhysAddr>(&__kernel_end[0]));

    // Heap is carved out explicitly later by heap_init().
    g_heap_curr = g_heap_end = 0;
}

PhysAddr alloc_page() {
    kernel::LockGuard guard(g_phys_lock);
    if (!g_phys.bitmap || g_phys.page_count == 0) return 0;

    for (usize pass = 0; pass < 2; ++pass) {
        usize start = (pass == 0) ? g_phys.next_hint : 0;
        usize end   = (pass == 0) ? g_phys.page_count : g_phys.next_hint;
        for (usize i = start; i < end; ++i) {
            if (!page_in_range(i)) continue;
            if (!bit_is_free(i)) continue;
            bit_set_used(i);
            g_phys.next_hint = (i + 1) % g_phys.page_count;
            return g_phys.ram_base + static_cast<PhysAddr>(i * PAGE_SIZE);
        }
    }
    return 0;
}

PhysAddr alloc_page_for(u32 owner_pid) {
    if (owner_pid != 0 && !sched::process_charge_page(owner_pid)) return 0;
    const PhysAddr pa = alloc_page();
    if (!pa && owner_pid != 0) sched::process_uncharge_page(owner_pid);
    return pa;
}

PhysAddr alloc_pages_contiguous_for(u32 pid, usize count) {
    usize charged = 0;
    for (; charged < count; ++charged) if (!sched::process_charge_page(pid)) break;
    PhysAddr pa = charged == count ? alloc_pages_contiguous(count) : 0;
    if (!pa) while (charged) { --charged; sched::process_uncharge_page(pid); }
    return pa;
}

u64 allocated_page_count() {
    kernel::LockGuard guard(g_phys_lock);
    u64 used = 0;
    for (usize i = 0; i < g_phys.page_count; ++i) if (!bit_is_free(i)) ++used;
    return used;
}

void free_page_for_identity(u64 identity, PhysAddr pa) {
    free_page(pa);
    auto p = sched::find_process(static_cast<u32>(identity));
    if (p && sched::process_identity(*p) == identity) sched::process_uncharge_page(p->pid);
}

PhysAddr alloc_pages_contiguous(usize count) {
    kernel::LockGuard guard(g_phys_lock);
    if (count == 0 || !g_phys.bitmap || g_phys.page_count == 0) return 0;
    usize start = 0;
    while (start + count <= g_phys.page_count) {
        // Advance past any used page.
        while (start + count <= g_phys.page_count && !bit_is_free(start)) {
            ++start;
        }
        // Measure the free run starting at `start`.
        usize run = 0;
        while (run < count && start + run < g_phys.page_count && bit_is_free(start + run)) {
            ++run;
        }
        if (run == count) {
            for (usize i = 0; i < count; ++i) bit_set_used(start + i);
            if (g_phys.next_hint < start + count) g_phys.next_hint = start + count;
            return g_phys.ram_base + static_cast<PhysAddr>(start * PAGE_SIZE);
        }
        // Skip past the first used page that broke the run.
        start += run + 1;
    }
    return 0;
}

PhysAddr alloc_pages_contiguous_aligned(usize count, usize align_pages) {
    kernel::LockGuard guard(g_phys_lock);
    if (count == 0 || !g_phys.bitmap || g_phys.page_count == 0) return 0;
    if (align_pages == 0 || (align_pages & (align_pages - 1)) != 0) return 0;  // power of two
    const usize step = align_pages > 1 ? align_pages : 1;
    // Walk the alignment grid; each scan stops at the first used page in the run,
    // so the work is bounded by the grid size. Allocation is not a hot path.
    for (usize start = 0; start + count <= g_phys.page_count; start += step) {
        usize run = 0;
        while (run < count && start + run < g_phys.page_count && bit_is_free(start + run)) {
            ++run;
        }
        if (run == count) {
            for (usize i = 0; i < count; ++i) bit_set_used(start + i);
            if (g_phys.next_hint < start + count) g_phys.next_hint = start + count;
            return g_phys.ram_base + static_cast<PhysAddr>(start * PAGE_SIZE);
        }
    }
    return 0;
}

void free_page(PhysAddr pa) {
    kernel::LockGuard guard(g_phys_lock);
    ANKO_ASSERT((pa & (PAGE_SIZE - 1)) == 0);
    ANKO_ASSERT(pa >= g_phys.ram_base);
    const PhysAddr delta = pa - g_phys.ram_base;
    const usize idx = static_cast<usize>(delta / PAGE_SIZE);
    ANKO_ASSERT(page_in_range(idx));
    ANKO_ASSERT(!bit_is_free(idx));
    bit_set_free(idx);
}

void free_page_for(u32 owner_pid, PhysAddr pa) {
    free_page(pa);
    if (owner_pid != 0) sched::process_uncharge_page(owner_pid);
}

void heap_init() {
    constexpr usize kHeapPages = 4096; // 16 MiB kernel heap for VM metadata.
    PhysAddr first = alloc_pages_contiguous(kHeapPages);
    if (!first) return;

    g_heap_curr = first;
    g_heap_end  = first + kHeapPages * PAGE_SIZE;
}

// Kernel heap bump allocator — used by cap tables, thread structs, etc.
void* heap_alloc(usize size, usize align) {
    kernel::LockGuard guard(g_heap_lock);
    if (size == 0) return nullptr;
    if (align == 0) align = alignof(void*);
    if ((align & (align - 1)) != 0) return nullptr;
    if (g_heap_curr == 0 || g_heap_curr >= g_heap_end) return nullptr;

    const PhysAddr aligned = (g_heap_curr + (align - 1)) & ~(static_cast<PhysAddr>(align - 1));
    if (aligned + size > g_heap_end) return nullptr;
    g_heap_curr = aligned + size;
    return reinterpret_cast<void*>(aligned);
}

static usize slab_class_index(usize size, usize align) {
    const usize needed = size > align ? size : align;
    for (usize i = 0; i < sizeof(kSlabClasses) / sizeof(kSlabClasses[0]); ++i) {
        if (needed <= kSlabClasses[i]) return i;
    }
    return ~static_cast<usize>(0);
}

void* slab_alloc(usize size, usize align) {
    if (size == 0) return nullptr;
    if (align == 0) align = alignof(void*);
    if ((align & (align - 1)) != 0) return nullptr;

    const usize idx = slab_class_index(size, align);
    if (idx == ~static_cast<usize>(0)) return heap_alloc(size, align);

    {
        kernel::LockGuard guard(g_slab_lock);
        FreeNode* node = g_slab_free[idx];
        if (node) {
            g_slab_free[idx] = node->next;
            libk::memset(node, 0, kSlabClasses[idx]);
            return node;
        }
    }

    void* p = heap_alloc(kSlabClasses[idx], align);
    if (p) libk::memset(p, 0, kSlabClasses[idx]);
    return p;
}

void slab_free(void* ptr, usize size, usize align) {
    if (!ptr || size == 0) return;
    if (align == 0) align = alignof(void*);
    const usize idx = slab_class_index(size, align);
    if (idx == ~static_cast<usize>(0)) return;

    auto* node = static_cast<FreeNode*>(ptr);
    kernel::LockGuard guard(g_slab_lock);
    node->next = g_slab_free[idx];
    g_slab_free[idx] = node;
}

} // namespace mm
