# SCHED_SPEC_V2.md — Ressha/Eki Fair Scheduler Tuned for IPC

Status: draft v0.1  
Scope: Midix kernel scheduler V2  
Primary goal: fair CPU allocation without sacrificing the IPC latency contract

---

## 1. Summary

Scheduler V2 replaces the current strict-priority FIFO scheduler with a per-CPU, capability-aware fair scheduler that is biased toward IPC responsiveness only where the bias is bounded, accountable, and compatible with Midix invariants.

The design keeps the existing Ressha/Eki shape:

- **Ressha** is the scheduler subsystem as a whole.
- **Eki** is the per-CPU scheduler station.
- Each CPU owns its run queues, local accounting, timer state, pending wake queue, and idle path.
- Cross-CPU wakeup never directly mutates another CPU's run queue.
- Cross-CPU coordination uses per-CPU MPSC queues and IPIs.

The important V2 change is policy. V1/V0 picks the highest non-empty priority queue and uses FIFO within that priority. V2 introduces:

1. **Class-based scheduling** for RT, interactive/service, normal, batch, VM, and idle work.
2. **Hierarchical fairness**, first between principals and then between threads inside each principal.
3. **Bounded IPC wake bias**, so servers and receivers woken by doorbells run quickly without being able to steal unbounded CPU time.
4. **IPC-aware placement**, so latency-sensitive IPC pairs are kept co-resident where possible, while throughput rings may be split across cores.
5. **Budget enforcement**, so principals, VMs, services, and RT work cannot bypass CPU accounting.
6. **Work stealing by request**, preserving the no-cross-CPU-runqueue-lock invariant.

V2 is not a general-purpose monolithic scheduler. It is a Midix scheduler: small, per-CPU, IPC-aware, principal-aware, and explicit about what receives fast paths.

---

## 2. Design Goals

### 2.1 Must Have

- Preserve per-CPU run queues.
- Preserve the rule that hot-path operations do not take cross-CPU locks.
- Bound starvation for non-RT work.
- Treat principals as the first-class fairness unit.
- Keep IPC receivers responsive when the communication pattern opts into the IPC contract.
- Keep wakeup insertion O(1) for FIFO/fast wake and O(log n) for fair queues.
- Keep cross-CPU wake O(1) on the sender side.
- Make scheduling policy observable through metrics.
- Keep real-time work explicit, admission-controlled, and non-stealable.
- Integrate VM/vCPU scheduling without trusting the guest scheduler for host-level fairness.

### 2.2 Should Have

- Avoid global scheduler locks entirely.
- Avoid touching unrelated principals on the hot path.
- Support heterogeneous ARM topology later without changing the base policy.
- Support service-specific latency hints without embedding service policy in the kernel.
- Allow bring-up on simple systems with small fixed arrays before replacing them with larger dynamic structures.

### 2.3 Must Not Have

- No scheduler-wide global run queue.
- No hidden priority boost that bypasses principal budget.
- No automatic optimization for arbitrary IPC patterns.
- No migration of RT, pinned, LPI-bound driver, or host-controlled vCPU threads.
- No dependency on inspecting userspace ring contents from the scheduler.
- No fairness guarantee that includes hard RT work. RT work is admitted and bounded separately.

---

## 3. Scheduling Model Overview

V2 divides runnable work into four bands:

```text
Band 0: RT / deadline-critical work
Band 1: latency-sensitive service and interactive work
Band 2: fair time-shared work: normal, batch, and most VM runners
Band 3: idle work
```

The picker checks bands in order, but not all bands are strict-priority in the V1 sense.

RT work remains strict because missing a real-time deadline is qualitatively different from being treated unfairly. Everything below RT participates in fair scheduling.

The high-level picker is:

```text
pick_next(cpu):
    drain_pending_wakes(cpu)

    if eligible_rt_exists(cpu):
        return pick_rt(cpu)

    if eligible_latency_work_exists(cpu):
        return pick_latency_fair(cpu)

    if eligible_timeshare_work_exists(cpu):
        return pick_timeshare_fair(cpu)

    if steal_request_should_be_sent(cpu):
        send_steal_request(cpu)

    return idle
```

The latency and time-shared bands use the same fair core. They differ by default slice, wake credit, stealing behavior, and placement rules.

---

## 4. Hard Invariants

### 4.1 Per-CPU Ownership

Each `Eki` owns:

- Its RT queues.
- Its latency fair queues.
- Its time-shared fair queues.
- Its idle thread.
- Its local pending-wake drain.
- Its local view of runnable principal entities.
- Its local timer and preemption decision.

A CPU may enqueue directly only onto its own Eki. A CPU that needs to make work runnable on another Eki pushes to that Eki's pending-wake queue and sends an IPI.

### 4.2 No Cross-CPU Runqueue Mutation

No CPU may directly remove from, insert into, rotate, or rebalance another CPU's run queue.

This includes work stealing. Stealing is implemented as a request to the target CPU, not as a thief-side dequeue from the victim queue.

### 4.3 Budget Cannot Be Bypassed

IPC wake credit, handoff donation, interactive latency bias, and affinity hints affect dispatch order. They do not create free CPU time.

All executed time is charged to:

1. The running thread.
2. The owning process.
3. The owning principal.
4. The owning scheduling class.
5. The owning VM, if applicable.

### 4.4 IPC Bias Is Bounded

A thread woken by IPC may receive bounded dispatch credit. The credit may move it ahead of comparable fair work. The credit may not:

- Move normal work ahead of admitted RT work.
- Run a throttled principal.
- Ignore CPU affinity.
- Ignore class restrictions.
- Accumulate without bound.
- Survive repeated CPU-bound execution.

### 4.5 Kernel Does Not Inspect Ring Payloads

