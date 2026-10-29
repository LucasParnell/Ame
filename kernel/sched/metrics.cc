// kernel/sched/metrics.cc — scheduler statistics accessors.
//
// Backs the SYS_DEBUG_COUNTERS accessor block: folds per-CPU Eki counters and
// the few scheduler-global counters into flat read-only values.

#include "anko/sched.h"
#include "libk/metric.h"
#include "sched_internal.h"

namespace sched {
u64 cpu_ipi_handled_count(u32 cpu) { return cpu < kMaxCpus ? libk::metric_load(eki_for_cpu(cpu).ipis_handled) : 0; }

using libk::metric_load;

static u64 g_yield_count = 0;
static u64 g_wrong_cpu_dequeues = 0;

// Incremented by yield(); owned here because it is only ever reported.
void note_yield() { libk::metric_inc(g_yield_count); }

u64 context_switch_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).context_switches);
    return t;
}
u64 cpu_context_switch_count(u32 c) {
    return c < kMaxCpus ? metric_load(eki_for_cpu(c).context_switches) : 0;
}
u64 yield_count()        { return metric_load(g_yield_count); }
u64 block_switch_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).block_switches);
    return t;
}
u64 timer_preempt_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).timer_preempts);
    return t;
}
u64 cpu_timer_preempt_count(u32 c) {
    return c < kMaxCpus ? metric_load(eki_for_cpu(c).timer_preempts) : 0;
}
u64 exit_switch_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).exit_switches);
    return t;
}
u64 local_wake_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).local_wakes);
    return t;
}
u64 remote_wake_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).remote_wakes);
    return t;
}
u64 ipi_sent_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).ipis_sent);
    return t;
}
u64 ipi_handled_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).ipis_handled);
    return t;
}
u64 pending_wake_drain_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).pending_wake_drains);
    return t;
}
u64 wake_doorbell_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).wake_doorbell);
    return t;
}
u64 wake_remote_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).wake_remote);
    return t;
}
u64 wake_resume_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(eki_for_cpu(i).wake_resume);
    return t;
}
u64 wrong_cpu_dequeue_count() {
    return metric_load(g_wrong_cpu_dequeues);
}

} // namespace sched
