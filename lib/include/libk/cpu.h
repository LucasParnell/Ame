#pragma once
// Architecture instructions only. Callers own privilege level, interrupt
// policy, scheduling, and the memory/device contract around these operations.
namespace libk::cpu {

inline void relax() {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    asm volatile("" ::: "memory");
#endif
}

// Cache hints only: they confer no ownership and never replace an acquire or
// release. The caller must retain the object until the actual access completes.
inline void prefetch_read(const void* address) { __builtin_prefetch(address, 0, 3); }

#if defined(__aarch64__)
// Privileged interrupt operations. Board-specific FIQ policy stays in the
// kernel caller; the library only performs the requested architectural action.
inline unsigned long long interrupt_save_disable() {
    unsigned long long saved;
    asm volatile("mrs %0, DAIF; msr DAIFSet, #2" : "=r"(saved) :: "memory");
    return saved;
}
inline void interrupt_restore(unsigned long long saved) {
    asm volatile("msr DAIF, %0" :: "r"(saved) : "memory");
}
inline void interrupt_enable() { asm volatile("msr DAIFClr, #2" ::: "memory"); }
inline void interrupt_disable() { asm volatile("msr DAIFSet, #2" ::: "memory"); }
inline void send_event() { asm volatile("sev" ::: "memory"); }

// A fast counter sample. Callers needing an instruction-serialized timestamp
// must provide the appropriate synchronization at their measurement boundary.
inline unsigned long long counter_ticks() {
    unsigned long long ticks;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(ticks) :: "memory");
    return ticks;
}

inline void instruction_sync() { asm volatile("isb" ::: "memory"); }
// Complete prior memory/MMIO accesses; stronger than a DMA ordering barrier.
inline void data_sync() { asm volatile("dsb sy" ::: "memory"); }
// Complete coherent shared-memory stores before signaling another CPU.
inline void data_store_sync() { asm volatile("dsb ishst" ::: "memory"); }

// Privileged idle only; never call from a lock waiter or a service. WFI wakes
// for a pending interrupt even with DAIF.I set. It does not consume SEV events.
// The scheduler must leave IRQs masked from its final work check through WFI,
// then unmask and synchronize so the interrupt handler can service the wake.
inline void wait_for_interrupt() {
    asm volatile("dsb sy\n wfi" ::: "memory");
}
#endif

} // namespace libk::cpu