The scheduler may consume IPC metadata attached to doorbells, endpoints, scheduling caps, or ring registration objects. It must not inspect arbitrary userspace ring payloads to make scheduling decisions.

---

## 5. Core Terms

### Thread

A schedulable execution context. Threads have a scheduling class, base weight, optional latency preference, optional hard affinity, optional soft affinity, and runtime accounting.

### Principal

The kernel-tracked identity that owns CPU budget and scheduling policy. Fairness is enforced primarily at the principal level. A principal can own one process, many processes, a service tree, or a VM.

### Eki

The scheduler station local to one CPU.

### Scheduling Entity

An object that can sit in a fair queue. V2 uses two entity levels:

- `PrincipalEntity`: one runnable principal on one CPU.
- `ThreadEntity`: one runnable thread inside a principal on one CPU.

### Wake Credit

A bounded, temporary dispatch-order advantage given to a thread woken for latency-sensitive IPC. Wake credit does not reduce accounted runtime.

### Handoff

An explicit synchronous IPC path where the sender blocks and the receiver runs immediately on the same CPU, subject to eligibility checks.

---

## 6. Classes

V2 keeps the current class names and makes them policy-active.

```cpp
enum SchedClass : u8 {
    SCHED_IDLE        = 0,
    SCHED_BATCH       = 1,
    SCHED_NORMAL      = 2,
    SCHED_SERVICE     = 3,
    SCHED_INTERACTIVE = 4,
    SCHED_RT          = 5,
    SCHED_VM          = 6,
};
```

### 6.1 SCHED_RT

For admitted real-time work only.

Properties:

- Fixed priority or deadline ordered.
- Budgeted.
- Admission controlled.
- Not stealable.
- Not migratable unless explicitly configured as migratable RT.
- May preempt fair work immediately.
- May not be created by untrusted principals without a scheduling capability.

Typical users:

- Audio deadlines.
- Sensor fusion.
- Hard input paths where missing a deadline corrupts state.

### 6.2 SCHED_INTERACTIVE

For latency-sensitive foreground work.

Properties:

- Fair-scheduled.
- Lower target latency than normal work.
- Eligible for bounded wake credit.
- Stealable only if not marked sticky and only if affinity allows.

Typical users:

- UI service.
- Input compositor path.
- Foreground shell.

### 6.3 SCHED_SERVICE

For system services that handle IPC requests.

Properties:

- Fair-scheduled.
- Eligible for IPC receiver credit when woken by a latency-marked doorbell.
- May be marked `IPC_SERVER` to receive short drain quanta.
- May be LPI-bound or pinned when acting as a driver thread.
- LPI-bound service threads are not stealable.

Typical users:

- Nameservice.
- Filesystem service.
- Console service.
- Driver service event threads.

### 6.4 SCHED_NORMAL

For ordinary time-shared work.

Properties:

- Fair-scheduled.
- Moderate slice size.
- Stealable.
- Eligible for weak wake credit only when explicitly attached to latency IPC.

### 6.5 SCHED_BATCH

For throughput-oriented or background work.

Properties:

- Fair-scheduled.
- Larger slices.
- Weak or no wake credit.
- Freely stealable when affinity allows.
- Lower default weight than normal work.

### 6.6 SCHED_VM

For vCPU runner threads and VM host-side execution.

Properties:

- Fair-scheduled unless admitted as RT-vCPU.
- Principal budget is per VM.
- Host schedules vCPU runners, not guest threads.
- Stealing depends on VM policy and vCPU pinning.
- A vCPU with RT host budget is treated as RT for host scheduling but still only guarantees vCPU time, not guest-thread correctness.

### 6.7 SCHED_IDLE

For idle threads and explicit idle-priority work.

Properties:

- Runs only when nothing else is eligible.
- Not part of fairness accounting except idle metrics.

---

## 7. Weights, Latency, and Defaults

### 7.1 Base Weights

Initial default weights:

```text
SCHED_BATCH       512
SCHED_NORMAL     1024
SCHED_SERVICE    1536
SCHED_INTERACTIVE 2048
SCHED_VM         1024
SCHED_IDLE          0
SCHED_RT            n/a; handled by RT policy
```

The current legacy priorities map as follows:

```text
PRIO_BACKGROUND -> SCHED_BATCH       weight 512
PRIO_NORMAL     -> SCHED_NORMAL      weight 1024
PRIO_SERVICE    -> SCHED_SERVICE     weight 1536 or 2048 during transition
PRIO_CRITICAL   -> SCHED_INTERACTIVE by default, SCHED_RT only with explicit cap
PRIO_IDLE       -> SCHED_IDLE
```

`PRIO_CRITICAL` must not automatically become RT. RT must be opt-in and admission-controlled.

### 7.2 Latency Hints

Each non-RT thread has a latency hint:

```cpp
enum LatencyHint : u8 {
    LATENCY_BATCH       = 0,
    LATENCY_NORMAL      = 1,
    LATENCY_INTERACTIVE = 2,
    LATENCY_IPC_SERVER  = 3,
};
```

Latency hint affects:

- Target slice.
- Wake credit cap.
- Preemption threshold.
- Whether the thread receives IPC drain quantum.

Latency hint does not affect total CPU entitlement except through configured weight.

### 7.3 Default Slices

Initial values for bring-up:

```text
RT quantum:                  class-specific; configured by admission
interactive target latency:  4 ms
service target latency:      4 ms
normal target latency:       12 ms
batch target latency:        24 ms
min granularity:             0.5 ms
max fair slice:              8 ms
IPC drain quantum:           50–200 us, tune by measurement
handoff max quantum:         min(sender_remaining_slice, 200 us) initially
```

The scheduler should use architectural counter time rather than timer-tick counts for runtime accounting.

---

## 8. Fairness Model

### 8.1 Hierarchical Fairness

V2 fairness is hierarchical:

