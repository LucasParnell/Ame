# Managed memory and arena migration

Implementation checklist:

- [x] Stable arena capabilities with replaceable reference-counted backing.
- [x] Separate physical allocation, VM mapping/COW/TLB operations, and ADMA copies.
- [x] Transactional grow, shrink, reserve, trim, overlapping realloc, reorder and COW clone.
- [x] Precopy outside the manager lock, write reconciliation, freeze and generation commit.
- [x] Reverse mappings, owner generations, software pins and fenced external leases.
- [x] Prepared mappings, Arm break-before-make, SMP rendezvous, ASID invalidation and deferred retirement.
- [x] Idle compaction, access/write temperature, bounded copy batches and pinned exclusions.
- [x] Manifest-defined primary arenas established before each service's first instruction.
- [x] Libmochi malloc/calloc/realloc/free and ordinary/aligned C++ allocation.
- [x] Typed kernel pools, separate allocator metadata, delayed reuse and stale-write checks.
- [x] Libmochi typed isoheaps and kernel-enforced process-lifetime virtual type cages.
- [x] Kernel image relocation into shared TTBR1, with physical entry retained for CPU startup.
- [x] Relocatable native images, per-process code/data, heap and stack randomization.
- [x] ChaCha20 generator seeded from trusted boot entropy or the Arm TRNG firmware ABI.
- [x] Temporary uptime fallback with explicit weak-entropy status and a build-time off switch.
- [ ] Verify the phone's actual HRNG interface and build a matching, audited BL31 patch if needed.

## Process contract and normal allocation

Every principal in `userland/system.json` declares an `arena` with `base`,
`initial`, `reserve`, `limit`, `type`, and `compact`. These declarations are
embedded in MXI executable headers and covered by executable checksums and signed
bundle hashes. Packaging rejects an executable whose arena differs from its
manifest, including profile overrides. Legacy hand-built images receive the
loader's bounded default contract.

The kernel creates and maps the private primary arena before starting a process.
`mx_process_arena` returns its capability and effective contract; with ASLR the
actual base differs from the manifest's preferred base. No physical address is
exported. Applications continue using normal allocation functions from libmochi;
only arena growth calls the kernel. Bin selection, splitting, coalescing, free,
and ordinary reuse remain local. A process mutex serializes its heap.

Defaults are 64 KiB initially committed/reserved and a 1 MiB maximum. The current
AMM bounds are 256 arenas globally, 256 backing pages per arena and 64 aliases per
arena. Resource exhaustion returns an error; it cannot escape these bounds.
Primary arena idle migration is disabled by default to keep the normal path
predictable. Explicit managed arenas can opt into `MX_ARENA_COMPACT_IDLE`.

`mochi::memory::IsolatedHeap<T>` provides a dedicated compile-time type ID and
kernel-enforced cage. Initialize it with a distinct cage, then use `create` and
`destroy`. Its address/type association survives decommit and arena destruction
until process retirement. Different processes have independent cage registries.
Ordinary C++ `new` uses the primary heap; type inference for arbitrary `new T`
is not claimed. Code requiring type isolation uses the typed API.

## Resize, realloc and migration

`mochi/managed_arena.h` exposes create, reserve, resize, trim, realloc, reorder,
clone, precopy, commit, cancel, inspect, pin, unpin, policy and bulk unmap.
The legacy arena-create syscall also produces managed backing; flags cannot
request kernel ownership, physical export or an unmanaged allocation.
Existing kernel-issued MMIO/device arenas retain their explicit wired semantics.

Resize within reserved capacity allocates no backing and copies no payload.
Growth publishes previously invalid leaf mappings without a CPU rendezvous.
Automatic capacity expansion prepares all additional pages and mappings before
publication. Shrink retains capacity and contents; trim releases backing.
Realloc at another VA and reorder change mappings while preserving physical
pages. Ordinary heap realloc can extend the tail block in place; relocation of
an ordinary allocation when adjacent space is unavailable still requires copying.
“Costless” means no backing allocation or payload copy when reserved capacity
permits; page-table work, TLB maintenance and synchronization still have costs.

The migration state machine is `STABLE → PRECOPY → DIRTY → FROZEN → COMMITTING`,
with the old generation retired before returning to `STABLE`. A precopy round
holds immutable source references independently of the active arena. Writers
COW those sources while ADMA runs outside the manager lock. Dirty pages are
reconciled while CPUs are frozen. Every alias uses prebuilt leaf slots; descriptor
replacement performs Arm break-before-make and broadcast ASID invalidation.
Published old pages cross an SMP grace period before reuse. Page zeroing and
physical release occur after CPUs resume, with a bounded idle reclamation budget.

Rendezvous attempts are bounded and can return `BUSY`; callers yield and retry.
Pins intentionally keep resize and migration busy until released. Mapping changes
that remove pages wait for remote kernel readers. COW modifies one page in each
alias, and transient EL1 user-buffer faults retry after the AMM transaction.
`SYS_UNMAP_PAGE` validates its entire page against the user VA interval before
walking either managed or wired mappings. The wired path additionally checks
the leaf is EL0-accessible; a privileged alias inside that numeric interval
cannot be removed.

## Devices and background work

ADMA supplies CPU copies and an asynchronous backend interface with submit,
poll and cancellation fences. Failed completion never releases buffers until
cancellation is proven. No memory-to-memory hardware DMA driver is installed;
SDHCI ADMA2 is a storage transfer engine. The tested asynchronous backend is a
host regression fixture, including writes during a pending copy.

