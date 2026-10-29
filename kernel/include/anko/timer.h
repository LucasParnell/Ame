#pragma once
#include "anko/types.h"

namespace drivers::timer {
// CPU-owned absolute nanosecond deadlines. Zero cancels this source only;
// timer maintenance and other sources remain armed. Call under local IRQ
// exclusion. An early already-programmed interrupt may be retained harmlessly.
enum class DeadlineSource : u32 { Scheduler, Ipc, Count };
void set_deadline(DeadlineSource source, u64 deadline_ns);
} // namespace drivers::timer