```text
CPU
  -> fair selection among runnable principals
      -> fair selection among runnable threads owned by that principal
```

This prevents a principal with many runnable threads from gaining more CPU than a principal with one runnable thread.

Example:

```text
Principal A has 1 runnable thread.
Principal B has 8 runnable threads.
Both principals have equal weight.

Result:
  A receives roughly 50% CPU.
  B receives roughly 50% CPU split among its 8 threads.
```

Thread count is not a way to buy CPU.

### 8.2 Principal Entity

Each CPU has a local `PrincipalEntity` for each principal that has runnable work on that CPU.

Suggested fields:

```cpp
struct PrincipalEntity {
    u32 principal_id;
    u32 weight;
    u64 vruntime_ns;
    u64 runtime_ns;
    u64 runnable_threads;
    u64 local_budget_ns;
    bool throttled;
    FairQueue threads;
};
```

The local principal entity sits in the CPU's fair principal tree or heap.

### 8.3 Thread Entity

Suggested thread scheduling fields:

```cpp
struct ThreadSchedState {
    SchedClass cls;
    u32 weight;
    u8 latency_hint;
    u8 flags;

    u64 vruntime_ns;
    u64 runtime_ns;
    u64 slice_ns;
    u64 deadline_ns;

    i64 wake_credit_ns;
    u64 last_start_ns;
    u64 last_wake_ns;
    u64 sleep_start_ns;

    u32 home_cpu;
    u32 last_cpu;
    u64 hard_affinity_mask;
    u64 soft_affinity_mask;
};
```

### 8.4 Virtual Runtime

Runtime is charged by weighted virtual runtime:

```text
delta_vruntime = delta_exec_ns * NICE_0_WEIGHT / entity_weight
```

For a thread:

```text
thread.vruntime += delta_exec_ns * 1024 / thread.weight
```

For a principal:

```text
principal.vruntime += delta_exec_ns * 1024 / principal.weight
```

The picker chooses the eligible entity with the smallest effective key.

### 8.5 Effective Pick Key

The stored `vruntime_ns` is the accounting truth. The effective pick key may include bounded wake credit:

```text
effective_key = vruntime_ns - min(wake_credit_ns, max_credit_for_class)
```

Wake credit affects only dispatch order. It does not reduce `vruntime_ns` and does not reduce accounted runtime.

### 8.6 Fairness Bound

For non-RT work, the scheduler should maintain this approximate bound per CPU:

```text
max_principal_lag <= target_latency_ns + max_slice_ns + ipc_credit_cap_ns
```

The diagnostic harness should expose max principal lag, max thread lag within principal, and class-specific wake latency.

---

## 9. Queue Structure

### 9.1 Per-CPU Eki Layout

V2 `Eki` should become:

```cpp
struct Eki {
    u32 cpu_id;
    Thread* current;
    Thread* idle;

    RtQueue rt;
    FairPrincipalQueue latency_fair;
    FairPrincipalQueue timeshare_fair;

    PendingWakeQueue pending_wakes;
    PendingStealQueue pending_steal_requests;

    u64 min_vruntime_latency;
    u64 min_vruntime_timeshare;

    bool switch_pending;
    bool requeue_current;
    SwitchReason pending_reason;

    SchedMetrics metrics;
};
```

### 9.2 RT Queue

Initial implementation may use fixed arrays of FIFO queues by RT priority:

```text
rt_queue[0..MAX_RT_PRIO)
rt_bitmap
```

Later implementation may use deadline heap if needed.

RT queue operations:

```text
enqueue_rt: O(1)
pick_rt:    O(1) with bitmap, or O(log n) with deadline heap
```

### 9.3 Fair Principal Queue

Use a min-heap or tree keyed by principal effective virtual runtime.

Bring-up can use a binary heap because the number of runnable principals is small.

Operations:

```text
enqueue principal: O(log P)
dequeue principal: O(log P)
rekey principal:   O(log P)
pick principal:    O(1) peek + O(log P) update after dequeue
```

### 9.4 Per-Principal Thread Queue

Each local principal entity owns one fair queue of its runnable threads.

Operations:

```text
enqueue thread: O(log T_p)
dequeue thread: O(log T_p)
rekey thread:   O(log T_p)
```

Where `T_p` is runnable threads for one principal on one CPU.

### 9.5 FIFO Fast Queue for Handoff

Direct handoff does not enter the fair queue when all handoff conditions are met. The receiver is installed as the next running thread directly.

There should not be a general unbounded fast queue. Handoff is a special same-CPU synchronous IPC path.

---

## 10. Runtime Accounting

### 10.1 Accounting Point

Runtime is charged on every context switch and on periodic timer tick if the current thread continues running.

Use:

```text
now = sched_clock_ns()
delta = now - current.last_start_ns
```

Then charge:

```text
thread.runtime_ns += delta
process.runtime_ns += delta
principal.runtime_ns += delta
class.runtime_ns += delta
```

And update virtual runtimes for non-RT work.

### 10.2 Tick Policy

Timer ticks should request reschedule when:

- Current thread consumed its fair slice.
- A higher class became runnable.
- A fair entity with much smaller effective key is waiting.
- Current principal exhausted budget.
- Current RT budget expired.
- A remote IPI delivered a runnable thread that should preempt current.

V2 should not depend on 10 ms ticks for fairness. Ticks are preemption and accounting checkpoints; fair math uses real elapsed time.

### 10.3 Slice Calculation

For fair work:

```text
slice = target_latency(class) * thread.weight / runnable_weight_in_principal
slice = clamp(slice, min_granularity(class), max_slice(class))
```

Then the principal slice is similarly bounded by competing runnable principals.

A thread may be preempted earlier if:

- It blocks.
- It yields.
- A handoff occurs.
- Its principal budget expires.
- A higher band becomes runnable.
- IPC receiver preemption threshold is met.

---

## 11. Principal Budgets

