#pragma once

#include "anko/types.h"
#include "anko/interrupt_mask.h"
#include "libk/spin.h"
#include "libk/cpu.h"

namespace kernel {

struct SpinLock {
    u32 value;
};

// CPU ownership needs local interrupt exclusion, not necessarily a shared lock.
class InterruptGuard {
public:
    InterruptGuard() {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
        irq_ = libk::cpu::interrupt_save_disable();
#endif
    }
    ~InterruptGuard() {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
        libk::cpu::interrupt_restore(host_interrupt_state(irq_));
#endif
    }
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;
private:
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
    u64 irq_;
#endif
};

class LockGuard {
public:
    explicit LockGuard(SpinLock& lock) : lock_(lock) {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
        irq_ = libk::cpu::interrupt_save_disable();
#endif
        libk::spin_lock(lock_.value);
    }

    ~LockGuard() {
        libk::spin_unlock(lock_.value);
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
        libk::cpu::interrupt_restore(host_interrupt_state(irq_));
#endif
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    SpinLock& lock_;
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
    u64 irq_;
#endif
};

} // namespace kernel
