#pragma once

#include "anko/types.h"
#include "anko/syscall.h"

namespace smp {

void init(PhysAddr dtb_phys);
u32 cpu_count();
void send_wake_ipi(u32 cpu_id);
void handle_ipi();
// Caller serializes rendezvous and keeps IRQs disabled until resume(). False
// means a busy CPU did not reach an interruptible boundary; no commit is safe.
bool rendezvous();
void resume();

} // namespace smp
