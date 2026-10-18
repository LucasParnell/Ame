# Release boot performance

ANE-LX1 hardware measurement, 2026-10-15. Latest measured image run tag:
`anne-usb-auto-v5`, SHA-256:
`1a4fa94b211699c25809e27f542927f85b6b1e892e95c32d04b2838353594826`.
The image is installed on the phone. Signed package and verified stock restore:
`out/anne-usb-auto-package-v5`. One measured boot reached msh in 0.30 s
(kernel to services ready: 308.521 ms). The user confirmed stable keyboard
typing and normal Mac reconnection without the Mac charging from the phone. The v8 IPC image initially measured 0.29 s; a later user-reported
0.36 s v8 boot could not be captured after the handset entered fastboot.

| Image | msh boot banner | DT probe checkpoints |
| --- | ---: | ---: |
| Previous release, `anne-cdc-hw-v1` | 1.46 s | 736.67 ms |
| Early instruction cache, `anne-boot-speed-v1` | 0.89 s | 345.75 ms |
| Compact DT transform, `anne-boot-speed-v2`, boot 1 | 0.81 s | 257.419 ms |
| Same image, boot 2 | 0.81 s | 257.435 ms |
| Same image, boot 3 | 0.81 s | 257.418 ms |
| Single-pass cached DT transform, `anne-boot-speed-v3`, boot 1 | 0.53 s | 4.555 ms |
| Same image, boot 2 | 0.53 s | 4.546 ms |
| Same image, boot 3 | 0.53 s | 4.548 ms |

The probe average fell from 257.424 ms to 4.550 ms: 56.58 times faster,
or a 98.23% reduction. These are actual phone checkpoint intervals, including
the temporary mapping's setup and teardown, with the same Release configuration.

The banner uses the unchanged `DBG_BOOT_CS` counter, from `kernel_main` to msh
startup, at centisecond precision. It excludes firmware, decompression, and BSS
clearing before `kernel_main`. Checkpoint intervals use the phone's 1.92 MHz
counter. Kernel-main to all-services-ready averaged 538.365 ms for v3 and
374.266 ms for v4. These measurements establish an improvement, not a minimum possible
boot time.

The next optimization, v4, batches permanent identity-map construction:

| Image | Page-table construction | msh boot banner |
| --- | ---: | ---: |
| v3, boot 1 | 92.833 ms | 0.53 s |
| v3, boot 2 | 92.823 ms | 0.53 s |
| v3, boot 3 | 92.807 ms | 0.53 s |
| v4, boot 1 | 1.037 ms | 0.37 s |
| v4, boot 2 | 1.037 ms | 0.37 s |
| v4, boot 3 | 1.036 ms | 0.37 s |

The page-table interval averages 92.821 ms before and 1.036667 ms after:
89.54 times faster, a 98.88% reduction. Service startup also fell from an
average 189.294 ms to 120.135 ms because each process creates an address space.
The hardware-discovery probe remains about 4.56 ms.

## Changes and ownership

