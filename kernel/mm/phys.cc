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
#include "libk/string.h"

extern "C" char __text_start[];
extern "C" char __kernel_end[];

namespace mm {

static PhysAllocator g_phys{};
static constexpr usize kMaxBitmapBytes = 128 * 1024;
static u8 g_bitmap[kMaxBitmapBytes];

static PhysAddr g_heap_base = 0;
static PhysAddr g_heap_curr = 0;
static PhysAddr g_heap_end  = 0;

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

static void reserve_range(PhysAddr base, PhysAddr end) {
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
    reserve_range(g_phys.ram_base, g_phys.ram_base + (2 * 1024 * 1024));
    reserve_range(reinterpret_cast<PhysAddr>(&__text_start[0]),
                  reinterpret_cast<PhysAddr>(&__kernel_end[0]));

    // Heap is carved out explicitly later by heap_init().
    g_heap_base = g_heap_curr = g_heap_end = 0;
}

PhysAddr alloc_page() {
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

void free_page(PhysAddr pa) {
    if (pa < g_phys.ram_base) return;
    const PhysAddr delta = pa - g_phys.ram_base;
    const usize idx = static_cast<usize>(delta / PAGE_SIZE);
    if (!page_in_range(idx)) return;
    bit_set_free(idx);
}

void heap_init() {
    constexpr usize kHeapPages = 4096; // 16 MiB kernel heap for VM metadata.
    PhysAddr first = alloc_page();
    if (!first) return;

    g_heap_base = first;
    g_heap_curr = first;
    g_heap_end  = first + PAGE_SIZE;

    for (usize i = 1; i < kHeapPages; ++i) {
        PhysAddr pa = alloc_page();
        if (!pa) break;
        if (pa != g_heap_end) break; // require contiguous pages for bump region
        g_heap_end += PAGE_SIZE;
    }
}

// Kernel heap bump allocator — used by cap tables, thread structs, etc.
void* heap_alloc(usize size, usize align) {
    if (size == 0) return nullptr;
    if (align == 0) align = alignof(void*);
    if ((align & (align - 1)) != 0) return nullptr;
    if (g_heap_curr == 0 || g_heap_curr >= g_heap_end) return nullptr;

    const PhysAddr aligned = (g_heap_curr + (align - 1)) & ~(static_cast<PhysAddr>(align - 1));
    if (aligned + size > g_heap_end) return nullptr;
    g_heap_curr = aligned + size;
    return reinterpret_cast<void*>(aligned);
}

} // namespace mm
