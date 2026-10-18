#pragma once
#include "anko/types.h"

namespace fpsimd {
// Cold per-thread/per-vCPU state. Kernel C++ and IRQ handlers stay scalar;
// ordinary syscalls need no save when they return to the same thread.
struct alignas(16) State {
    u64 v[32][2];
    u32 fpsr;
    u32 fpcr;
    u64 reserved;
};
static_assert(sizeof(State) == 528 && alignof(State) == 16);
static_assert(__builtin_offsetof(State, fpsr) == 512);
static_assert(__builtin_offsetof(State, fpcr) == 516);

#ifdef ANKO_HOST_TEST
// Privileged-register boundary for the production scheduler's host probes.
inline thread_local State live{};
inline thread_local u64 saves = 0, restores = 0;
inline void enable() {}
inline void save(State* state) { *state = live; ++saves; }
inline void restore(const State* state) { live = *state; ++restores; }
#else
extern "C" void arm64_fpsimd_enable();
extern "C" void arm64_fpsimd_save(State* state);
extern "C" void arm64_fpsimd_restore(const State* state);
inline void enable() { arm64_fpsimd_enable(); }
inline void save(State* state) { arm64_fpsimd_save(state); }
inline void restore(const State* state) { arm64_fpsimd_restore(state); }
#endif
} // namespace fpsimd
