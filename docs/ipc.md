# IPC data layout and throughput

IPC uses shared SPSC pages for payloads, doorbells for sleeping/waking, and
capabilities for authority and lifetime. The shared page keeps its existing
256-byte header and 64-byte slots. Head, tail, and sleep coordination occupy
separate cache lines. Kernel metadata is private to the kernel.

## Ownership and working sets

| Data | Owner / layout | Work moved out of the hot path |
| --- | --- | --- |
| Doorbell | One aligned 64-byte object | Cached lifetime identity replaces registry searches |
| IPC counters | One 64-byte row per CPU | Diagnostic totals aggregate on demand |
| Capability borrow | 32-byte retained snapshot | Reserved slot bytes are never copied |
| Capability allocation | 128-byte occupancy bitmap plus count | No scan across 1,024 slot cache lines |
| Deadline waits | CPU-owned bitmap, deadline column, retained-link column | Empty ticks skip clock conversion and record scans |
| Object registry | 256 pointer buckets; unchanged 32-byte records | Cold searches visit the selected bucket |
| Ring byte runs | At most 16 slots per publication | Peer cursor reads and wake RMWs amortize across a run |
| Console reader | Eight copied slots per drain batch | Head reads and tail publication amortize across callbacks |

Doorbell identity, synchronization, wake policy, and counters still fit one line.
The cached record uses former padding, with no separate cold allocation. Ring
kernel objects grow from 24 to 32 bytes to retain their identity too. These
internal layouts do not change ring pages, syscall records, or handle encoding.

Each CPU's eight IPC counters fit one line. The historical `doorbell_wake_count`
and `notify_woke_thread` accessors counted identical events and now share one
counter. Relaxed atomics preserve correctness if a local IRQ also notifies;
unrelated CPUs write different lines. Diagnostics sum those rows when requested.

The timed-wait table keeps 64 slots per CPU. Its active bitmap admits a free
slot with a bit scan and visits only occupied slots during polling. The compact
table occupies 3,136 bytes per CPU, versus 4,096 previously. Stale notifications
are reaped on the owning CPU's next poll, preserving reference lifetime and
capacity recovery. Empty polling returns before touching DAIF or the clock.

## Notification and waiting

The current [IPC and scheduler refactor](IPC_REFACTOR.md) adds paired atomic
sleep arming, copied-message batching, coalesced remote wake IPIs, exact clock
scaling, and cache-line separation of scheduler working sets.

For a syscall from the current process, execution already retains its owner.
IPC validates the handle directly in that process's capability table; it adds
only the capability borrow's reference. Calls targeting another process retain
the normal process lookup. Generation, type, and rights checks remain mandatory.

Notification increments the counter and detaches/retains its waiter under the
doorbell guard, then releases that guard before waking. Cached thread records
avoid a global identity search. The scheduler's `wake_ipc` applies credit,
chooses a CPU, and publishes the wake under one thread lifecycle guard, replacing
three separate acquisitions. Its placement, affinity, budget, and deferred-frame
rules match the individual scheduler operations.

An untimed wait whose counter already changed returns before IRQ masking or
doorbell locking. Actual registration rechecks the counter while holding the
doorbell guard. A timed wait whose condition is already satisfied also returns
success when the timer table is full. It needs no new timer slot.

Registration retains the capability's known object record. A timer additionally
retains both objects through their cached identities. Its atomic sequence check
prevents an old timeout from canceling a later wait by the same thread. Replacing
the previous wait reference releases it after the new doorbell guard is dropped,
so final destruction executes outside that guard.

Remote publication still waits until the executing CPU saves the complete
exception frame. The counter/waiter handshake, wake coordination, and capability
table remain guarded. This rewrite does not make every IPC operation lock-free.

## Creation, sharing, and teardown

Object factories store the creator record immediately. Capability allocation and
sharing can retain that known record directly, and creation transfers exactly
one creator reference. VM channel endpoint cleanup uses those records as well.
Sharing resolves its destination once and attenuates the source rights.

The object registry uses a fixed bucket index with full target comparison in
each chain. Record size and atomic retain/drop semantics remain unchanged. A
zero-count retiring record cannot be acquired again. Registry insertion, target
lookup, and final unlink retain their existing lock and destruction ordering;
the index reduces traversal length rather than adding an unlocked reclamation
protocol. Worst-case collisions still require traversing a bucket's chain.

Capability allocation preserves its rotating slot order using the occupancy
bitmap. A cached used count handles capacity checks without scanning entries.
Generations start at one on first allocation and advance on revoke, so process
creation needs no second strided initialization pass over the table.

Teardown visits occupied entries in bitmap-word batches. At most 64 references
(512 bytes) are detached under a guard, then released outside it. This reduces
empty-table clear from 2,048 guard acquisitions to 16, touches no empty slots,
and permits final callbacks to access capability tables without recursive
locking. Revoke-by-target uses the same live-slot traversal. Individual slots
remain 64 bytes and are now explicitly cache aligned; the bitmap/control prefix
adds 184 logical bytes per table, making its size 65,728 bytes.

