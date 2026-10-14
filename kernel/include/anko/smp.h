#pragma once

#include "anko/types.h"

namespace smp {

void init(PhysAddr dtb_phys);
u32 cpu_count();
void send_wake_ipi(u32 cpu_id);
void handle_ipi();

// Machine power through the platform firmware path (PSCI on arm64). Both mask
// interrupts first and only return when the firmware refuses; the caller then
// decides its fallback.
void system_off();
void system_reset();

} // namespace smp
