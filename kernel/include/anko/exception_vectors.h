#pragma once
// anko/exception_vectors.h — names for the 16 AArch64 exception vector slots.
//
// The vector table is 4 groups of 4 (synchronous, IRQ, FIQ, SError) at 128-byte
// spacing, so a slot index is just the offset divided by 128. The kernel routes
// the slots it expects to real handlers (EL1 SPx sync -> panic, EL1 SPx IRQ ->
// irq_entry_kernel, lower-EL AArch64 sync -> syscall/fault entry); everything
// else lands here, because an unexpected exception must produce a report rather
// than a silent spin.

#include "anko/types.h"

namespace anko {

constexpr u32 kExceptionVectorCount = 16;

// One entry per slot, in table order. Slots handled by a dedicated entry name
// the vector; the rest are the ones that reach the unexpected-exception handler.
inline constexpr const char* kExceptionVectorReason[kExceptionVectorCount] = {
    "unexpected sync exception (EL1t, SP0)",   // 0
    "unexpected IRQ (EL1t, SP0)",              // 1
    "unexpected FIQ (EL1t, SP0)",              // 2
    "unexpected SError (EL1t, SP0)",           // 3
    "sync exception (EL1h, SPx)",              // 4 — kernel_sync_fault
    "IRQ (EL1h, SPx)",                         // 5 — irq_entry_kernel
    "unexpected FIQ (EL1h, SPx)",              // 6
    "SError (asynchronous abort, EL1h)",       // 7
    "sync exception (EL0, AArch64)",           // 8 — syscall/fault entry
    "IRQ (EL0, AArch64)",                      // 9 — irq_entry
    "unexpected FIQ (EL0, AArch64)",           // 10
    "SError (asynchronous abort, EL0)",        // 11
    "unexpected sync exception (EL0, AArch32)",// 12
    "unexpected IRQ (EL0, AArch32)",           // 13
    "unexpected FIQ (EL0, AArch32)",           // 14
    "SError (asynchronous abort, EL0, AArch32)", // 15
};

} // namespace anko