### 11.1 Purpose

Principal budgets are the security and resource-control layer of scheduling. Fairness alone prevents thread-count abuse, but budgets prevent a principal from exceeding an explicit CPU allocation.

### 11.2 Budget Fields

Suggested principal state:

```cpp
struct PrincipalSchedState {
    u32 principal_id;
    u32 weight;

    u64 period_ns;
    u64 budget_ns;
    u64 used_in_period_ns;
    u64 period_start_ns;

    u64 runtime_ns;
    u32 runnable_threads;
    bool throttled;
};
```

### 11.3 Budget Enforcement

If `budget_ns == 0`, the principal has no hard cap and participates only in weighted fairness.

If `budget_ns > 0`, the principal may run only while:

```text
used_in_period_ns < budget_ns
```

When exhausted:

- Its fair entities remain queued but ineligible.
- Wakes still mark threads runnable.
- IPC credit is not granted while throttled.
- Handoff into the principal fails with `E_BUDGET` or falls back to normal notify, depending on syscall semantics.
- At the next period, the principal is unthrottled and its entities are reactivated.

### 11.4 Per-CPU Budget Cache

To avoid global hot locks, each CPU keeps a small local budget cache for active principals.

```text
On context switch/accounting:
  charge local principal bucket.
  if local bucket remains positive: continue.
  else refill from global principal budget with atomic subtract.
```

Refill is cold relative to every context switch and can be tuned by refill quantum.

Suggested refill quantum:

```text
min(500 us, remaining_global_budget)
```

For bring-up, a single atomic `used_in_period_ns` is acceptable because principal count and CPU count are small.

---

## 12. IPC-Aware Scheduling

IPC tuning in V2 has four mechanisms:

1. Wake credit.
2. Wake-affine placement.
3. Drain quantum.
4. Explicit handoff.

All four are bounded. None bypass budget.

### 12.1 IPC Metadata

Doorbells and ring registration objects may carry scheduler-visible hints:

```cpp
enum IpcSchedHint : u32 {
    IPC_HINT_NONE        = 0,
    IPC_HINT_LATENCY     = 1 << 0,
    IPC_HINT_THROUGHPUT  = 1 << 1,
    IPC_HINT_WAKE_AFFINE = 1 << 2,
    IPC_HINT_HANDOFF_OK  = 1 << 3,
    IPC_HINT_SERVER      = 1 << 4,
    IPC_HINT_NO_STEAL    = 1 << 5,
};
```

Hints are set only by a holder of an appropriate scheduling or endpoint-management capability.

### 12.2 Wake Credit

When a thread wakes because of a latency-sensitive doorbell, it receives wake credit:

```text
credit = class_credit(thread.class, doorbell.hints)
```

Initial caps:

```text
SCHED_INTERACTIVE: 300 us
SCHED_SERVICE:     200 us
SCHED_NORMAL:       75 us only with IPC_HINT_LATENCY
SCHED_BATCH:         0 us
SCHED_VM:            0 us by default; VM-specific policy may opt in
```

Credit is applied as:

```text
thread.wake_credit_ns = min(thread.wake_credit_ns + credit, class_credit_cap)
```

Credit decays when the thread runs:

```text
thread.wake_credit_ns -= min(thread.wake_credit_ns, delta_exec_ns)
```

Credit is cleared when:

- The thread consumes a full slice.
- The thread runs CPU-bound beyond the IPC drain quantum.
- The principal becomes throttled.
- The thread migrates for load balancing, unless the migration is due to wake-affine IPC placement.

### 12.3 Wake Credit Eligibility

Wake credit applies only if all are true:

- Wake reason is `WAKE_DOORBELL`, `WAKE_IRQ`, or explicit IPC endpoint wake.
- The endpoint has `IPC_HINT_LATENCY` or the thread has `LATENCY_IPC_SERVER`.
- The thread was actually sleeping or blocked, not merely already runnable.
- The principal is not throttled.
- The thread is not batch class.
- The thread is not trying to accumulate repeated credit while already runnable.

### 12.4 Wake-Affine Placement

For latency IPC, receiver placement should prefer co-residence with the active sender when possible.

Wake target selection:

```text
target_cpu_for_wake(thread, waker, endpoint):
    if thread has hard affinity only one CPU:
        return that CPU

    if endpoint has IPC_HINT_WAKE_AFFINE and waker exists:
        if waker.cpu is allowed and not overloaded by RT:
            return waker.cpu

    if thread.home_cpu is allowed and locally reasonable:
        return thread.home_cpu

    if thread.last_cpu is allowed:
        return thread.last_cpu

    return least_loaded_allowed_cpu_for_class(thread.class)
```

The scheduler should distinguish latency and throughput IPC:

- `IPC_HINT_LATENCY`: prefer same CPU or same cluster.
- `IPC_HINT_THROUGHPUT`: prefer separate CPUs in the same cluster when both endpoints are CPU-active.
- No hint: preserve last CPU unless load balancing says otherwise.

### 12.5 IPC Drain Quantum

A service thread woken for IPC should get a short uninterrupted chance to drain nearby work.

Rules:

- Applies only to `SCHED_SERVICE` or `SCHED_INTERACTIVE` threads marked `LATENCY_IPC_SERVER`.
- Initial quantum: 50–200 us.
- Runtime is fully charged.
- If the thread blocks again before consuming the quantum, no penalty.
- If the thread remains CPU-bound after the quantum, it returns to ordinary fair scheduling.

This encourages the intended pattern:

```text
wake -> drain ring -> reply/notify -> wait_doorbell
```

It does not reward:

```text
wake -> compute forever
```

### 12.6 Preemption on IPC Wake

A local IPC wake may request preemption if the receiver's effective key is sufficiently earlier than the current thread's key.

Suggested rule:

