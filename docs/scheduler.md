# Ressha / Eki scheduler

Ressha has one CPU-owned station (`Eki`) per online CPU. Native RT uses fixed
priority FIFO queues. The other working bands share weighted principal fairness,
then weighted thread fairness; idle work runs only when no other band is eligible.
The station owns its heaps, dispatch state, and accounting clocks. Remote CPUs
publish wakes and steal requests, rather than editing those heaps.

## Fair scheduling

Each principal has one local `service_vruntime_ns` across latency, VM latency,
fair, VM fair, and batch. Running time increases it by
`elapsed_ns * NICE_0_WEIGHT / principal_weight`. Each band heap orders principals
by this clock. The picker compares eligible principals across bands, so a busy
latency process cannot permanently starve a fair process.

Within a principal, each thread increases its own vruntime by
`elapsed_ns * NICE_0_WEIGHT / thread_weight`. The least effective thread key wins;
equal service and thread keys use this band precedence:

1. Native latency
2. VM latency
3. Native fair
4. VM fair
5. Batch

Idle is a separate final fallback. Native RT precedes all these bands and is
available only to kernel or trusted principals. There is no hard RT admission
or deadline scheduler.

A principal's thread watermark uses thread keys, including its running thread;
it never uses the principal service clock. This distinction preserves relative
thread weights. Principal service is normalized against the station's service
watermark when work joins. These are local CPU fairness clocks; an aggregate
principal budget, when configured, is enforced across CPUs.

Legacy thread creation maps priorities as follows:

| Priority | Band | Thread weight | Hint |
|---|---|---:|---|
| IDLE | Idle | 0 (accounting falls back to 1024) | Normal |
| BACKGROUND | Batch | 512 | Batch |
| NORMAL | Fair | 1024 | Normal |
| SERVICE | Latency | 1536 | IPC server |
| CRITICAL | Latency | 2048 | Interactive |

Trusted process principals default to weight 2048; sandboxed principals use 1024.
`PrincipalClass` controls authorization; unique principal IDs define fairness
buckets. Principal generations prevent a recycled ID from inheriting local state.
Contexts retain their principal, so a transferred context can outlive its creator
without referring to a subsequently recycled ID.

## Wake publication and frame ownership

`Thread::execution_cpu` identifies the CPU that still owns the live exception
frame. Entering a blocking syscall changes state before the syscall return frame
is saved. A wake during that window records `deferred_wake_cpu`, under the thread
lifecycle lock, and publishes no runnable node yet.

`post_syscall` saves the frame, charges execution through the blocking operation,
and releases execution ownership before publishing the deferred wake. Remote
publication uses an MPSC CAS stack and a wake IPI. The receiving CPU detaches the
stack with acquire semantics, reverses it to preserve FIFO order, and inserts the
threads into its own queues. Thread state uses acquire/release publication too;
state publication alone does not transfer frame ownership.

Wake selection considers hard affinity, wake-affine hints, doorbell home CPU,
thread home/last CPU, and published load. Publication rechecks affinity while
holding the lifecycle lock, since a blocked thread can be rebound after selection.
Queue membership rejects duplicate insertion. Termination is collected by the
owning station; a retiring thread leaves the active address space before storage
is reclaimed. An idle station retains no finished thread or process reference.

## Budgets and slices

Principal and context budgets are independent. An exhausted running thread is
requeued, then becomes ineligible until its period rolls over. Ineligible nodes
remain in their queues, so period refill never needs to edit another CPU's heaps
or reconstruct lost queue membership.

Charging and period rollover use per-principal and per-context locks. Period
starts advance by whole periods rather than drifting to the next timer tick.
A late accounting timestamp cannot underflow that arithmetic. Budgeted contexts
require a nonzero period and `budget_ns <= period_ns`.

The picker refills the principal/context it examines, including after an idle
period. Ordinary ticks do not scan all principal and context tables. A CPU with
only budget-limited work waits for a timer or event, then retries selection.

Slices are derived from band target latency, thread weight, and the number of
queued peers, with 0.5 ms minimum granularity and band-specific maxima. The
current generic timer checks at about 10 ms, so these are scheduling targets,
not sub-millisecond timing guarantees. Budgets can overshoot by the accounting
checkpoint interval, including simultaneous execution on several CPUs.

## IPC credit and handoff

Doorbell and IRQ events can grant bounded, per-thread wake credit. Credit lowers
the thread's effective key and decays as it executes. IPC servers use a nominal
100 us drain window. Blocking ends that window so the next wake starts a fresh
one. A timeout on the drain or handoff window clears the remaining advantage.

Synchronous handoff requires the doorbell hint, a sleeping same-CPU receiver,
compatible affinity, no existing sender/receiver reply chain, and available
principal budget. Dispatch also checks the receiver's context budget. The direct
path sets a normal slice and a nominal 100 us handoff deadline; expiry is checked
at the next accounting/timer checkpoint. The reply link survives quantum expiry,
while the donated scheduling key does not. Ineligible handoffs use ordinary wake
and scheduling rather than bypassing budget checks.

## Work stealing and data layout

Idle stations request work from the peer with the largest published lower-band
thread count. This counts threads, rather than principal heap entries, so a
single multithreaded principal can supply work. Victims select only eligible
lower-band threads whose affinity includes the destination CPU. Pinned and
IPC-hot work is excluded; VM fair runners require an explicit stealable flag.
Every request outcome releases the thief's request slot.

Migration transfers thread lag relative to the source thread watermark; the
receiving station rebases it against its own watermark. Absolute clocks from
unrelated stations are not compared directly.

Stations and cross-CPU coordination areas are cache aligned. Peers read atomic
load summaries, never live heap sizes. Dispatch state precedes the cold fixed
thread-heap arrays. Per-principal RT thread heaps were removed because native RT
uses the station FIFO queues. Heap mechanics live in `libk/indexed_heap.h`, with
scheduler index/membership adapters in `fair_queue.cc`.

Accounting and dispatch use the thread's retained process owner instead of
looking it up through the global process registry. Address-space activation
reads the active TTBR0 and skips `vm_activate` when it already matches; restoring
an address space changed by a syscall still uses the existing VM activation path.
Initial station dispatch restores the complete saved EL0 trap frame, including
when the first runnable thread has already executed on another CPU.

## Validation and metrics

Run `python3 tests/test_scheduler.py` for the host regression probe. It compiles
scheduler code with address/undefined-behavior sanitizers, replacing only
privileged clock, interrupt, wait, and EL0-entry boundaries in a temporary copy.
It checks heap ordering/indexes, weighted shares, starvation, principal reuse,
budget refill, affinity, remote frame publication, idle reclamation, and concurrent
charging of a shared principal/context. QEMU lifecycle and supervision fixtures
provide kernel execution and teardown coverage.

`timer_preempts` counts timer requests caused by slice or handoff expiry;
`cpu_tick_count` counts timer interrupts. Runtime counters measure charged elapsed
nanoseconds. `fairness_max_lag` is the spread of nonzero cumulative principal
runtime; it is diagnostic and does not normalize weights or sleeping time.
