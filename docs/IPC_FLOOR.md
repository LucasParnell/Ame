# IPC execution paths for Cortex-A53

The primary device is the Huawei P20 Lite (Kirin 659, Cortex-A53). These changes
use ARMv8.0 instructions and the mandatory eight-bit ASID facility. They do not
require LSE, newer wait instructions, speculative SIMD ownership, or a DMA
engine. QEMU Cortex-A53 validates execution and ordering protocols; TCG timings
do not establish physical-device latency or the architectural floor.

## Address spaces

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

## Copied records and credits

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

## Kernel entries and synchronous RPC

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

## Scheduler, assembly and working sets

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

## Verification and measurements

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

## Wake policy and FP/SIMD ownership

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

### Policy surface

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

### The controller

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

### Provisioning

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

### Three-way check

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

### Supervisor reads (why write-back is not wired yet)

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

### The profiler proxy

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

At boot `init` encodes the compiled profile, asks the profiler to store it, then
asks it to load it back and decodes what returns - a full write/read/parse cycle
through the real filesystem, logged as `stored=` / `loaded=` by the service.
Applying the loaded values to already-running children needs a pid lookup
(the supervisor counters are pid-addressed), which is the next step.