Guest, GPU and display attachments use external generation leases. Shared backing
must be private and physically contiguous for these paths. A lease prevents
movement until stage-2 invalidation/device completion proves that readers have
stopped. A failed fence quarantines the backing. Cache cleaning/invalidation
supports noncoherent devices; software pins alone do not authorize DMA.

Idle work samples access-flag faults and first writes, ages page temperature,
skips pinned/hot arenas and compacts fragmented cold backing. Each copy pass
handles at most four pages. Oldest-first selection lets other arenas progress
while a backend is busy. Temperature is a policy estimate rather than a complete
hardware access-frequency counter.

## Apple-inspired hardening

The design adapts [Apple's kalloc_type isolation](https://security.apple.com/blog/towards-the-next-generation-of-xnu-memory-safety/)
and [WebKit libpas isoheaps](https://docs.webkit.org/Deep%20Dive/Libpas/Libpas.html)
to Ame's capabilities and bounded arenas. Kernel allocation types have dedicated
page runs whose ownership never changes type. Slot state and free links live on
separate metadata pages, with an O(1) data-page owner directory. Byte buffers and
typed control arrays use separate pools. Freed slots are cleared and pass through
a bounded per-type FIFO quarantine; reuse verifies clearing and redzones.
Under memory pressure quarantine can release the oldest slot to the same type.

Libmochi checks block signatures, extent guards, free-list bounds and reciprocal
links before writes. Freed blocks are cleared and quarantined; stale writes are
checked before reuse. These software checks detect common corruption and delayed
reuse reduces immediate use-after-free overlap. They are not hardware memory
tagging or protection against arbitrary writes within the same privilege domain.
The Cortex-A53 does not provide Apple's newer hardware tag enforcement.

## Address-space layout and entropy

The kernel image is linked in the high half and entered initially at its physical
Linux Image address. A bounded relocation table rebases image pointers and GOT
entries before constructors. The bootstrap installs shared supervisor-only TTBR1
mappings, switches static stacks and vectors, and uses the same high-half image
on secondary CPUs. Permanent TTBR0 roots mark the physical kernel image NX.
Privileged low physical aliases for legacy MMIO/physical-buffer code remain;
this is not a claim that all kernel direct-map accesses have moved to TTBR1.

Native MXI files contain validated absolute-pointer relocation records generated
from linked ELF relocation sections. Page-aligned slides preserve relative
instructions; unsupported relocation types fail conversion. Code/data, primary
heap and stack locations are chosen independently per process. Native code/data
slides remain in the ABI window below fixed IPC/device mappings at `0x18000000`;
ASLR cannot choose an existing protocol window. User VA windows
remain bounded; this is finite layout entropy rather than unbounded address space.

The generator uses Monocypher ChaCha20 with output/state separation and rekeying.
QEMU uses its firmware-provided `rng-seed`, which is erased after consumption.
Phone builds first request the Arm TRNG firmware interface, with separate error/status
handling, bounded retries and basic repeated-word rejection. The temporary `ANKO_ALLOW_WEAK_ENTROPY=ON` option uses hardware-counter uptime
when the trusted source is unavailable, as requested for bring-up. It is enabled
by default for now and reports `KASLR/ASLR active with weak entropy`; predictable
uptime does not establish secure randomization. Set this option to `OFF` to
require trusted entropy: production builds then fail closed and development
builds report randomization inactive. Device identifiers and embedded signing/
FIDO seeds are never used as entropy.
The phone's stock monitor/TRNG compatibility and any required hardware-specific
EL3 patch remain unverified without the matching firmware artifact.

## Validation

Host tests exercise production AMM/VM/ADMA under ASan/UBSan, allocation and
rendezvous rollback, asynchronous copy/fence failures, page retirement, cages,
external leases, kernel allocator corruption, and 40,000 randomized libmochi
allocation/realloc/alignment operations. The local allocation loop verifies no
backing-growth calls. QEMU lifecycle tests exercise EL0 COW, access faults,
concurrent writers, guest pins, normal allocation, typed isolation and privileged
unmap rejection. Both EL1 single-CPU and EL2 SMP configurations are used; Huawei
cross-compilation alone does not establish hardware behavior.

The pinned Huawei Hi6250 OSS audit uses revision
`65e545cbaa11d132e243287b150146bc2a51c45e`. Its
[defconfig disables HW_RANDOM](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/arch/arm64/configs/merge_hi6250_defconfig#L344).
The [TrustZone client](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/hisi/tzdriver/tc_client_driver.c#L1249)
uses `get_random_bytes_arch`, whose
[fallback consumes the Linux software pool](https://github.com/hisi-oss/android_kernel_huawei_hi6250-8/blob/65e545cbaa11d132e243287b150146bc2a51c45e/drivers/char/random.c#L1339).
There is no ARM64 architecture RNG hook in that release. Published SECENG base
addresses alone do not establish an HRNG register layout or a callable BL31
service. No unverified MMIO reads or speculative monitor patches are installed.

`scripts/test/security-layout.py` runs three fresh QEMU boots, checks actual
high-half instruction fetches and confirms independent user code/stack/arena
variation. It discards its instruction traces after validation. The lifecycle
fixture also enters a real EL2 guest and checks HVC return and complete VM
retirement; guest shutdown remains paused until the fenced teardown finishes.

The layout test accepts `--weak-fallback` to disable QEMU firmware randomness
and verify that counter-based bring-up boots identify their entropy as weak.