The phone entry path invalidates the instruction cache and enables it before
BSS clearing and DT probing. During hardware discovery, a temporary identity
mapping enables data caching for the kernel and immutable DTB. It is removed
before allocator initialization; the existing page-table setup still performs
the permanent MMU transition. The handoff checks still reject MMU/data-cache-on
entry. The [AArch64 boot protocol](https://docs.kernel.org/arch/arm64/booting.html)
allows instruction caching with the MMU off and requires no stale kernel-image
instruction entries.

Hardware discovery is a bounded DTB-to-`HwInfo` transformation:

- A contiguous array of 64-byte aligned records holds active node metadata.
  It has the same maximum depth as the structural walker, with no heap allocation.
- `reg` and `ranges` use bounded read-only views into the DTB; views never escape
  into the published hardware description.
- One path buffer and per-depth lengths replace copied full paths per node.
  An overflowing path cannot match a truncated console name.
- Each compatibility list is traversed once, against a fixed table that preserves
  selection priority.
- Hardware, `/chosen`, aliases, and UART candidates are collected in one walk.
  Fixed-size candidate/path storage handles the common case; overflow performs
  exact streaming selection rather than truncating or silently losing a UART.
- A walk-local cache validates and classifies property names by their interned
  strings-block offsets. Full tags make hash collisions harmless. Node names
  are checked and measured in one scan. No cache survives a different input blob.
- Candidate hardware data stays local until the entire walk succeeds, including
  the phone's RAM limit. Malformed input leaves the caller's output unchanged.
- Disabled ancestors remain disabled, and a memory-controller node cannot be
  mistaken for a root memory bank.

DTB input is immutable and discovery runs on the sole boot CPU before SMP.
No cacheable alias or new concurrent writer is introduced. The existing SMP
startup cleans the kernel, including `g_hw`, to PoC and executes `DSB SY` before
CPU_ON; its release/acquire handshake publishes startup state to secondary CPUs.
CPU/PSCI and reservation walkers also use bounded local records over the same
read-only input.

The temporary tables use complete 2 MiB blocks plus 4 KiB boundary pages. Only
the exact kernel/DTB page ranges become Normal WBWA, inner-shareable memory;
the DTB is read-only and execute-never, and peripherals remain Device-nGnRnE.
Invalid, overlapping, or unsupported extents retain the uncached boot path.
Table walks and software accesses have matching cache/shareability attributes.
Before enabling caching, stale lines are invalidated by VA. A stackless assembly
routine cleans dirty kernel/stack/output lines to PoC and drains writes with
`DSB SY` before restoring the original MMU/cache registers. It then invalidates
the clean DTB through physical access, without creating another VA alias.

An initial hardware boot exposed a teardown permission bug: `DC IVAC` requires
write permission even at EL1, so it must not target the read-only DTB while
translation is active. Moving this invalidation after the MMU-off transition
fixed the phone boot while retaining read-only cached parsing. QEMU does not
enforce that permission check; the CPU test also inspects the linked restore
routine's ordering. See the [Arm architectural correction, D23577](https://documentation-service.arm.com/static/67ab274f6dbc975ccea916d7).

## Permanent identity-map construction

The DOP boundary is a compact array of 24-byte `{begin, end, attributes}` runs.
Cold code resolves RAM/MMIO/framebuffer/noncached policy at attribute boundaries,
sorts the resulting runs, rejects overlaps, and merges adjacent equivalent runs.
The plan is cached across process creation and invalidated when the registered
noncached ranges change. The VM lock serializes plan creation and invalidation.

The emitter consumes runs in slices bounded by one L3 table. It walks upper
levels once per slice and streams contiguous 64-bit PTE stores, rather than
repeating the full lookup and attribute policy for each 4 KiB page. A complete
new leaf needs no preliminary zeroing; partial leaves clear only their uncovered
edges. Other tables are cleared with 64-bit stores instead of byte stores.
Existing leaves reject a duplicate batch before writing it.

This removes repeated work but retains 4 KiB leaves and private identity tables
per address space. A fully present 512 MiB bootstrap RAM mapping still needs
131,072 PTEs, 256 L3 tables, and 1 MiB of leaf-table storage. Future block mapping
needs a safe split path for noncached retagging, including page-table access while
breaking a block. Shared kernel TTBR1 mappings, ASIDs with SMP invalidation, and
restricting EL1 execution to kernel text remain separate work. The current kernel
identity map is supplied by TTBR0; the source comments now reflect that fact.

## Reservation batch: v5 first phone boot

v5 replaces the second full uncached DT traversal with a bounded array of
16-byte `{base, size}` records, kept separate from hot `HwInfo` data. The existing
cached hardware-discovery walk also collects `/reserved-memory` tuples; the short
header reservation list is decoded in the same cache scope. After `phys_init()`,
allocator setup streams the records directly. No extra cache transition, heap
allocation, node graph, or per-node property copy is introduced. The existing
temporary-cache teardown publishes the batch to PoC before MMU-off consumption.

A source pointer marks a batch complete only after both sections validate and
console selection succeeds. Capacity overflow, malformed input, or a different
DTB pointer selects the original streaming consumer. The collector preserves the
existing reservation policy: header entries first, two address/two size cells,
disabled and nested reserved nodes, the 16-level consumer bound, partial trailing
tuple handling, zero filtering, duplicates, and source order. Console replay
cannot collect a reservation twice. The phone's 28 ranges match an independent
decode of the original consumer's addresses, sizes, and ordering exactly.

The signed Release package is `out/anne-boot-speed-package-v5`, run tag
`anne-boot-speed-v5`, native image SHA-256
`5bd60f0d2385760246f64a1f87ca29a6609308969b28df0cdb38056b13cfc1c3`.
Its raw kernel hash matches the final Release build. Following the user's flash
request, v5 was installed and verified at the USB shell. The first phone boot
measured reservations at **0.469 ms**, versus the v4 three-boot average of
75.105 ms (160.14 times faster for this first sample). Hardware discovery was
4.718 ms and page-table construction 1.041 ms. msh reported **0.29 s**, and
kernel-main to all-services-ready was 299.170 ms. This is one boot, not a v5
average. All 28 reservation records were applied; all eight CPUs reported MMU,
instruction cache, and data cache enabled, without a fault. The sink-only marker
and complete diagnostic report were present. The phone was left running v5.

For v5, 37 focused tests passed. The production transform runs with ASan/UBSan
over read-only input, including full register arrays, disabled ancestors, header
entries, malformed structures/headers, exact capacity, overflow, and console
replay. Full Debug QEMU boots passed at EL1, EL2, and eight-core EL1. Two additional
full boots used injected reservation DTBs: a complete two-record batch and a
129-record overflow, both reaching all-services-ready with the full reservation
count. Compiler stack-usage reports keep discovery plus the walker/callbacks below
the existing 16 KiB boot stack; the reservation array has static storage.

## Scheduler DOP: v6 ready, phone measurement pending

The hot principal record is now exactly **64 bytes, aligned to 64 bytes**, down
from the original 3,264-byte record containing six thread heaps. Selection clocks,
generation, bounded queue counts, membership, and heap indexes fit one line.
The active service/thread clocks retain their full 64-bit precision. Unused
per-band clock/watermark bookkeeping and a duplicated local runtime total were
removed; authoritative thread, process, and global principal runtime remain.
Fixed-capacity heaps occupy a parallel array. Both arrays are CPU-owned and
indexed by principal ID; generation reuse resets the hot state. Compile-time assertions
enforce the layout, and the linked Release scheduler storage is 64-byte aligned.

Heap size/band fields sit near their root pointers. Dispatch combines principal
metadata reads under one lock, skips empty bands, and avoids clock conversion for
unbudgeted eligibility. Accounting reuses its locked weight snapshot. The switch
path retains the running thread's cached lifetime record instead of searching
the global object registry; a same-thread syscall return also avoids copying
the saved trap frame. Deferred wakes still publish only after a full frame save,
and temporary syscall address-space changes still restore the caller's mapping.

The total station shrank from 212,928 to 207,808 bytes, saving 40 KiB across eight
CPUs. Queue capacity remains 64 threads per principal/band. Locks still protect
global budgets and lifetime references; cross-CPU wakes and fairness policy retain
their existing rules. See [the scheduler notes](../../docs/scheduler.md).

The final code passed 39 focused tests, including ASan/UBSan scheduler and
capability probes, 30,000 heap operations, 10,000 cross-band dispatches, exact
queue capacity/overflow, weighted fairness, budget rollover, principal reuse,
remote frame publication, and retirement. Seven QEMU checks passed: full Debug
boots at EL1, EL2, and eight-core EL1; eight-core EL1/EL2 workloads checking pinned
workers, preemption, budgets, and diagnostics; and four-core process lifecycle
and supervision regressions. Synthetic Anne builds use the same private EL3
compile fixture described above; no EL3 diagnostic command is exercised.

The signed Release image is `out/anne-boot-speed-package-v6`, run tag
`anne-boot-speed-v6`, native image SHA-256
`efdbd4237cedcc553f1a24d4a312e4d309e14687b3a01db83d583f282d2f9a03`.
It matches the final raw kernel. **v6 has not been flashed**, at the user's request
to leave it ready for later; phone boot timing remains pending. Its rebuilt bundle
is 917,504 bytes versus v5's 913,408 bytes, including other workspace changes.
The host benchmark isolates the scheduler sources instead.

Reproduce the warm host dispatch stress comparison with
`scripts/test/benchmark-scheduler.py --baseline-dir out/sched-opt/baseline`.
It uses 200,000 dispatch rounds with 1/8/32 runnable threads and 1,024 unrelated
live object records, taking medians over five runs. Hardware boundaries are
substituted; these are scheduler stress costs, not phone boot predictions.
Final timings are in `out/sched-opt/host-performance.json`; the isolated 128-byte
versus 64-byte layout comparison is in `out/sched-opt/cacheline-comparison.json`.

| Runnable threads | Original dispatch round | Final dispatch round | Speedup |
| --- | ---: | ---: | ---: |
| 1 (reselect itself) | 886.2 ns | 79.6 ns | 11.13x |
| 8 | 910.0 ns | 121.1 ns | 7.51x |
| 32 | 909.5 ns | 127.8 ns | 7.12x |

The isolated one-line/two-line comparison is approximately even: 79.0 versus
78.2 ns with one thread, 121.6 versus 123.2 ns with eight, and 125.3 versus
127.4 ns with 32. These small differences do not establish a latency gain from
the layout alone; its demonstrated benefits are the 64-byte record and lower
static memory use. The larger stress gain also includes the cached lifetime
lookup, combined metadata reads, and reduced return/dispatch work above.

## Scheduler lock-free dispatch: v7 ready, phone measurement pending

Normal preemption/yield now accounts, requeues, and activates CPU-owned threads
without taking lifecycle locks. Remote wake/bind paths reject running/queued
threads before inspecting mutable dispatch fields. Blocking, queue-overflow
parking, and deferred remote wakes keep their saved-frame/lifecycle handshake.
Allocation, teardown, stealing, and object creation/final unlink still use locks;
this is a lock-free ordinary dispatch path, not an entirely lock-free kernel.

The DOP change moves principal runtime into CPU-owned, cache-aligned rows indexed
by principal ID. Diagnostics aggregate on demand. Principal generation/weight
publish atomically in one 64-bit word. Shared budgets use aligned 128-bit CAS
windows containing full-width used time and period start, so rollover cannot
erase a concurrent charge. Cached object retains and nonfinal releases are atomic;
target lookup cannot resurrect a record awaiting final unlink. The hot principal
record remains exactly 64 bytes and each station remains 207,808 bytes. The
changed global principal/context/runtime/guard arrays use 10 KiB less BSS than v6.

The scheduling-context syscall keeps its existing 80-byte wire record. Kernel
budget alignment is private, and the kernel explicitly translates policy fields.
Compile-time checks pin the wire size/offsets and require native lock-free
128-bit atomics. The linked AArch64 Release image uses LDXP/STXP sequences and
has no unresolved atomic-library symbols; it needs no LSE extension.

Seven-run medians against the captured v6 sources are in
`out/sched-lockfree/host-performance.json`:

| Host case | v6 | v7 | Ratio |
| --- | ---: | ---: | ---: |
| Dispatch, 1 runnable thread | 80.4 ns | 73.9 ns | 1.09x |
| Dispatch, 8 runnable threads | 119.6 ns | 114.0 ns | 1.05x |
| Dispatch, 32 runnable threads | 131.0 ns | 121.0 ns | 1.08x |
| Shared unbudgeted principal, 4 CPUs | 87.3 ns/charge | 5.7 ns/charge | 15.32x |
| Shared principal + context budgets, 4 CPUs | 111.3 ns/charge | 109.0 ns/charge | 1.02x |

The concurrent cases report wall time divided by all charges: throughput, not
one thread's latency. Each CPU makes 100,000 charges. Budgeted results vary enough
that the 2% median difference does not establish an improvement; those shared
atomic windows still contend. Dispatch improves modestly, while unbudgeted
accounting benefits substantially from removing the shared write owner.
Hardware clock/IRQ/address-space boundaries are substituted and process-owner
aggregation is absent in this host fixture. These are not phone boot timings.
Reproduce with `scripts/test/benchmark-scheduler.py --baseline-dir
out/sched-lockfree/baseline --runs 7`.

The final source passed 39 focused tests, including ASan/UBSan scheduler and
capability probes, plus a ThreadSanitizer scheduler run. New checks cover eight
concurrent budget chargers, full-width rollover and saturation, CPU runtime
aggregation/reuse, dispatch with lifecycle guards held, simultaneous remote
wake/bind attempts, and deferred publication when a full queue parks its runner.
Seven final QEMU checks passed: three full boots, two eight-core EL1/EL2 pinned
workloads, and four-core lifecycle/supervision regressions. Synthetic Anne builds
retain the existing private EL3 compile fixture; no EL3 command is exercised.

The signed Release package is `out/anne-boot-speed-package-v7`, run tag
`anne-boot-speed-v7`, native image SHA-256
`1c62fc1b135e4eade9a163da8c30739a596f666c233d96b9180fda5aed66eb81`.
Its raw kernel matches the packaged build. **v7 has not been flashed**; v6 remains
available too, following the request to leave the handset/image for later.
Results and source hashes are in `out/sched-lockfree/validation.json`, with
package state in `out/anne-device-private/boot-speed-v7/state.json`.

## IPC DOP rewrite: v8 flashed and boot verified

IPC now keeps one 64-byte metrics row per CPU, a compact occupancy index for
capability allocation/teardown, 32-byte retained capability snapshots, and
CPU-owned timer tables with separate deadline/link columns. Doorbells remain
exactly 64 bytes with cached lifetime identity; ring objects cache identity in
32 bytes. Normal caller resolution uses the executing process directly, and
doorbell wake metadata/routing/publication uses one thread lifecycle guard.
Cold object lookup uses 256 buckets with unchanged 32-byte lifetime records.
Byte-run producers publish up to 16 slots together and console readers drain
eight-slot batches. The shared page/handle ABI and saved-frame handshake remain.

Seven-run host medians versus v7 show: small-registry notify 17.14 -> 14.07 ns;
empty timer poll 21.49 -> 2.49 ns; empty capability clear 2,035.15 -> 16.12 ns;
two-thread ring throughput 37.60 -> 9.13 ns/slot. Singleton ring latency is
approximately unchanged. Independent four-CPU notifications improve from
33.82 to 1.49 ns/event in aggregate throughput. Stress cases with 2,048 unrelated
objects also improve substantially from removing linear registry searches.
These are substituted-hardware host workloads, not phone timings. Full workload
definitions and caveats are in [the IPC notes](../../docs/ipc.md).

The final source passed 47 focused host tests, IPC/ring ThreadSanitizer probes,
and seven QEMU checks including both eight-core workloads and process
lifecycle/supervision. Results, stack reports, snapshots, and source hashes live
in `out/ipc-opt`. The signed Release package is
`out/anne-boot-speed-package-v8`, run tag `anne-boot-speed-v8`, native image SHA-256
`f5fa04b6f17f39a4b20f095decd9766b1df1f1391fcf144abcd9ca4dec6aaed2`.
v8 was flashed and boot-verified on the ANE-LX1. One boot reached msh in
0.29 s; kernel to services ready was 299.881 ms. All eight CPUs were online
with MMU, instruction cache, and data cache enabled, and the USB diagnostic
shell responded without boot fault markers. This single sample does not
establish an IPC throughput gain on the phone. The image retains sink-only
USB operation, so it does not provide keyboard host power. v7 is preserved.
Package state and capture are in `out/anne-device-private/boot-speed-v8`.

## Automatic USB switching and startup batching

The initial automatic switching build added independent Type-C control register
round trips and measured 0.36 s to msh (368.234 ms kernel to services ready).
Reading the contiguous ID/Modes/Control bytes, writing the Modes/Control image
and verifying that image takes three I2C transactions instead of six. The
revised image measured 0.30 s (308.783 ms); the reconnect revision also measured
0.30 s (309.569 ms). Kernel initialization stages remained effectively unchanged.
These are single-boot samples, not averages. Negotiation timers run after service
readiness and do not hold the shell startup path.

The user verified that auto-v2 powers the keyboard and accepts typing, then
reported the Mac accepting source power after the swap back. Auto-v3 adds a
USB data-connection generation check and explicitly restarts Unattached.Sink
after removing host power. Its boot and initial Mac peripheral selection are
verified. The user reported keyboard power cycling with auto-v3. Its retained
log shows successful keyboard enumeration followed by disconnect roughly one
second into each source session. Auto-v4 qualifies latched charger faults
against live OTG_ON, follows the vendor recovered UVP/SCP check, and records
source-stop and connection-epoch details. It measured 0.30 s / 309.113 ms, with
the user again reported cycling. The detailed auto-v4 capture proves the order:
charger software sees `status=0x40 faults=0x08` (healthy live OTG plus latched
OCP), cuts source power, HID disconnects, and then the role guard reacts.
Auto-v5 permits recovered OCP/UVP/SCP at the first healthy maintenance sample
only; stopped boost, OVP/overtemperature, I2C failure and recurring faults still
shut down. Hardware current limits remain at the vendor 500 mA encoding. It
measured 0.30 s / 308.521 ms. The user confirmed stable keyboard typing and
Mac reconnection without receiving power from the phone. The retained capture
shows recovered startup OCP acknowledged with boost healthy, a 6.10 s interval
from keyboard-ready to physical unplug, then peripheral mode with source off
and responsive CDC. It also records one initial stopped-boost/SCP shutdown
and retry before the successful keyboard session; initial attachment can
therefore take longer than the normal negotiation window.
Implementation and policy limits are documented in [the CDC notes](CDC_SERIAL.md).
Results and immutable package state are in `out/anne-device-private/usb-auto-v5`.

## Verification and remaining costs

The production parser passed ASan/UBSan tests with its input mapped `PROT_READ`:
alias resolution, property ordering, nested console paths, disabled ancestors,
deep and overflowing paths, malformed/truncated data, and compatibility priority.
The original and revised parsers returned identical RAM, UART, GIC, and USB fields
for the captured phone DTB.

For v2, 41 focused host tests passed across DT probing/formats, CPU topology, USB,
serial sender, msh input, and app streaming. Three QEMU boot checks passed:
EL1, EL2, and eight-core EL1. The existing QEMU EL3 diagnostic code lacks its
request-buffer constant in the synthetic build, so these boot-only checks used
a private compile fixture for that constant; no EL3 diagnostic command was run.
The QEMU cache flag was restored afterward.

All eight phone CPUs reported MMU, instruction cache, and data cache enabled
after each v2 and v3 boot. For v2, app tunnel start/run/stop passed with native test exit zero
and memory released. CDC accepted a command after a USB bus reset. The sink-only
Type-C marker remained present.

For v3, 29 focused tests passed across the production DT transform, formats,
CPU topology, and temporary cache tables/CPU transitions. The new cases cover
late console selection, alias/candidate/path-pool overflow, every identity-map
page's permissions, invalid extents, repeated cache entry, and complete register
restoration. Full Debug QEMU boots passed at EL1, EL2, and eight-core EL1 using
the same private EL3 compile fixture. The fixed Debug image also booted on the
phone, returned a complete diagnostic report, and accepted `help` before Release
measurement. All three Release boots were fault-free, retained the sink-only
Type-C marker, and reached the USB shell; two were ordinary shell-requested reboots.

For v4, 30 focused tests passed, including ASan/UBSan execution of the production
identity-map builder for generic, Anne, and synthetic Anne policies. Independent
table decoding checks every page against the original attribute policy, dirty
allocation pages, unmapped holes, allocation failures, duplicate-batch rejection,
user W^X and ownership, and noncached retagging across existing and new spaces.
Full Debug QEMU boots passed at EL1, EL2, and eight-core EL1. Three Release phone
boots reached msh, retained the sink-only marker, and reported MMU, instruction
cache, and data cache enabled on all eight CPUs without a fault.

The host benchmark of the captured 475,354-byte phone DTB improved from a median
486.077 us to 154.627 us (3.14 times), with identical hashes of every semantic
`HwInfo` field. Reproduce using `scripts/test/benchmark-dtb.py` and optionally
`--baseline-dir` pointing to saved `probe.cc`/`fdt.cc`. Host timings describe
warm host caches; use the phone checkpoints for boot-performance claims.

Separate reservation parsing still takes about 75.1 ms in v4; page-table
construction now takes about 1.04 ms. The reservation walker decodes only 28
ranges from the full phone DTB, making another full uncached traversal costly.
v5 removes that traversal; its first measured reservation interval is 0.469 ms.

Private captures, comparisons, and results are in
`out/anne-device-private/boot-speed-v1`, `boot-speed-v2`, `boot-speed-v3`, and
`boot-speed-v4`; averages and per-boot samples are in their `performance.json` files.
The v5 package preparation, first phone capture, and `performance.json` are in
`out/anne-device-private/boot-speed-v5`;
range comparisons, stack reports, and injected-DTB QEMU captures are in
`out/reservations-opt`.
Scheduler snapshots, host benchmarks, and QEMU results are in `out/sched-opt`;
the unflashed v6 package state is in `out/anne-device-private/boot-speed-v6`.
The boot-fix Debug capture is in `out/anne-device-private/dt-cache-fix-v1`;
the original baseline
is `out/anne-device-private/boot-speed-baseline.txt`.