## Payload batching and ordering

Byte-run producers fill up to 16 available slots, release-publish the head once,
and perform one sleep-flag exchange. Partial acceptance, zeroed payload tails,
per-chunk yield limits, and wrapping 64-bit sequences remain intact. Singleton
control messages still publish immediately. Explicit frame-batch APIs preserve
protocol-owned sequence tokens and publish only the valid prefix that fits.

Batch consumers acquire the producer head, copy a bounded set of frames to
caller-owned storage, then release-publish the tail. Console draining uses an
eight-slot stack batch and returns that space before potentially slow UART or
terminal callbacks. Callbacks receive frames in order within each batch; all
frames in that batch have been consumed before the callbacks begin. There is
no persistent cursor cache to invalidate when another value view accesses the
same ring.

The producer keeps its unconditional sleep-flag RMW; it does not replace it with
a speculative waiter load. Lockless wait/wake tests require careful ordering,
as explained in the [Linux waitqueue documentation](https://kernel.org/doc/html/next/driver-api/basics.html).
The page ABI and existing release/acquire publication protocol are preserved.

## Validation and measurements

Run `python3 -m unittest discover -s tests -p test_ipc.py` for real kernel IPC,
scheduler, capability, and object code with substituted privileged boundaries.
It checks rights attenuation, stale handles, failed allocation/capacity cleanup,
borrowed ring storage, full remote frame publication, timeout reuse, 64-slot timer
capacity, request/reply handoff, exact per-CPU totals, and 2,000 cross-CPU
notification/registration races. The IPC and concurrent ring probes also pass
ThreadSanitizer. Ring tests cover one-shot wake coalescing, partial batches,
malformed-frame boundaries, zeroed tails, and a concurrent 1 MiB transfer through
64-bit cursor wraparound. Capability probes check sparse slot reuse, exhaustion,
generation changes, concurrent revoke, and reentrant destruction.

The final code passed 47 focused host tests with ASan/UBSan where applicable.
Seven QEMU checks passed: EL1/EL2 full boots, eight-core EL1 boot, both eight-core
pinned workloads, and four-core lifecycle/supervision regressions. Native Release
stack reports show 64 bytes for notify, 144 for wait, 112 for timer polling, and
624 for batched capability cleanup, excluding callees.

Reproduce host timing with:

```
python3 scripts/test/benchmark-ipc.py --baseline-dir out/ipc-opt/baseline --runs 7
```

The snapshot is the preceding v7 implementation. Seven-run medians are recorded
in `out/ipc-opt/host-performance.json`:

| Host workload | v7 | v8 | Throughput ratio |
| --- | ---: | ---: | ---: |
| Notify, small registry | 17.14 ns | 14.07 ns | 1.22x |
| Independent notifications, four CPUs | 33.82 ns/event | 1.49 ns/event | 22.70x |
| Empty timer poll | 21.49 ns | 2.49 ns | 8.63x |
| Empty capability table clear | 2,035.15 ns | 16.12 ns | 126.25x |
| Full capability table rejection | 319.24 ns | 2.76 ns | 115.67x |
| Ring, 16-slot burst on one thread | 5.96 ns/slot | 5.52 ns/slot | 1.08x |
| Ring, separate producer/consumer threads | 37.60 ns/slot | 9.13 ns/slot | 4.12x |

The singleton ring case is approximately unchanged: 2.35 versus 2.28 ns.
Concurrent cases divide elapsed wall time by all events/slots; they measure
throughput, not individual call latency. Ring callbacks checksum every payload
byte. The IPC fixture substitutes process-directory/allocation boundaries and
privileged IRQ/clock/MMU operations; production scheduling and IPC logic run.
These are warm host measurements, not Cortex-A53 or phone boot predictions.

Additional cases deliberately keep 2,048 unrelated lifetime records alive.
Notify then improves from 1,699.79 to 14.09 ns; doorbell create/share/revoke from
9,873.15 to 82.65 ns; ring create/share/revoke from 9,800.56 to 134.27 ns. These
stress cases expose the removed linear traversals and should not be generalized
to a lightly populated registry.

The signed native package is `out/anne-boot-speed-package-v8`, run tag
`anne-boot-speed-v8`, image SHA-256
`f5fa04b6f17f39a4b20f095decd9766b1df1f1391fcf144abcd9ca4dec6aaed2`.
Flashed and boot-verified on the ANE-LX1: one boot reached msh in 0.29 s
(kernel to services ready: 299.881 ms), with all eight CPUs online and their
MMU/instruction/data caches enabled. The USB shell responded without boot fault
markers. Phone IPC throughput remains unmeasured; v7 is preserved.
This image retains the sink-only USB policy, which disables keyboard host power.