```text
preempt if:
  receiver.band > current.band

or if same fair band:
  receiver.effective_key + wakeup_preempt_margin < current.effective_key

or if explicit handoff succeeds
```

Initial wakeup preempt margins:

```text
interactive/service: 50 us
normal:             250 us
batch:              no wake preemption
```

This prevents every notify from turning into a context switch while still making latency IPC responsive.

---

## 13. Synchronous Handoff

### 13.1 Purpose

`notify_handoff` is for synchronous request/reply IPC where running the receiver immediately is better than waking it and letting the fair picker eventually choose it.

It is not the default notify path.

### 13.2 Eligibility

Handoff may succeed only if all are true:

- Sender and receiver are on the same CPU, or receiver can be legally moved to sender CPU without cross-CPU dequeue.
- Receiver is sleeping on the exact doorbell or endpoint.
- Endpoint has `IPC_HINT_HANDOFF_OK`.
- Sender holds notify/handoff rights.
- Receiver principal is not throttled.
- Receiver class is not lower than a policy minimum for the endpoint.
- Current CPU is not running admitted RT work that must continue.
- Receiver is not pinned to another CPU.
- Receiver is not already runnable elsewhere.

If any check fails, `notify_handoff` falls back to `notify` unless the caller requested strict handoff.

### 13.3 Semantics

On success:

```text
sender.state = BLOCKED_ON_REPLY
receiver.state = RUNNING
receiver.reply_target = sender
receiver.donation = max(receiver.effective_priority, sender.effective_priority)
receiver.handoff_deadline = now + min(sender_remaining_slice, HANDOFF_MAX_NS)
```

The receiver returns to userspace with register payload according to the IPC ABI.

The sender resumes when:

- Receiver calls `reply_handoff`.
- Receiver exits or dies.
- Handoff timeout/budget expires.
- Endpoint is revoked.

### 13.4 Donation

Donation is priority/latency donation, not CPU budget donation.

The receiver temporarily inherits the sender's effective latency priority for dispatch ordering. Runtime is still charged to the receiver's principal.

Donation is released when:

- Matching reply occurs.
- Receiver consumes the handoff quantum.
- Receiver blocks on an unrelated endpoint.
- Receiver exits.

V2 donation is non-transitive. If a donated receiver performs another handoff, the second handoff does not inherit the original sender's donation.

### 13.5 Handoff Abuse Control

A principal cannot use handoff to escape fairness.

Controls:

- Receiver runtime is fully charged.
- Receiver principal budget applies.
- Handoff quantum is capped.
- Handoff count and timeout metrics are tracked per endpoint and principal.
- Repeated handoff timeouts may disable handoff on that endpoint until userspace re-enables it.

---

## 14. Sleep and Wake State Machine

V2 should make runnable states more precise.

Recommended thread states:

```cpp
enum ThreadState : u8 {
    TS_RUNNING,
    TS_RUNNABLE_QUEUED,
    TS_RUNNABLE_PENDING_REMOTE,
    TS_BLOCKED,
    TS_SLEEPING_DOORBELL,
    TS_SUSPENDED,
    TS_DEAD,
};
```

The distinction between `RUNNABLE_QUEUED` and `RUNNABLE_PENDING_REMOTE` prevents duplicate enqueue bugs as wake paths become more concurrent.

### 14.1 Local Wake

```text
wake_local(t, reason):
    old = atomic_load(t.state)
    if old is DEAD or RUNNING or RUNNABLE_QUEUED:
        return

    if CAS(t.state, old, RUNNABLE_QUEUED):
        apply_wake_metadata(t, reason)
        enqueue_on_current_eki(t)
        maybe_request_preemption(t)
```

### 14.2 Remote Wake

```text
wake_remote(t, target_cpu, reason):
    old = atomic_load(t.state)
    if old is DEAD or RUNNING or RUNNABLE_QUEUED or RUNNABLE_PENDING_REMOTE:
        return

    if CAS(t.state, old, RUNNABLE_PENDING_REMOTE):
        apply_wake_metadata(t, reason)
        pending_push(target_eki.pending_wakes, t)
        send_ipi(target_cpu)
```

### 14.3 Pending Wake Drain

```text
drain_pending_wakes(cpu):
    list = atomic_exchange(cpu.pending_wakes.head, null)
    reverse list for FIFO order

    for t in list:
        if t.state == RUNNABLE_PENDING_REMOTE:
            t.state = RUNNABLE_QUEUED
            enqueue_on_local_eki(t)
```

The target CPU alone inserts into its run queues.

---

## 15. SMP and Work Stealing

### 15.1 Philosophy

Work stealing is a power and throughput mechanism, not a latency mechanism. IPC affinity should be allowed to keep hot endpoints together even when naive balancing would move them.

### 15.2 Stealable Work

Stealable:

- `SCHED_BATCH`
- `SCHED_NORMAL`
- Some `SCHED_SERVICE` only if not LPI-bound, not `IPC_HINT_NO_STEAL`, and not recently hot
- `SCHED_VM` only if vCPU is explicitly migratable

Not stealable:

- `SCHED_RT`
- Pinned threads
- Driver threads with bound LPIs
- Host-controlled RT vCPUs
- Threads inside active handoff
- Threads marked IPC-sticky within the recent hot window

### 15.3 Steal by Request

An idle CPU does not touch a peer's run queue. It sends a steal request.

```text
idle_cpu_steal(thief):
    victim = choose_overloaded_peer(thief)
    if no victim:
        return false
    push steal request onto victim.pending_steal_requests
    send_ipi(victim)
    return true
```

The victim CPU handles the request:

```text
handle_steal_request(victim, thief):
    candidate = select_stealable_thread(victim)
    if candidate:
        dequeue candidate from victim local fair queue
        candidate.cpu_id = thief
        candidate.state = RUNNABLE_PENDING_REMOTE
        pending_push(thief.pending_wakes, candidate)
        send_ipi(thief)
```

