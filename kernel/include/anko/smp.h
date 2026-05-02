#pragma once

#include "anko/types.h"

namespace smp {

void init(PhysAddr dtb_phys);
u32 cpu_count();
void send_wake_ipi(u32 cpu_id);
void handle_ipi();

} // namespace smp
