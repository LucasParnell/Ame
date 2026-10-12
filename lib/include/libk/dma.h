#pragma once
// DMA ordering is separate from CPU-to-CPU ring publication. These barriers
// order device-visible accesses; they do NOT clean or invalidate cached data.
// Use only with coherent memory or uniformly Normal Non-Cacheable mappings.
namespace libk::dma {

inline void publish() {
#if defined(__aarch64__)
    asm volatile("dmb oshst" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

inline void consume() {
#if defined(__aarch64__)
    asm volatile("dmb oshld" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

inline void order_mmio() {
#if defined(__aarch64__)
    asm volatile("dmb osh" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

} // namespace libk::dma
