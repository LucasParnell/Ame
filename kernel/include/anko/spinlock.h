#pragma once

#include "anko/types.h"

namespace kernel {

struct SpinLock {
    u32 value;
};

class LockGuard {
public:
    explicit LockGuard(SpinLock& lock) : lock_(lock) {
#if defined(__aarch64__)
        asm volatile("mrs %0, DAIF; msr DAIFSet, #2" : "=r"(irq_) :: "memory");
#endif
        while (__atomic_exchange_n(&lock_.value, 1u, __ATOMIC_ACQUIRE) != 0) {
#if defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#else
            __builtin_ia32_pause();
#endif
        }
    }

    ~LockGuard() {
        __atomic_store_n(&lock_.value, 0u, __ATOMIC_RELEASE);
#if defined(__aarch64__)
        asm volatile("msr DAIF, %0" :: "r"(irq_) : "memory");
#endif
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    SpinLock& lock_;
#if defined(__aarch64__)
    u64 irq_;
#endif
};

} // namespace kernel
