#pragma once
// anko/panic.h — kernel panic / bugcheck handler.
//
// panic() and panicf() may be called from any kernel C++ context.
// kernel_sync_fault() is the assembly entry point for EL1 synchronous faults.

#include "anko/sched.h"   // TrapFrame
#include "anko/types.h"

// General panic — prints message, FP-chain call trace, then halts.
[[noreturn]] void panic(const char* msg);
[[noreturn]] void panicf(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));

// Called from vec_kernel_sync assembly.
// frame reuses TrapFrame layout; sp_el0 slot holds pre-fault SP_EL1.
extern "C" [[noreturn]] void kernel_sync_fault(u64 esr, u64 far, TrapFrame* frame);

// Assembly entry for the exception vectors with no dedicated handler (unexpected
// FIQ/SError, stray EL1t or AArch32 exceptions): reports the vector and halts.
extern "C" [[noreturn]] void kernel_unexpected_exception(u64 vector_index, u64 esr, u64 far,
                                                         TrapFrame* frame);

#define ANKO_PANIC(msg)           panic(msg)
#define ANKO_PANICF(fmt, ...)     panicf(fmt, ##__VA_ARGS__)
#define ANKO_ASSERT(cond)         do { if (!(cond)) [[unlikely]] panic("assert: " #cond); } while (0)
#define ANKO_ASSERTF(cond, fmt, ...) \
    do { if (!(cond)) [[unlikely]] panicf("assert: " fmt, ##__VA_ARGS__); } while (0)
