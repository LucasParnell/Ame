#include "anko/adma.h"
#include "anko/mm.h"
#include "anko/spinlock.h"
namespace adma {
namespace {
kernel::SpinLock lock{};
const Backend* selected;
u64 active;
void cache(PhysAddr pa, bool invalidate) {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
    u64 ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 line = 4ull << ((ctr >> 16) & 15);
    for (u64 p = pa; p < pa + PAGE_SIZE; p += line) {
        if (invalidate) asm volatile("dc ivac, %0" ::"r"(p) : "memory");
        else asm volatile("dc cvac, %0" ::"r"(p) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
#else
    (void)pa; (void)invalidate;
#endif
}
SysError finish(Copy& copy, SysError result) {
    fence();
    if (copy.backend && !copy.backend->coherent) cache(copy.destination, true);
    copy.active = false; copy.result = result;
    kernel::LockGuard guard(lock);
    --active;
    return result;
}
}
SysError install(const Backend* backend) {
    kernel::LockGuard guard(lock);
    if (active) return E_BUSY;
    if (backend && (!backend->submit || !backend->poll || !backend->cancel)) return E_INVALID;
    selected = backend; return E_OK;
}
u64 active_count() { kernel::LockGuard guard(lock); return active; }
void publish_dma(PhysAddr pa, usize size, bool coherent) {
    if (!coherent) for (usize off = 0; off < size; off += PAGE_SIZE) {
        cache(pa + off, false); cache(pa + off, true);
    }
    fence();
}
void complete_dma(PhysAddr pa, usize size, bool coherent) {
    fence();
    if (!coherent) for (usize off = 0; off < size; off += PAGE_SIZE) cache(pa + off, true);
}
SysError start(Copy& copy, PhysAddr destination, PhysAddr source) {
    if (copy.active || !destination || !source ||
        ((destination | source) & (PAGE_SIZE - 1)) || destination == source) return E_INVALID;
    {
        kernel::LockGuard guard(lock);
        copy = {}; copy.backend = selected; copy.destination = destination; copy.source = source;
        copy.active = true; ++active;
    }
    if (!copy.backend) { copy_page(destination, source); return finish(copy, E_OK); }
    if (!copy.backend->coherent) {
        cache(source, false);
        // Destination cache lines cannot later overwrite data produced by DMA.
        cache(destination, false); cache(destination, true);
    }
    const auto result = copy.backend->submit(copy.backend->context, destination, source,
                                              PAGE_SIZE, &copy.cookie);
    if (result != E_OK && result != E_BUSY) {
        copy.result = result;
        return cancel(copy); // never retire a buffer on an unproven failed submission
    }
    return poll(copy);
}
SysError poll(Copy& copy) {
    if (!copy.active) return copy.result;
    if (copy.result != E_OK) return cancel(copy);
    const auto result = copy.backend->poll(copy.backend->context, copy.cookie);
    if (result == E_BUSY) return E_BUSY;
    if (result == E_OK) return finish(copy, E_OK);
    copy.result = result;
    return cancel(copy);
}
SysError cancel(Copy& copy) {
    if (!copy.active) return copy.result;
    if (copy.backend->cancel(copy.backend->context, copy.cookie) != E_OK) return E_BUSY;
    return finish(copy, copy.result == E_OK ? E_FAULT : copy.result);
}
void copy_page(PhysAddr destination, PhysAddr source) {
    // Source may be written during precopy. Such pages are reconciled while
    // frozen. Volatile accesses also prevent memcpy's nonaliasing assumptions.
    // Both endpoints are page aligned. Scalar words avoid an 8x byte-loop
    // penalty while retaining the kernel's no-SIMD context contract.
    auto* dst = reinterpret_cast<volatile u64*>(destination);
    auto* src = reinterpret_cast<const volatile u64*>(source);
    for (usize i = 0; i < PAGE_SIZE / sizeof(u64); ++i) dst[i] = src[i];
}
void fence() {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
    asm volatile("dsb ish" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
}
