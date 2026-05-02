// kernel/sched/runqueue.cc — Ressha/Eki per-CPU scheduler.

#include "anko/sched.h"
#include "anko/mm.h"
#include "anko/smp.h"
#include "anko/log.h"
#include "libk/printf.h"
#include "libk/string.h"

namespace sched {

static Eki g_eki[kMaxCpus]{};
static u32 g_online_cpu_count = 1;
static u64 g_yield_count = 0;
static u64 g_principal_runtime[64]{};

static inline void metric_inc(u64& counter) {
    __atomic_add_fetch(&counter, 1ull, __ATOMIC_RELAXED);
}

static inline u64 metric_load(const u64& counter) {
    return __atomic_load_n(&counter, __ATOMIC_RELAXED);
}

static inline void send_event() {
    asm volatile("sev" ::: "memory");
}

static inline void irq_enable() {
    asm volatile("msr DAIFClr, #2" ::: "memory");
}

static inline void wait_for_interrupt() {
    irq_enable();
    asm volatile("wfi" ::: "memory");
}

u32 current_cpu_id() {
    u64 mpidr = 0;
    asm volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    return static_cast<u32>(mpidr & 0xFFu);
}

static Eki& eki_for_cpu(u32 cpu_id) {
    if (cpu_id >= kMaxCpus) cpu_id = 0;
    return g_eki[cpu_id];
}

static Eki& current_eki() {
    return eki_for_cpu(current_cpu_id());
}

static u64 online_mask() {
    u64 mask = 0;
    const u32 count = g_online_cpu_count > kMaxCpus ? kMaxCpus : g_online_cpu_count;
    for (u32 i = 0; i < count; ++i) mask |= (1ull << i);
    return mask ? mask : 1ull;
}

static ThreadPriority queue_class(Thread* t) {
    if (!t) return PRIO_NORMAL;
    if (t->priority >= PRIO_COUNT) return PRIO_NORMAL;
    return t->priority;
}

static void enqueue_tail(Eki& e, Thread* t) {
    if (!t || t->state == TS_DEAD) return;
    t->next = nullptr;
    t->state = TS_RUNNABLE;
    t->cpu_id = e.cpu_id;

    RunQueue& q = e.queues[queue_class(t)];
    if (!q.head) {
        q.head = q.tail = t;
    } else {
        q.tail->next = t;
        q.tail = t;
    }
    ++q.count;

    if (Process* p = find_process(t->pid)) ++p->runnable_threads;
}

static Thread* dequeue_from(RunQueue& q) {
    while (q.head) {
        Thread* t = q.head;
        q.head = t->next;
        if (!q.head) q.tail = nullptr;
        t->next = nullptr;
        if (q.count) --q.count;
        if (Process* p = find_process(t->pid)) {
            if (p->runnable_threads) --p->runnable_threads;
        }
        if (t->state == TS_RUNNABLE) return t;
    }
    return nullptr;
}

static Thread* pick_next(Eki& e) {
    for (int prio = PRIO_CRITICAL; prio >= PRIO_BACKGROUND; --prio) {
        if (Thread* t = dequeue_from(e.queues[prio])) return t;
    }
    return nullptr;
}

static void activate_thread_space(Thread* t) {
    if (!t) return;
    Process* p = find_process(t->pid);
    if (p) mm::vm_activate(p->ttbr0);
}

static u32 first_allowed_cpu(u64 affinity_mask) {
    const u64 allowed = affinity_mask & online_mask();
    if (allowed == 0) return current_cpu_id();
    for (u32 i = 0; i < kMaxCpus; ++i) {
        if (allowed & (1ull << i)) return i;
    }
    return 0;
}

static u32 target_cpu_for(Thread* t) {
    if (!t) return current_cpu_id();
    const u64 allowed = t->affinity_mask ? (t->affinity_mask & online_mask()) : online_mask();
    if (allowed & (1ull << t->cpu_id)) return t->cpu_id;
    return first_allowed_cpu(allowed);
}

static void pending_push(Eki& target, Thread* t) {
    Thread* old = nullptr;
    do {
        old = __atomic_load_n(&target.pending_wakes.head, __ATOMIC_ACQUIRE);
        t->remote_next = old;
    } while (!__atomic_compare_exchange_n(&target.pending_wakes.head,
                                          &old,
                                          t,
                                          false,
                                          __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));
}

static void remote_wake(Thread* t, u32 target_cpu) {
    Eki& src = current_eki();
    Eki& dst = eki_for_cpu(target_cpu);
    t->state = TS_RUNNABLE;
    t->cpu_id = target_cpu;
    pending_push(dst, t);
    metric_inc(src.remote_wakes);
    smp::send_wake_ipi(target_cpu);
}

static void drain_pending_wakes(Eki& e) {
    Thread* list = __atomic_exchange_n(&e.pending_wakes.head, nullptr, __ATOMIC_ACQ_REL);
    if (!list) return;
    metric_inc(e.pending_wake_drains);

    Thread* rev = nullptr;
    while (list) {
        Thread* next = list->remote_next;
        list->remote_next = rev;
        rev = list;
        list = next;
    }

    while (rev) {
        Thread* t = rev;
        rev = rev->remote_next;
        t->remote_next = nullptr;
        if (t->state == TS_DEAD) continue;
        if (t == e.current) {
            t->state = TS_RUNNABLE;
            continue;
        }
        enqueue_tail(e, t);
    }
}

[[noreturn]] static void enter_el0(VirtAddr entry, VirtAddr sp, u64 arg0) {
    asm volatile(
        "mov x0, %2\n"
        "msr sp_el0, %0\n"
        "msr elr_el1, %1\n"
        "mov x2, xzr\n"
        "msr spsr_el1, x2\n"
        "eret\n"
        :
        : "r"(sp), "r"(entry), "r"(arg0)
        : "x0", "x2", "memory");
    __builtin_unreachable();
}

static void init_eki(u32 cpu_id) {
    Eki& e = g_eki[cpu_id];
    libk::memset(&e, 0, sizeof(e));
    e.cpu_id = cpu_id;
    e.pending_reason = SWITCH_NONE;
}

void init() {
    ANKO_LOG_INFO("kernel/sched", "ressha init");
    for (u32 i = 0; i < kMaxCpus; ++i) init_eki(i);
    g_online_cpu_count = 1;
    mark_cpu_online(0);
    ANKO_LOG_INFO("kernel/sched", "eki0 init");
}

[[noreturn]] static void enter_cpu_scheduler(u32 cpu_id, bool announce_el0) {
    Eki& e = eki_for_cpu(cpu_id);
    for (;;) {
        drain_pending_wakes(e);
        Thread* next = pick_next(e);
        if (!next) {
            wait_for_interrupt();
            continue;
        }

        e.current = next;
        next->state = TS_RUNNING;
        activate_thread_space(next);
        if (announce_el0) ANKO_LOG_INFO("kernel/sched", "enter el0");
        enter_el0(next->tf.elr, next->tf.sp_el0, next->tf.x[0]);
    }
}

[[noreturn]] void enter() {
    enter_cpu_scheduler(current_cpu_id(), true);
}

[[noreturn]] void enter_secondary(u32 cpu_id) {
    enter_cpu_scheduler(cpu_id, false);
}

void tick() {
    Eki& e = current_eki();
    Thread* cur = e.current;
    if (!cur || cur->state != TS_RUNNING) return;
    metric_inc(e.timer_preempts);
    metric_inc(cur->runtime_ticks);
    if (Process* p = find_process(cur->pid)) {
        metric_inc(p->runtime_ticks);
        if (p->principal_id < 64) metric_inc(g_principal_runtime[p->principal_id]);
    }
    request_switch(true, SWITCH_TIMER);
}

void yield() {
    metric_inc(g_yield_count);
    request_switch(true, SWITCH_YIELD);
}

void block(Thread* t) {
    if (!t) return;
    if (t->state != TS_SLEEPING) t->state = TS_BLOCKED;
    if (t == current_eki().current) request_switch(false, SWITCH_BLOCK);
}

void wake(Thread* t) {
    if (!t || t->state == TS_DEAD) return;
    if (t->state == TS_RUNNABLE && t != current_eki().current) return;
    if (t->state == TS_RUNNING) return;

    const u32 target = target_cpu_for(t);
    if (target == current_cpu_id()) {
        Eki& e = current_eki();
        t->state = TS_RUNNABLE;
        if (t != e.current) enqueue_tail(e, t);
        metric_inc(e.local_wakes);
        send_event();
    } else {
        remote_wake(t, target);
    }
}

Thread* current() { return current_eki().current; }

Process* current_process() {
    Thread* cur = current();
    if (!cur) return nullptr;
    return find_process(cur->pid);
}

void enqueue(Thread* t) {
    if (!t || t->state == TS_DEAD) return;
    const u32 target = target_cpu_for(t);
    t->state = TS_RUNNABLE;
    if (target == current_cpu_id()) {
        enqueue_tail(current_eki(), t);
        send_event();
    } else {
        remote_wake(t, target);
    }
}

bool set_affinity(Thread* t, u64 cpu_mask) {
    if (!t || cpu_mask == 0) return false;
    if (t->state != TS_SUSPENDED) return false;
    const u64 allowed = cpu_mask & online_mask();
    if (allowed == 0) return false;
    t->affinity_mask = allowed;
    t->cpu_id = first_allowed_cpu(allowed);
    return true;
}

SysError resume_thread(Thread* t) {
    if (!t) return E_NOTFOUND;
    if (t->state == TS_DEAD) return E_DEAD;
    if (t->state != TS_SUSPENDED) return E_INVALID;
    t->state = TS_RUNNABLE;
    enqueue(t);
    return E_OK;
}

void request_switch(bool requeue_current, SwitchReason reason) {
    Eki& e = current_eki();
    e.switch_pending = true;

    if (reason == SWITCH_BLOCK || reason == SWITCH_EXIT) {
        e.requeue_current = requeue_current;
        e.pending_reason = reason;
        return;
    }

    if (requeue_current) e.requeue_current = true;
    if (e.pending_reason == SWITCH_NONE || e.pending_reason == SWITCH_IDLE) {
        e.pending_reason = reason;
    }
}

void mark_current_dead() {
    Thread* cur = current();
    if (!cur) return;
    cur->state = TS_DEAD;
    request_switch(false, SWITCH_EXIT);
}

extern "C" void post_syscall(void* frame_ptr) {
    Eki& e = current_eki();
    drain_pending_wakes(e);
    Thread* cur = e.current;
    if (!frame_ptr || !cur) return;

    auto* frame = static_cast<TrapFrame*>(frame_ptr);
    cur->tf = *frame;

    if (!e.switch_pending && cur->state != TS_RUNNING) {
        e.switch_pending = true;
        e.requeue_current = false;
        e.pending_reason = (cur->state == TS_DEAD) ? SWITCH_EXIT : SWITCH_BLOCK;
    }

    if (!e.switch_pending) return;
    e.switch_pending = false;
    const bool requeue = e.requeue_current;
    const SwitchReason reason = e.pending_reason;
    e.requeue_current = false;
    e.pending_reason = SWITCH_NONE;

    if (!requeue && cur->state == TS_RUNNABLE) {
        cur->state = TS_RUNNING;
        return;
    }

    if (requeue && cur->state == TS_RUNNING) {
        cur->state = TS_RUNNABLE;
        enqueue_tail(e, cur);
    } else if (requeue && cur->state == TS_RUNNABLE) {
        enqueue_tail(e, cur);
    }

    Thread* next = pick_next(e);
    while (!next) {
        if (cur->state == TS_RUNNING || cur->state == TS_RUNNABLE) {
            next = cur;
            break;
        }
        e.current = nullptr;
        wait_for_interrupt();
        drain_pending_wakes(e);
        next = pick_next(e);
    }

    if (next != cur) {
        metric_inc(e.context_switches);
        metric_inc(next->switches);
        switch (reason) {
        case SWITCH_YIELD: metric_inc(e.yield_switches); break;
        case SWITCH_BLOCK: metric_inc(e.block_switches); break;
        case SWITCH_TIMER: break;
        case SWITCH_EXIT:  metric_inc(e.exit_switches); break;
        case SWITCH_IDLE:  metric_inc(e.idle_switches); break;
        default: break;
        }
    }

    e.current = next;
    next->state = TS_RUNNING;
    activate_thread_space(next);
    *frame = next->tf;
}

u32 online_cpu_count() { return g_online_cpu_count; }

void set_online_cpu_count(u32 count) {
    if (count == 0) count = 1;
    if (count > kMaxCpus) count = kMaxCpus;
    g_online_cpu_count = count;
}

void mark_cpu_online(u32 cpu_id) {
    if (cpu_id >= kMaxCpus) return;
    if (cpu_id + 1 > g_online_cpu_count) g_online_cpu_count = cpu_id + 1;
}

void note_ipi_sent(u32) {
    metric_inc(current_eki().ipis_sent);
}

void note_ipi_handled() {
    metric_inc(current_eki().ipis_handled);
}

void drain_pending_wakes() {
    drain_pending_wakes(current_eki());
}

u64 context_switch_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) total += metric_load(g_eki[i].context_switches);
    return total;
}

