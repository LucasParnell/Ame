// userland/lib/runtime/cxx_guard.cc — Midix userland C++ runtime hooks.
//
// Mochi services link with `-nostdlib` (see userland/CMakeLists.txt), so the
// compiler-rt / libsupc++ half of the C++ runtime is absent. The only piece the
// generated code actually needs for ordinary C++ is the Itanium-ABI guard for
// function-local statics with a non-constant initialiser:
//
//     static Thing t{...};   // needs a guard unless it can be constant-init'ed
//
// Without these symbols the link fails ("undefined symbol: __cxa_guard_acquire"),
// and without a correct implementation two threads could run the initialiser
// twice or read a half-built object. This is the real fix for that class; code
// must not have to move state to .bss or hand-roll a flag to avoid it.
//
// Guard word layout (Itanium C++ ABI, §3.3.3): byte 0 is the "initialised" flag
// and byte 1 is the "initialisation in progress" flag; the rest is reserved.

#include <stdint.h>

namespace {

constexpr uint8_t kGuardInitialised = 1;   // byte 0
constexpr uint8_t kGuardInProgress = 2;    // byte 1

inline void cpu_relax() {
    __asm__ __volatile__("yield" ::: "memory");
}

} // namespace

extern "C" {

// Returns non-zero when the caller must run the initialiser, zero when another
// thread already completed it. Atomic so concurrent first-use is safe.
int __cxa_guard_acquire(uint64_t* guard) {
    auto* flags = reinterpret_cast<uint8_t*>(guard);
    for (;;) {
        const uint8_t state = __atomic_load_n(flags, __ATOMIC_ACQUIRE);
        if ((state & kGuardInitialised) != 0) return 0;
        if ((state & kGuardInProgress) != 0) {
            cpu_relax();
            continue;
        }
        uint8_t expected = state;
        // Claim the in-progress bit; another thread may have claimed it first.
        if (__atomic_compare_exchange_n(flags, &expected,
                                        static_cast<uint8_t>(state | kGuardInProgress), false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
}

// Publishes the fully constructed object to every other thread.
void __cxa_guard_release(uint64_t* guard) {
    auto* flags = reinterpret_cast<uint8_t*>(guard);
    uint8_t state = __atomic_load_n(flags, __ATOMIC_RELAXED);
    __atomic_store_n(flags, static_cast<uint8_t>(state | kGuardInitialised), __ATOMIC_RELEASE);
}

// The initialiser failed (it must not have thrown in a freestanding build, but
// the ABI requires the hook): drop the claim so a later call retries.
void __cxa_guard_abort(uint64_t* guard) {
    auto* flags = reinterpret_cast<uint8_t*>(guard);
    uint8_t state = __atomic_load_n(flags, __ATOMIC_RELAXED);
    __atomic_store_n(flags, static_cast<uint8_t>(state & ~kGuardInProgress), __ATOMIC_RELEASE);
}

} // extern "C"
