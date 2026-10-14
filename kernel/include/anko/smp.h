#pragma once

#include "anko/types.h"
#include "anko/syscall.h"

namespace smp {

void init(PhysAddr dtb_phys);
u32 cpu_count();
void send_wake_ipi(u32 cpu_id);
void handle_ipi();

// Power transitions return an error if refused; a successful transition never
// returns. CHECK performs no writes and permits init to reject unsupported modes
// before stopping any service. Native Anne uses its vendor mechanisms.
SysError system_power_check(u32 operation);
SysError system_off();
SysError system_reset(bool fastboot = false);

} // namespace smp
