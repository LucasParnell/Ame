// Host hardware seam: production scheduler/IPC choose deadlines unchanged.
#include "anko/timer.h"
thread_local u64 audit_timer_deadlines[static_cast<u32>(drivers::timer::DeadlineSource::Count)]{};
namespace drivers::timer {
void set_deadline(DeadlineSource source, u64 value) {
    audit_timer_deadlines[static_cast<u32>(source)] = value;
}
} // namespace drivers::timer