This preserves per-CPU queue ownership.

### 15.4 Load Metric

Initial load metric:

```text
load = runnable_weight_timeshare + runnable_weight_latency_nonsticky
```

Exclude:

- RT load.
- Pinned load.
- Idle.
- Sleeping threads.
- IPC-sticky service receivers within the hot window.

### 15.5 Hot IPC Stickiness

A thread may be temporarily sticky if:

- It was woken by the same endpoint repeatedly.
- It has `IPC_HINT_WAKE_AFFINE`.
- It blocks again quickly after running.

Suggested hot window:

```text
hot_until = now + 2 ms
```

Hot stickiness is advisory and may be broken under severe imbalance, except for hard affinity and no-steal flags.

---

## 16. Affinity

V2 distinguishes hard affinity and soft affinity.

### 16.1 Hard Affinity

Hard affinity is an enforcement constraint.

```text
thread may run only on CPUs in hard_affinity_mask
```

Used for:

- Pinned RT work.
- LPI-bound driver threads.
- vCPUs pinned by host policy.
- Bring-up debugging.

Changing hard affinity of a queued/running thread requires a scheduler control operation that coordinates with the owning CPU. Directly editing the mask is allowed only while suspended.

### 16.2 Soft Affinity

Soft affinity is a placement hint.

Fields:

```text
home_cpu
last_cpu
preferred_cluster
ipc_peer_cpu
soft_affinity_mask
```

Used for:

- IPC co-location.
- Cache locality.
- Energy policy.
- Avoiding unnecessary migration.

### 16.3 IPC Affinity Hints

The IPC layer may set affinity hints through scheduler-owned metadata:

```text
set_affinity_hint(endpoint, receiver_thread, mode)
```

Modes:

```text
COLOCATE_WITH_WAKER
KEEP_RECEIVER_HOME
SPLIT_FOR_THROUGHPUT
PIN_TO_CPU_REQUIRES_CAP
```

Only `PIN_TO_CPU_REQUIRES_CAP` creates a hard constraint.

---

## 17. VM Scheduling

### 17.1 vCPU Runner Model

V2 represents each vCPU as a host schedulable thread or as a thread-attached schedulable object. The initial implementation should use host vCPU runner threads because it fits the existing syscall and thread model.

Each vCPU runner has:

```text
class = SCHED_VM unless admitted as RT-vCPU
principal = VM principal
weight = VM-configured weight
budget = VM-configured budget
hard_affinity = host vCPU placement mask
```

### 17.2 Budget Meaning

VM budget is host CPU budget, not guest instruction budget and not guest-thread budget.

The host guarantees:

```text
this VM principal receives host CPU according to its budget and weight
```

The host does not guarantee:

```text
a particular guest thread receives CPU
```

That remains the paravirtual agent's responsibility.

### 17.3 VM Entry Budget

`run_vcpu` should evolve from “number of guest exits” to time budget:

```text
run_vcpu(vcpu, budget_ns, reason, deadline_ns, core_mask)
```

The scheduler arms a preemption timer or checks elapsed host time on VM exit.

### 17.4 RT vCPUs

A vCPU may be admitted as RT only with explicit host policy.

Rules:

- Treated as RT by host scheduler.
- Budgeted and deadline/admission checked.
- Not stealable unless explicitly migratable RT.
- Only guarantees vCPU time.
- Verification of guest RT placement is optional and sampling-based.

---

## 18. IRQ and Driver Threads

IRQ delivery posts a wake to a normal scheduler thread. There are no special invisible interrupt threads.

A driver thread may be:

- `SCHED_SERVICE` with `LATENCY_IPC_SERVER` for ordinary drivers.
- `SCHED_RT` for admitted hard latency devices.
- Hard-affine when bound to an LPI target.
- Marked no-steal while LPI-bound.

When a migratable driver thread moves, the IRQ routing layer updates the LPI redistributor target. Latency-critical drivers should normally be pinned with their LPI.

---

## 19. Scheduler Control ABI

V2 should add a scheduling control capability rather than making scheduling policy ambient.

### 19.1 Capability Types

Suggested capabilities:

```text
CAP_SCHED_CONTROL       global or supervisor-level scheduling control
CAP_THREAD_SCHED        authority over one thread's scheduling attributes
CAP_PRINCIPAL_SCHED     authority over one principal's budget/weight
CAP_ENDPOINT_SCHED      authority over one endpoint's IPC scheduling hints
CAP_VM_SCHED            authority over one VM's host scheduling attributes
```

### 19.2 Operations

```text
thread_set_sched(thread_cap, attrs)
thread_get_sched(thread_cap)
thread_set_hard_affinity(thread_cap, mask)
thread_set_soft_affinity(thread_cap, hint)
thread_set_latency_hint(thread_cap, hint)

principal_set_weight(principal_cap, weight)
principal_set_budget(principal_cap, period_ns, budget_ns)
principal_get_usage(principal_cap)

endpoint_set_sched_hint(endpoint_cap, hints)
endpoint_bind_server(endpoint_cap, thread_cap)
endpoint_enable_handoff(endpoint_cap, policy)

vm_set_sched(vm_cap, attrs)
vcpu_set_affinity(vcpu_cap, mask)
vcpu_set_budget(vcpu_cap, budget)
```

### 19.3 Unprivileged Defaults

A normal process may voluntarily lower its weight, increase batch behavior, or restrict its own affinity. It may not:

- Raise itself to RT.
- Increase principal budget.
- Pin to a CPU reserved for another principal.
- Mark arbitrary endpoints as handoff-capable.
- Disable stealing for normal work.

---

## 20. Pick Algorithm

### 20.1 Top-Level Pick

