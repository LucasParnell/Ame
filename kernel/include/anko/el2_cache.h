#pragma once
#include "anko/types.h"

// The bring-up EL2 monitor has its stage-1 MMU and D-cache disabled. Its
// context and private stack therefore cross a cacheability boundary with EL1.
namespace vm::el2_cache {
inline void clean(PhysAddr base, usize size, bool invalidate = false) {
#ifdef ANKO_HOST_TEST
    (void)base;
    (void)size;
    (void)invalidate;
#else
    if (!size) {
        return;
    }
    u64 ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 line = 4ull << ((ctr >> 16) & 15);
    for (u64 p = base & ~(line - 1); p < base + size; p += line) {
        if (invalidate) {
            asm volatile("dc civac, %0" ::"r"(p) : "memory");
        } else {
            asm volatile("dc cvac, %0" ::"r"(p) : "memory");
        }
    }
    asm volatile("dsb sy" ::: "memory");
#endif
}
inline void invalidate(PhysAddr base, usize size) {
#ifdef ANKO_HOST_TEST
    (void)base;
    (void)size;
#else
    u64 ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 line = 4ull << ((ctr >> 16) & 15);
    for (u64 p = base & ~(line - 1); p < base + size; p += line) {
        asm volatile("dc ivac, %0" ::"r"(p) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
#endif
}
} // namespace vm::el2_cache
