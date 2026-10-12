#pragma once

// The native Huawei port has IRQ handlers but no FIQ handler. Preserve F in
// both live DAIF restores and host EL0 exception-return state. Interrupts routed
// to a higher exception level remain firmware's responsibility.
#if defined(ANKO_BOARD_HUAWEI_ANNE) || defined(ANKO_ANNE_DIAG_TEST)
#define ANKO_HOST_FIQ_MASK 0x40
#else
#define ANKO_HOST_FIQ_MASK 0
#endif

#ifndef __ASSEMBLER__
#include "anko/types.h"
namespace kernel {
constexpr u64 host_interrupt_state(u64 saved) { return saved | ANKO_HOST_FIQ_MASK; }
constexpr u64 kInitialUserPstate = ANKO_HOST_FIQ_MASK;
}
#endif