```text
pick_next(cpu):
    drain_pending_wakes(cpu)
    handle_pending_steal_requests(cpu)

    if cpu.current can continue and !need_resched(cpu):
        return cpu.current

    account_current(cpu)

    t = pick_rt(cpu)
    if t:
        return t

    t = pick_fair(cpu.latency_fair)
    if t:
        return t

    t = pick_fair(cpu.timeshare_fair)
    if t:
        return t

    maybe_send_steal_request(cpu)
    return cpu.idle
```

### 20.2 Fair Pick

```text
pick_fair(fair_band):
    p = fair_band.peek_min_principal()
    while p and p.throttled:
        fair_band.mark_ineligible_until_refill(p)
        p = fair_band.peek_min_principal()

    if !p:
        return null

    t = p.threads.pop_min_effective_key()
    if !t:
        fair_band.remove(p)
        return pick_fair(fair_band)

    fair_band.remove_or_rekey(p)
    return t
```

### 20.3 Requeue After Run

```text
requeue_current(cpu, t, delta_exec):
    account_runtime(t, delta_exec)

    if t.state != RUNNING:
        return

    if t.class == SCHED_RT:
        if rt_budget_remaining(t):
            enqueue_rt(t)
        else:
            throttle_rt(t)
        return

    update_thread_vruntime(t, delta_exec)
    update_principal_vruntime(t.principal, delta_exec)
    decay_wake_credit(t, delta_exec)

    if principal_budget_exhausted(t.principal):
        throttle_principal(t.principal)
        return

    enqueue_fair(t)
```

---

## 21. Notify Path Integration

### 21.1 Normal Notify

```text
notify(endpoint):
    validate cap
    db.counter++
    waiter = exchange(db.waiter, null)

    if !waiter:
        return OK

    reason = endpoint_to_wake_reason(endpoint)
    hint = endpoint.sched_hint
    target = target_cpu_for_wake(waiter, current_thread, endpoint)

    prepare_wake_metadata(waiter, reason, hint)
    wake_to_cpu(waiter, target)
    return OK
```

### 21.2 Handoff Notify

```text
notify_handoff(endpoint, regs):
    validate cap and handoff right
    receiver = endpoint.waiter

    if !handoff_eligible(current, receiver, endpoint):
        if strict:
            return E_WOULD_BLOCK
        notify(endpoint)
        return OK

    account_current_partial()
    block_sender_on_reply(current, endpoint)
    install_register_payload(receiver, regs)
    apply_donation(receiver, current)
    switch_to(receiver)
```

### 21.3 Reply Handoff

```text
reply_handoff(reply_cap, regs):
    sender = current.reply_target
    if !sender:
        return E_INVALID

    install_reply_payload(sender, regs)
    release_donation(current)
    wake(sender)

    if current consumed handoff quantum or sender should resume immediately:
        request_switch(requeue_current=true, reason=SWITCH_REPLY)
```

---

## 22. Preemption Rules

A running fair thread should continue unless one of these is true:

- It exhausted its slice.
- It voluntarily yielded.
- It blocked.
- Its principal became throttled.
- A higher band is runnable.
- A newly woken latency entity beats it by the wakeup preemption margin.
- A scheduler control operation changed its eligibility.
- The CPU must idle/suspend/stop.

RT preemption is immediate subject to architecture-safe return points.

IPC wake preemption is not automatic. It is conditional and margin-based.

---

## 23. Data Structure Changes

### 23.1 Thread

Add or split scheduling state:

```cpp
struct Thread {
    ...
    ThreadState state;
    SchedAttrs sched;
    ThreadSchedState sched_state;
    PrincipalId principal_id;
    AtomicThreadState atomic_state;
};
```

Avoid spreading scheduling fields across the base `Thread` structure. Keep hot scheduler fields cache-local.

### 23.2 Process

Keep process runtime and runnable count, but do not use process as the primary fairness unit.

```cpp
struct Process {
    ...
    PrincipalId principal_id;
    u64 runtime_ns;
    u32 runnable_threads;
};
```

### 23.3 Principal

Create an explicit principal object rather than using the principal enum as identity.

```cpp
struct Principal {
    PrincipalId id;
    PrincipalClass cls;
    PrincipalSchedState sched;
};
```

The existing enum values can remain principal classes, not principal identities.

### 23.4 Doorbell / Endpoint

Add scheduling metadata:

```cpp
struct Doorbell {
    ...
    u32 sched_hints;
    u32 bound_server_tid;
    u32 home_cpu;
    u64 wake_count;
    u64 handoff_count;
};
```

---

## 24. Metrics

Required V2 metrics:

### Per CPU

```text
context_switches
preemptions
rt_preemptions
fair_preemptions
ipc_preemptions
local_wakes
remote_wakes
pending_wake_drains
steal_requests_sent
steal_requests_received
steals_granted
steals_denied
idle_ns
busy_ns
```

### Per Principal

```text
runtime_ns
vruntime_ns
budget_used_ns
budget_throttles
runnable_threads
max_lag_ns
```

### Per Class

```text
runtime_ns
wake_latency_p50/p95/p99
slice_expirations
voluntary_blocks
```

### IPC-Specific

```text
doorbell_notify_calls
notify_no_waiter
notify_woke_thread
notify_handoff_success
notify_handoff_fallback
reply_handoff_count
handoff_timeouts
ipc_wake_credit_granted_ns
ipc_wake_credit_consumed_ns
ipc_wake_preemptions
same_cpu_ipc_wakes
cross_cpu_ipc_wakes
```

The performance harness should make these queryable through debug syscalls or a kernel trace ring.

---

## 25. Correctness Tests

### 25.1 Fairness Tests

1. Equal principal fairness: two principals, one thread each, equal weight.
2. Thread-count abuse: one principal with one thread, one principal with eight threads.
3. Weighted principal fairness: weights 1:2:4.
4. Budget throttle: principal with 20% cap cannot exceed cap.
5. Batch isolation: batch work must not starve normal work.

