# Anko Scheduler — Architecture Reference

Spec refs: §5.4 Threads and Scheduler, §5.6 Principals

---

## Overview: Ressha / Eki model

The scheduler design is called **Ressha**. Each online CPU owns one local **Eki** (station)
object (`struct Eki` in `kernel/include/anko/sched.h`). Scheduling decisions are made
per-Eki; cross-CPU wakes go through a lock-free pending-wake queue + IPI.

---

## Priority levels and scheduling classes

V0 uses five flat priority levels (`ThreadPriority`):

| Enum            | Level | SchedClass       | Weight |
|-----------------|-------|------------------|--------|
| PRIO_IDLE       | 0     | SCHED_IDLE       | 256    |
| PRIO_BACKGROUND | 1     | SCHED_BATCH      | 512    |
| PRIO_NORMAL     | 2     | SCHED_NORMAL     | 1024   |
| PRIO_SERVICE    | 3     | SCHED_SERVICE    | 2048   |
| PRIO_CRITICAL   | 4     | SCHED_INTERACTIVE| 4096   |

`SchedAttrs` (added Milestone 2) records the class and weight per thread. `pick_next()`
still uses the flat priority order; the fair-queue path (`kSchedFairQueue`) is not yet
enabled.

---

## Run queues

Each `Eki` holds `RunQueue queues[PRIO_COUNT]` — one singly-linked FIFO per priority
level. `pick_next()` scans from `PRIO_CRITICAL` down to `PRIO_BACKGROUND`, skipping
`PRIO_IDLE` (the idle thread is handled separately).

---

## SwitchReason

Recorded at the point a context switch is requested:

| Value         | When                                           |
|---------------|------------------------------------------------|
| SWITCH_NONE   | No pending switch                              |
| SWITCH_YIELD  | `sys_yield` called                             |
| SWITCH_BLOCK  | Thread blocked on doorbell or I/O              |
| SWITCH_TIMER  | Timer preemption (10 ms tick, IRQ 30)          |
| SWITCH_EXIT   | Thread or process terminated                   |
| SWITCH_IDLE   | No runnable threads, going idle                |

---

## WakeReason

Recorded on the thread when it is transitioned from blocked to runnable:

| Value          | When                                           |
|----------------|------------------------------------------------|
| WAKE_NONE      | Default / not yet set                          |
| WAKE_YIELD     | (reserved — yield does not sleep)              |
| WAKE_TIMER     | (reserved — timer wakes via preemption path)   |
| WAKE_DOORBELL  | `notify_object()` woke a sleeping waiter       |
| WAKE_NOTIFY    | Reserved for future sys_notify_handoff         |
| WAKE_REMOTE    | Thread drained from pending-wake queue (IPI)   |
| WAKE_RESUME    | `resume_thread()` resumed a suspended thread   |
| WAKE_IRQ       | Reserved for device IRQ wakeup                 |
| WAKE_DEBUG     | Reserved for debug injection                   |

---

## Timer preemption

`drivers::timer::init()` programs the AArch64 generic timer (PPI IRQ 30) for ~10 ms
intervals. On each tick, `sched::tick()` increments `runtime_ticks` on the current
thread and process, increments `g_principal_sched[principal_id].runtime_ns`, and calls
`request_switch(true, SWITCH_TIMER)`.

The actual context switch happens in `post_syscall()` after the SVC return path
processes `e.switch_pending`.

---

## Remote wake and IPI path

When `sched::wake(t)` is called and the target CPU differs from the current CPU:

1. `t->state` is set to `TS_RUNNABLE`.
2. `t` is pushed onto `target_eki.pending_wakes` (lock-free CAS stack).
3. `smp::send_wake_ipi(target_cpu)` fires a GICv2 SGI.
4. On the target CPU, `drain_pending_wakes()` is called from `post_syscall()` and
   from the idle loop, reversing the stack (to preserve FIFO order) and enqueuing
   each thread locally.

Counters: `Eki::remote_wakes` (incremented by the sending CPU),
`Eki::wake_remote` (incremented by the draining CPU per thread).

---

## Fairness tracking

`g_principal_sched[64]` (type `PrincipalSchedState`) accumulates `runtime_ns` per
principal ID on each timer tick. `sched::fairness_max_lag()` returns `max − min`
across all non-zero principals — a read-only fairness indicator. No enforcement is
performed yet (`kSchedPrincipalBudget = false`).

---

## Principal classes

| Enum                      | Value | Description                           |
|---------------------------|-------|---------------------------------------|
| PRINCIPAL_KERNEL          | 0     | Kernel threads (none in V0)           |
| PRINCIPAL_NATIVE_TRUSTED  | 1     | init, nameservice, io.console         |
| PRINCIPAL_NATIVE_SANDBOXED| 2     | demo.ping, demo.pong, untrusted code  |

---

## Context switch mechanics

`context_switch(CpuContext* prev, CpuContext* next)` in
`kernel/arch/arm64/context.S` saves callee-saved registers (x19–x28, fp, lr)
and SP into `prev`, then restores from `next`. Return from `context_switch`
resumes execution in the new thread at its saved `lr`.

In V0, all threads are EL0 and context switches happen through the TrapFrame
path in `post_syscall()`. `context_switch()` is implemented but its direct
call path is not yet wired into `post_syscall()`.

---

## Scheduler feature flags

Defined in `kernel/sched/runqueue.cc`:

| Flag                    | Status | Guards                                  |
|-------------------------|--------|-----------------------------------------|
| `kSchedFairQueue`       | false  | vruntime fair queue for NORMAL/BATCH    |
| `kSchedHandoff`         | false  | SYS_NOTIFY_HANDOFF same-CPU IPC boost   |
| `kSchedReplyHandoff`    | false  | SYS_REPLY_HANDOFF reply token path      |
| `kSchedDonation`        | false  | Priority donation on accepted handoff   |
| `kSchedWorkStealing`    | false  | Steal fair-class threads across CPUs    |
| `kSchedPrincipalBudget` | false  | Enforce period/budget in pick_next      |
| `kSchedVmClass`         | false  | SCHED_VM class and vCPU accounting      |

---

## Known gaps (future milestones)

- No vruntime / fair queue: all scheduling is strict priority FIFO (Milestone 3)
- No handoff: SYS_NOTIFY and SYS_WAIT_DOORBELL do not attempt same-CPU handoff (Milestone 4)
- No priority donation (Milestone 4)
- No work stealing (Milestone 5)
- No principal budget enforcement (Milestone 5)
- No dedicated scheduler conformance tests (Milestone 6)

---

## Debug counters (SYS_DEBUG_COUNTERS)

| Index | Metric                        |
|-------|-------------------------------|
| 2     | Total context switches        |
| 13    | Total yields                  |
| 14    | Block switches                |
| 15    | Timer preempts (all CPUs)     |
| 16    | Exit switches                 |
| 18–19 | Per-CPU context switches      |
| 20–21 | Per-CPU timer preempts        |
| 22    | Local wakes (all CPUs)        |
| 23    | Remote wakes (all CPUs)       |
| 24–25 | IPI sent / handled            |
| 26    | Pending-wake drain calls      |
| 27–28 | Doorbell local/remote wakes   |
| 29    | fairness_max_lag()            |
| 30–31 | Principal 1/2 runtime_ns      |
| 32    | wake_doorbell (all CPUs)      |
| 33    | wake_remote (all CPUs)        |
| 34    | wake_resume (all CPUs)        |