u64 cpu_context_switch_count(u32 cpu_id) {
    return cpu_id < kMaxCpus ? metric_load(g_eki[cpu_id].context_switches) : 0;
}

u64 yield_count() { return metric_load(g_yield_count); }

u64 block_switch_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) total += metric_load(g_eki[i].block_switches);
    return total;
}

u64 timer_preempt_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) total += metric_load(g_eki[i].timer_preempts);
    return total;
}

u64 cpu_timer_preempt_count(u32 cpu_id) {
    return cpu_id < kMaxCpus ? metric_load(g_eki[cpu_id].timer_preempts) : 0;
}

u64 exit_switch_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) total += metric_load(g_eki[i].exit_switches);
    return total;
}

u64 local_wake_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) total += metric_load(g_eki[i].local_wakes);
    return total;
}

u64 remote_wake_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) total += metric_load(g_eki[i].remote_wakes);
    return total;
}

u64 ipi_sent_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) total += metric_load(g_eki[i].ipis_sent);
    return total;
}

u64 ipi_handled_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) total += metric_load(g_eki[i].ipis_handled);
    return total;
}

u64 pending_wake_drain_count() {
    u64 total = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) total += metric_load(g_eki[i].pending_wake_drains);
    return total;
}

u64 principal_runtime(u32 principal_id) {
    return principal_id < 64 ? metric_load(g_principal_runtime[principal_id]) : 0;
}

u64 fairness_max_lag() {
    u64 min = ~0ull;
    u64 max = 0;
    bool any = false;
    for (u32 i = 0; i < 64; ++i) {
        const u64 runtime = metric_load(g_principal_runtime[i]);
        if (runtime == 0) continue;
        any = true;
        if (runtime < min) min = runtime;
        if (runtime > max) max = runtime;
    }
    return any ? (max - min) : 0;
}

} // namespace sched