### 25.2 IPC Tests

1. Same-CPU doorbell wake latency under CPU load.
2. Cross-CPU doorbell wake latency under CPU load.
3. Latency endpoint gets bounded wake credit.
4. Batch endpoint gets no wake credit.
5. Service drains ring and blocks again quickly.
6. CPU-bound service loses IPC credit after drain quantum.
7. Handoff succeeds when receiver is sleeping on exact endpoint.
8. Handoff fallback works when receiver is remote or ineligible.
9. Donation ends on reply.
10. Donation does not chain.

### 25.3 SMP Tests

1. No direct cross-CPU runqueue mutation.
2. Remote wake uses pending-wake queue.
3. Work stealing moves only stealable fair work.
4. Pinned threads are never stolen.
5. LPI-bound driver thread is never stolen.
6. IPC-sticky receiver resists migration during hot window.

### 25.4 VM Tests

1. VM principal receives weighted fair CPU.
2. VM budget throttles vCPU runner.
3. RT vCPU admission rejects impossible reservations.
4. vCPU pinning is respected.
5. Guest exit count is not treated as CPU budget.

---

## 26. Performance Targets

Initial targets should be measured on the bring-up target and updated with real numbers.

```text
Local wake enqueue:             O(log runnable principals + log threads in principal)
Remote wake sender side:        O(1) push + IPI
Remote wake receiver drain:     O(W log N) for W drained wakes
Fair pick:                      O(log P + log T_p)
RT pick:                        O(1) with bitmap queues
Same-CPU notify no waiter:      under 100 cycles target after doorbell lock removal
Cross-CPU notify wake:          under 200 cycles target after doorbell lock removal
Same-CPU handoff:               under 400 cycles target with register payload
```

V2 does not need to hit the final cycle targets in its first implementation. It must preserve the structure that allows those targets later.

---

## 27. Migration Plan from Current Scheduler

### Stage 1: Accounting Foundation

- Add `sched_clock_ns()`.
- Replace tick-count runtime with nanosecond runtime.
- Add thread/principal `runtime_ns` and `vruntime_ns`.
- Keep current priority FIFO picker.
- Add metrics for wake latency and run duration.

### Stage 2: Fair Queue Within Current Priorities

- Replace FIFO queues for non-critical priorities with fair queues.
- Keep legacy priority bands.
- Implement virtual runtime for threads.
- Verify equal-thread fairness.

### Stage 3: Principal Fairness

- Add principal entities.
- Pick principal first, then thread.
- Verify thread-count abuse test.
- Add principal weights.

### Stage 4: Principal Budgets

- Enforce period/budget.
- Add throttle/unthrottle timer.
- Ensure IPC credit cannot run throttled principals.

### Stage 5: IPC Wake Credit

- Add endpoint scheduling hints.
- Add bounded wake credit.
- Add wake-preemption margin.
- Add IPC drain quantum.
- Measure same-CPU and cross-CPU wake latency.

### Stage 6: Handoff

- Add `notify_handoff` and `reply_handoff`.
- Implement non-transitive donation.
- Add timeout and fallback handling.
- Measure synchronous IPC.

### Stage 7: SMP Work Stealing

- Add steal requests and victim-side dequeue.
- Steal only normal/batch first.
- Add service stealing only after IPC stickiness is working.

### Stage 8: VM Integration

- Convert vCPU runner accounting from exit-count budget to time budget.
- Attach vCPU runners to VM principals.
- Add RT-vCPU admission later.

---

## 28. Rejected Alternatives

### 28.1 Global Run Queue

Rejected because it violates the per-CPU hot-path rule and creates cross-CPU contention.

### 28.2 Strict Priority Everywhere

Rejected because it allows starvation and lets high-priority service loops dominate normal work indefinitely.

### 28.3 IPC Boost as Real Priority Promotion

Rejected because it lets request storms create hidden priority inversion and budget bypass. V2 uses bounded wake credit instead.

### 28.4 Stealing Directly from Peer Queues

Rejected because it requires cross-CPU queue locking or unsafe concurrent mutation. V2 uses steal by request.

### 28.5 Fairness Per Thread Only

Rejected because principals with more threads would get more CPU.

### 28.6 Scheduler Inspecting Ring Contents

Rejected because it violates the IPC contract and makes the kernel depend on userspace protocol layout.

---

## 29. Open Questions

1. Should V2 implement deadline RT immediately, or start with fixed-priority RT plus budget?
2. Should `SCHED_SERVICE` default weight be 1536 or 2048 during transition from current `PRIO_SERVICE`?
3. What is the first hardware target's realistic handoff cycle budget?
4. Should wake-affine placement be opt-in per endpoint only, or default for all latency doorbells?
5. Should VM vCPU runners be normal `Thread`s permanently, or become a separate schedulable object later?
6. How much IPC drain quantum is enough for console/nameservice without harming normal fairness?
7. Should principal budget refill be exact per CPU or approximate with local buckets in V2.0?

---

## 30. V2 Contract

The V2 scheduler contract is:

1. RT work runs first only when explicitly admitted and budgeted.
2. Non-RT work is fair by principal, not by raw thread count.
3. Threads inside a principal are fair by weight.
4. IPC wakeups can improve dispatch latency but cannot create unaccounted CPU time.
5. Handoff is explicit, same-CPU-first, budgeted, and bounded.
6. Cross-CPU wake and steal paths preserve per-CPU queue ownership.
7. VM execution is scheduled by the host at the vCPU runner level.
8. Service authors who use rings, doorbells, affinity hints, and handoff correctly receive low latency; arbitrary IPC patterns receive correctness, not special optimization.

That is the scheduler version of the Midix performance contract: fast for conforming IPC, fair for everyone else, and explicit about every exception.

