# IPC implementation and evolution

Start with [the current IPC contract](architecture.md#ipc-shared-data-explicit-wakeups). The first section describes data ownership; subsequent sections retain dated refactor/design checkpoints. Later changes can supersede earlier implementation details. Measurements live in [performance](performance.md).

- [IPC data layout and throughput](#ipc)
- [IPC execution paths for Cortex-A53](#ipc_floor)
- [IPC and scheduler refactor](#ipc_refactor)

<a id="ipc"></a>
## IPC data layout and throughput

<a id="ipc-ipc-data-layout-and-throughput"></a>

IPC uses shared SPSC pages for payloads, doorbells for sleeping/waking, and
capabilities for authority and lifetime. The shared page keeps its existing
256-byte header and 64-byte slots. Head, tail, and sleep coordination occupy
separate cache lines. Kernel metadata is private to the kernel.

<a id="ipc-ownership-and-working-sets"></a>
### Ownership and working sets

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

<a id="ipc-notification-and-waiting"></a>
### Notification and waiting

The current [IPC and scheduler refactor](ipc.md#ipc_refactor) adds paired atomic
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

<a id="ipc-creation-sharing-and-teardown"></a>
### Creation, sharing, and teardown

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

<a id="ipc-payload-batching-and-ordering"></a>
### Payload batching and ordering

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

<a id="ipc-validation-and-measurements"></a>
### Validation and measurements

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

<a id="ipc_floor"></a>
## IPC execution paths for Cortex-A53

<a id="ipc_floor-ipc-execution-paths-for-cortex-a53"></a>

The primary device is the Huawei P20 Lite (Kirin 659, Cortex-A53). These changes
use ARMv8.0 instructions and the mandatory eight-bit ASID facility. They do not
require LSE, newer wait instructions, speculative SIMD ownership, or a DMA
engine. QEMU Cortex-A53 validates execution and ordering protocols; TCG timings
do not establish physical-device latency or the architectural floor.

<a id="ipc_floor-address-spaces"></a>
### Address spaces

The 64 reserved live process IDs are also ASIDs 1–64. ASID zero belongs to the
kernel root. Process IDs remain reserved until the old process's execution and
lifetime references retire. Creating a new incarnation completes `ASIDE1IS`
before publishing it. A tagged context is `vm_context(root, pid)`; physical
page-table walkers continue to receive the untagged root.

Normal switches write TTBR0 and execute ISB without invalidating translations.
Mapping changes invalidate only the affected ASID. Root teardown detaches each
subtree and completes its shootdown before reclaiming its pages. Changes that
affect all kernel roots retain their global shootdowns. Kernel identity leaves
are non-global too: late MMIO mappings can differ between roots while TTBR1
separation remains unimplemented. Temporary VM kernel-root scopes restore the
caller's tagged context.

`test_asid.py` executes production activation/shootdown/setup code at EL1 and
EL2 on Cortex-A53, alternating different backing pages at the same VA and then
reusing all 64 ASIDs. It also checks that activation contains no TLBI.

<a id="ipc_floor-copied-records-and-credits"></a>
### Copied records and credits

`MX_RING_PACKED_FRAMES` opts a ring into packed framing. The formerly reserved
header line carries the feature and a separate producer sleep request. Existing
head/tail/receiver-flag offsets and the 256-byte header size are unchanged.
Feature-zero rings retain the original interoperable FIRST/LAST slot protocol.

Packed logical messages above 48 bytes have one 16-byte header and contiguous
payload, with at most two copies at ring wrap. Their occupancy is
`ceil((16 + payload_size) / 64)` cursor units. A 4096-byte message uses 65 units,
one publication and one release instead of 86 individually framed slots.
Publication requires room for the whole record. Larger-than-capacity records
fall back to bounded legacy fragmentation. Readers validate bounds, feature,
flags and available occupancy before copying or advancing ownership. Invalid
framing poisons the reader.

The default framed geometry is order 7: an 8 KiB data area per direction. This
fits a full 4 KiB record while limiting the A53 cache footprint. Existing inline
service rings retain order 6. Bulk services can explicitly choose a larger
supported order; allocation and mapping remain outside the exchange path.

Optional space doorbells let a producer arm, compare the tail, and block for
the actual required room. Readers publish tail progress before claiming the
producer's one-shot wake request. A separate line prevents contention with the
receiver's data wake request. Both directions preserve the acquire/release RMW
handshake; a speculative zero-flag load is not a substitute. No per-message
cache clean/invalidate is used for coherent normal CPU memory.

<a id="ipc_floor-kernel-entries-and-synchronous-rpc"></a>
### Kernel entries and synchronous RPC

`WAIT_RING(ring, doorbell, cursor, direction)` compares a retained kernel-backed
ring cursor while registering its doorbell waiter. It needs no preceding
counter-read syscall and never dereferences an arbitrary userspace pointer.
Readable waits compare head; credit waits compare tail. Callers arm their
shared wake request first. Publication preceding registration makes the syscall
return immediately; publication after registration produces a notification.

`NOTIFY_WAIT(notify, ring, wait, cursor, flags)` validates all three capabilities
before any notification side effect. It combines notification with conditional
waiting. `REPLY_WAIT` selects reply mode. The production `Channel::call` uses
ordinary combined notification/waiting when endpoint handles are available;
legacy/deadline endpoints keep their existing behavior.

Trusted creators may configure scheduling hints in `sys_doorbell_create(hints)`.
Handoff is an explicit server protocol: the receiver must use `reply_handoff`
or reply-and-wait. It cannot be enabled transparently for an arbitrary server.
Requests use same-core handoff only when affinity, ownership, principal and
context budgets permit it. Already-readable replies skip request handoff.
Reply-and-wait rearms the server before directly returning to its caller.
Cross-core and ineligible exchanges retain the normal wake/dispatch path.

An exclusive sender may defer a wake claimed by `FrameWriter` or `Ring::try_push`
and carry it into its next receive syscall. It must flush before waiting for
space, switching to polling, terminating, or abandoning the operation. The
benchmark adapter follows these rules, including its final `Closed` response.
It defers only same-core wakes. Cross-core publication wakes the receiver
immediately so production and consumption overlap. When both peers explicitly
negotiate polling, readers omit credit notifications because producers never
park for space in that policy.
Blocking cross-core waits may poll for at most 64 cursor checks before parking;
same-core waits poll zero times. This avoids waking a core for data arriving
just after its empty check. The budget is recorded in benchmark metadata, and
does not turn blocking waits into unlimited spin waits. Energy is not measured.
The closing server also completes any synchronous reply token before exiting;
otherwise teardown correctly aborts that token even if response bytes exist.
An end-to-end A53 integrity test covers this shutdown handshake, partial stream
windows, both placements and polling as well as blocking data/credit waits.

<a id="ipc_floor-scheduler-assembly-and-working-sets"></a>
### Scheduler, assembly and working sets

The scheduler saves the outgoing full GPR and SIMD state before relinquishing
execution ownership, as before. ARM return assembly restores straight from the
selected thread's saved frame, eliminating the copy back to the CPU exception
stack. Host probes retain a compatibility wrapper that materializes that copy.
Huawei's FIQ return mask remains in the assembly path.

Reply and RT queue links fill the 24-byte alignment gap following `CpuContext`,
avoiding accesses beyond the cold SIMD file. Direct handoff bypass is limited to
eight consecutive transfers when other runnable work exists. Queued real-time
work always forces normal selection. Budgets, donation deadlines, termination
and deferred remote-wake ownership checks remain active.

Steady-state IPC, clock and yield dispatch avoid the large cold VM/control
dispatcher. Syscall statistics use separate cache lines per CPU and atomic
load/store updates under exception-entry IRQ exclusion. General IPC counters
retain their atomic updates where kernel notifications can be reentrant.

Aligned byte copies use scalar AArch64 LDP/STP pairs without overreading,
touching FP/SIMD state, or depending on unaligned kernel accesses. Unaligned and
short copies retain bounded byte handling. The Huawei build also schedules C++
instructions for Cortex-A53 without enabling a newer ISA.
Inline frames have separate inlined readers/writers; packed copying and partial
fragment handling stay in non-inlined helpers. Empty/small messages therefore
avoid the bulk handlers' register spills and instruction footprint.

With capture disabled, Huawei syscall/timer hooks return after their one-time
records instead of acquiring the boot logger's global lock on every entry.
CPU-zero watchdog polling continues under exception-entry IRQ exclusion.
Capture mode retains its original serialized observation/reset path.

<a id="ipc_floor-verification-and-measurements"></a>
### Verification and measurements

Run correctness first:

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 scripts/test/simd.py --build out/architectural --cpus 2 4 --entry-el 1 2
```

New coverage includes packed and fragmented concurrency, cursor wrap, buffer
guards, malformed records, real blocking data/credit races, combined syscall
authority failures, handoff/reply ownership, fair competitors and RT precedence.
The normal lifecycle bundle additionally exercises 100 process restart cycles.

Performance collection follows implementation and correctness validation:

```sh
python3 scripts/test/ipc-bench.py ame --cpu cortex-a53 --build-type Release \
  --output out/ipc-results/final
```

Use a freshly built original snapshot with the same compiler, QEMU, CPU model,
workload, parameters and build type for the before/after comparison. Retain raw
samples, independent runs, binaries and logs. The supplied older A57/TCG
collection is historical context and is not a matched A53 baseline.

<a id="ipc_floor-wake-policy-and-fpsimd-ownership"></a>
### Wake policy and FP/SIMD ownership

Two costs sit outside the copy path on a Cortex-A53: the one-shot wake handshake
and the per-switch vector state.

A ring whose consumer is a negotiated poller sets `Ring::peer_polls`. Publishers
then only release-store the head: no claim exchange on the shared flags line and
no doorbell notification, because that consumer never arms its sleep flag. The
contract is static per case; dropping it before a reply that the peer may be
waiting for (see the Finish path in `benchmarks/ipc/workload.h`) is mandatory.

`Ring::wait_cursor(poll_budget)` relaxes before arming. The benchmark selects
1024 iterations for cross-core placement: a 256-byte round trip completes inside
the budget, so the pair does not park and re-wake per message, while a genuinely
idle wait still falls through to the kernel. The value is a latency/CPU trade:
64 leaves 256-byte RPC at ~35 us, 1024 brings it to ~3.3 us because the peers
stop paying two sleeps and two IPIs per round trip.

FP/SIMD state is owned lazily. `CPACR_EL1.FPEN` stays at `0b01` (EL0 traps, EL1
usable for the save/restore routines) and a thread's 528-byte state is live only
while it runs. The first EL0 vector instruction after a switch takes the 0x07
trap, restores the thread's state, opens `FPEN` and re-executes the instruction.
Threads that never touch a vector register - every kernel path, and the scalar
service code - therefore cross a switch with zero FP traffic instead of 528
bytes stored and 528 bytes loaded. A thread that does use FP becomes sticky:
from its second dispatch the switch path restores eagerly, so it never pays more
than one trap in its lifetime and behaves exactly as the eager scheme did.

<a id="ipc_floor-policy-surface"></a>
#### Policy surface

`SYS_IPC_POLICY` (75) is the only knob a process has over its own wake policy:
`op 0` reads the principal's poll budget, `op 1` sets it, `op 2` returns park and
wake counts in x0/x1. The kernel clamps the budget to
`kIpcPollBudgetMax` (4096) and refuses to address any principal but the
caller's own, so a stale or hostile profile cannot turn a wait into an
unbounded spin. Defaults are zero: a process that never calls the syscall parks
exactly as before.

`mochi::Ring::refresh_poll_budget()` seeds a ring from that policy once, and
`wait_readable_auto()` uses it. Ring helpers that know the peer runs on the same
core must keep the budget at zero - the peer cannot make progress while this
thread spins - which is the same reason the benchmark only sets it for
different-CPU placement.

Parks and wakes are counted per principal at the doorbell park and wake sites.
The counts are the input a userspace controller needs; the kernel itself keeps
no heuristic and no timer.

<a id="ipc_floor-the-controller"></a>
#### The controller

`mochi::BudgetController` (userland, header-only) scores one window of 64 waits:
if at least half parked, the peer keeps answering just after we slept, so the
budget doubles; a window with no parks only counts toward a quiet streak, and
eight consecutive quiet windows give back one eighth. Everything else is a dead
band, which is what stops per-message oscillation. The shape is deliberate:
growing late costs latency on the next message, decaying late only costs cycles,
so growth is fast and decay is slow. Floor 64, cap 4096, seeded from the
principal policy; the ring reports whether a wait actually parked
(`wait_readable(budget, &parked)`) and the caller applies a change through the
same clamped syscall.

Measured against a fixed 1024 on the same six-pair harness: RPC blocking within
+3..+12%, stream/256 11% faster, everything else inside noise. A fixed value is
still marginally better for one steady workload - the controller exists for
processes that mix bulk and latency traffic, where no single constant is right.

<a id="ipc_floor-provisioning"></a>
#### Provisioning

`config/ipc-profile.conf` (`service=budget`) is compiled by
`scripts/gen-ipc-profile.py` into a small binary object with the same format as
`mochi/ipc_profile.h`: magic, version, count, 16-byte checksummed entries keyed
by FNV-1a of the service name. A malformed line, a duplicate name, more than 32
entries or a budget above 4096 fails the build instead of shipping a silent
default. `init` decodes the blob once, looks each service up by name and adds a
single `ipc.budget` BootInfo entry; `mochi::boot::map()` applies it through the
normal clamped syscall, so there is no privileged "set another principal"
operation anywhere.

A writable amefs copy would need `init` to hold an io.fs session and to read
other principals' counters, i.e. a new aggregation authority. That was left out
deliberately: the image-provisioned file plus the in-process controller already
covers the measured need, and the profile stays a hint that cannot widen the
kernel's bound. `msh ipc` prints the running budget and the park/wake counts.

<a id="ipc_floor-three-way-check"></a>
#### Three-way check

Zero budget (policy off), fixed 1024, and the adaptive controller, three
interleaved boots each, different-CPU, medians:

| case | zero | fixed | adaptive |
|---|---:|---:|---:|
| rpc blocking 0 | 28,791 | 523 | 521 |
| rpc blocking 256 | 33,393 | 3,387 | 3,593 |
| rpc blocking 8 | 29,092 | 711 | 834 |
| stream blocking 0 | 895 | 134 | 156 |
| stream blocking 8 | 1,024 | 191 | 203 |
| stream blocking 256 | 2,245 | 2,005 | 1,910 |

The controller lands within a few percent of the best fixed value on the RPC
cases and beats it on stream/256, while giving budget back when nothing parks.

<a id="ipc_floor-supervisor-reads-why-write-back-is-not-wired-yet"></a>
#### Supervisor reads (why write-back is not wired yet)

`SYS_IPC_POLICY` op 3 returns the park/wake counts of a direct child
(`supervisor_pid` must match the caller). It is read-only, needs no capability
and exposes nothing the supervisor does not already own by lifetime. That is the
aggregation primitive a profiler would use.

The write-back loop itself is not wired, because the two halves do not meet:
`init` is the only process that supervises every service, but it holds no io.fs
session — it only routes client endpoints — so it cannot read or write the
profile file at boot. Adding that session means teaching `init` to be a client of
a service it is still launching, which risks a boot-order deadlock; a service
that does have fs access (msh) is not the supervisor of anything else. Until one
of those is solved deliberately, the profile stays image-provisioned and the
controller adapts within each boot.

<a id="ipc_floor-the-profiler-proxy"></a>
#### The profiler proxy

`init` supervises every service, so it can read their counters and apply their
budgets, but it holds no io.fs session. `ipc.profiler` closes that gap: a small
service with fs access that only touches the profile file. The transport is two
shared pages and two doorbells - a request page and a reply page - so there is
no ring protocol to get wrong and no chunking beyond the 520-byte bound.

File I/O is deliberately inline-only: reads step in <= 32-byte requests and
writes in <= 24-byte requests, which the fs replies carry in `inline_data`. The
first version used `read_bytes()`, which switches to an arena above 32 bytes and
failed on the very first boot-time read; the inline path removes that dependency
entirely.

At boot `init` asks the profiler what the file says and decodes it, so services
launched afterwards inherit it. Two seconds later it samples each child: the
runtime diagnostic table maps a service name to a live pid, the supervisor
counter read gives parks/wakes for that pid, and a park-dominant child is moved
to 1024 while a bulk child is moved to 0. The result is applied through the
supervisor-gated set and written back through the profiler, logged as
`init: ipc.profile applied=N entries=M` and `ipc.profiler: stored=`.

One caveat that is environmental, not architectural: `/data` is non-durable on
this image, so the file survives a service restart within a boot but not a
power cycle in QEMU. The read path handles a missing file (status -4) as an
empty profile rather than an error.

<a id="ipc_refactor"></a>
## IPC and scheduler refactor

<a id="ipc_refactor-ipc-and-scheduler-refactor"></a>

The subsequent [Cortex-A53 IPC execution-path changes](ipc.md#ipc_floor) add ASIDs,
packed records, producer credits, combined waits and bounded direct replies.
The results below describe the supplied earlier refactor, not that later pass.

This pass reduces copied-message overhead and scheduler work while preserving
the existing IPC workload contract, capability checks, affinity, budget policy,
and exception-frame ownership rules. Native Linux/Mach performance leadership
remains a measurement target; QEMU results cannot establish it.

<a id="ipc_refactor-changes"></a>
### Changes

- `mochi/framed_ring.h` implements allocation-free copied framing with bounded
  publication and receive batches, defaulting to 16 slots. Singleton messages
  publish immediately. Reassembly copies payload directly into caller-owned
  storage before releasing slots. FIRST/LAST, sequence, opcode, buffer bounds,
  zero-length control messages and cursor wrap remain checked. The transport
  interoperates with the previous slot-at-a-time wire format.
- Ordinary ring, multi-ring and inbox waits now share `ring_publish_sleep`.
  Both publisher and waiter use acquire/release exchanges on the sleep flag.
  If publication wins, arming acquires its head publication; if arming wins,
  publication claims the flag and notifies. The former release-store arm could
  miss publication. An intermediate stress run demonstrated a sleeping client
  with 22 unread reply fragments; that stalled run is excluded from results.
- Empty remote-wake and steal queues use read checks before destructive atomic
  exchanges. Remote publishers signal only empty-to-nonempty transitions. A CAS
  racing a drain retries against null and therefore requests a fresh IPI.
  Release publication still precedes the GIC's store-completion barrier and SGI.
- Thread wake/ownership fields occupy one aligned 64-byte line. Identity/wait
  references and accounting occupy separate lines; saved general and SIMD
  registers are cold and aligned. Thread size is 1,280 bytes. Process identity,
  accounting and supervision occupy separate lines in a 192-byte object.
  Peer-readable CPU load summaries no longer share local dispatch counters.
- Doorbell counter writers retain the IRQ-masked doorbell lock and release
  publication, but avoid an additional atomic increment loop inside that lock.
  Waiters still acquire the counter and register under the lock.
- Scheduler wake metadata reuses one timestamp. Tick accounting reuses its
  already captured time. Generation-checked CPU-local principal weights avoid
  repeated shared-policy reads; default-weight accounting avoids division.
- `libk::CounterScale` precomputes an exact reduced tick-to-nanosecond ratio.
  Integral ratios need a multiply; other ratios use multiply-high and exact
  remainder correction. Read-side division and floating point are absent.
  Tests compare full-width counter values and thousands of frequencies against
  a 128-bit arithmetic oracle, including platform frequencies and overflow.
- Counter reads, interrupt instructions, store synchronization and cache hints
  live in `libk/cpu.h`. Dispatch prefetches the selected register state while
  preparing the context switch. Board FIQ policy remains in the kernel.

<a id="ipc_refactor-coherency-and-correctness"></a>
### Coherency and correctness

CPU ring pages use normal, inner-shareable mappings. Acquire/release publication
establishes visibility and ownership. Cache prefetch is only a hint; it neither
publishes a frame nor authorizes its reuse. DMA cache policy remains separate.
No per-message cache clean/invalidate is added to coherent CPU communication.

The scheduler still saves the outgoing trap and SIMD frames before releasing
execution ownership. Deferred remote wakes are published only after that point.
Shared budgets, principal generations and bounded handoff donation remain checked.
The original WFI idle sequence and publish-before-IPI barrier are retained.

Blocking stress covers single-ring, multi-ring and inbox waits with real host
threads. Protocol tests cover malformed fragments, poisoned readers, backpressure,
partial progress, old/new interoperability and concurrent cursor wrap. Scheduler
tests cover concurrent multi-producer wake batches, exact membership and renewed
IPIs after drain. Doorbell tests include counter wrap and notification races.

<a id="ipc_refactor-reproduction"></a>
### Reproduction

The normal benchmark selects the production batched adapter:

```sh
python3 scripts/test/ipc-bench.py ame --output out/ipc-results/after-refactor
python3 scripts/test/ipc-bench.py compare \
  benchmarks/ipc/baselines/2026-10-18/ame-qemu.json \
  out/ipc-results/after-refactor/results.json
```

An optional `--reference-framing` control retains the old fragment-at-a-time
adapter while using the current kernel and repaired wait helpers. It isolates
framing changes from scheduler changes; it is not selected for ordinary runs.
The runner records implementation/library identities and publication limits.
The portable workload, payload generation, full validation, clock instrumentation
and operation counting are unchanged. See [the benchmark guide](performance.md#ipc_benchmark)
for interpretation and matching native-machine requirements.

The runner now defaults to `CMAKE_BUILD_TYPE=Release`. The original saved baseline
used RelWithDebInfo. Comparisons across these collections include the compiler
configuration change as well as the code changes; they do not isolate refactoring
alone. Development diagnostic counters remain enabled and are explicitly recorded.

<a id="ipc_refactor-final-release-results"></a>
### Final Release results

The final run used `CMAKE_BUILD_TYPE=Release`, verified `-O3 -DNDEBUG` kernel
compiler commands, the unchanged `-O2` portable workload, two cortex-a57 vCPUs,
EL2, and the same three independent observations per case as the initial run.
No tests or builds ran concurrently with the accepted measurement collection.
The old baseline used RelWithDebInfo, so these gains include that build change.

| Same-CPU workload | Initial baseline | Final Release | Observed speedup |
| --- | ---: | ---: | ---: |
| 256-byte RPC | 160.73 µs/RTT | 98.19 µs/RTT | 1.64× |
| 4096-byte RPC | 1539.29 µs/RTT | 621.06 µs/RTT | 2.48× |
| 256-byte streaming | 29391 ns/message | 16355 ns/message | 1.80× |
| 4096-byte streaming | 456129 ns/message | 265740 ns/message | 1.72× |

Same-CPU 4 KiB RPC diagnostic medians fell from 34.52 to 13.14 selected-thread
switches, 18.09 to 6.76 doorbell sleeps, and 5.53 to 2.48 yields per operation.
These counts support the reduction in scheduling and per-fragment wake work.

This is not an across-the-board win. Different-CPU 4 KiB RPC changed from
119.20 to 153.09 µs, an observed regression of about 28%; its three-run aggregate
intervals overlap. Other small-message cases also remain noisy or regress in
their medians. Several cases exceed 10% run CV. Do not infer native CPU costs or
Linux/Mach superiority from this TCG collection. Native testing must resolve
cross-core batching, cache transfer costs and scheduler wake latency before that
performance goal can be considered achieved.

The [complete raw Release collection](../benchmarks/ipc/baselines/2026-10-18/ame-refactored-release.json)
retains every sample, round, counter, build identity and warning. The result
archive also preserves serial logs and exact kernel/bundle binaries. The initial
baseline is retained unchanged. Intermediate candidates and the stalled run are
excluded from the accepted comparison.

<a id="ipc_refactor-verification"></a>
### Verification

- Before the scheduler extension, the full host suite passed: 276 tests,
  39 skipped. After the scheduler changes, focused scheduler/IPC sanitizer
  probes passed, including multi-producer IPI coalescing, fairness, budgets,
  exception-frame ownership, doorbell wrap and 2,000 notification races.
- The exact counter-conversion oracle passed with platform, power-of-two,
  arbitrary 32-bit frequencies and full-width tick values.
- Blocking single/multi-ring/inbox stress, framed wire interoperability,
  malformed messages, buffer guards and concurrent cursor wrap passed.
- All 12 benchmark integrity/native-adapter tests passed.
- Four-core normal boot and lifecycle regression passed. SIMD register-state
  regressions passed at two/four cores and EL1/EL2 entry.
- The exact measured Release kernel subsequently passed four-core normal boot,
  lifecycle and EL1/EL2 SIMD checks; its raw image hash remained unchanged.
- The final Release IPC matrix completed all 20 cases with three independent
  observations per case, without a stall or payload-validation failure.
- Optimized ARM disassembly confirms the clock read uses MRS, MUL/UMULH and
  correction arithmetic with no UDIV on the read path. No coherency barrier or
  capability/ownership check was removed to obtain these results.
