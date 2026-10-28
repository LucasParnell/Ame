# IOMMU ownership, routing and ARM SMMUv3 verification

Companion to [Ame memory and DMA ownership](architecture.md#memory-and-dma-ownership).
This guide covers the generic/native/assigned DMA contract and SMMUv3 backend;
the [Anne GPU audit](../boards/huawei-anne/gpu-isolation.md) documents the separate
Mali shadow-MMU boundary.

## Layout

| File | Role |
|---|---|
| `kernel/include/anko/iommu_pt.h` | Pure descriptor format data for the bounded radix walker. |
| `kernel/include/anko/smmuv3_update.h` | Checked break-before-make STE replacement, with failure-injection probes. |
| `kernel/include/anko/smmuv3_enc.h` | Pure register/record encoders (STE, commands, events, queue maths). Host-testable, no MMIO. |
| `kernel/include/anko/smmuv3.h` | Public hardware API (bring-up, stream programming, fault drain, selftest); native registration lives in `iommu.h`. |
| `kernel/iommu/smmuv3.cc` | MMIO, stream table, command/event queues, backend hooks. |
| `kernel/iommu/faults.cc` | Event decode, rate-limited records, `ACTIVE → FAULTED → BUS_MASTER_DISABLED` quarantine. |
| `kernel/iommu/native.cc` | IOMMU context for native (non-assigned) devices, e.g. drv.usb.xhci. |
| `kernel/iommu/selftest.cc` | Opt-in `iommu-testdev` hostile-DMA verification. |
| `kernel/iommu/core.cc` | Generic core; also calls the `attach_stream`/`detach_stream` hooks. |

## Translation model

Each Ame IOMMU domain is a set of page tables built by the generic core. The
SMMU is programmed for **stage-2 only** translation: the device IOVA is the
stage-2 input address (IPA) and the STE's `S2TTB` page tables translate it to an
Ame physical address. There is no stage-1 table and therefore no context
descriptor: the assignment identity lives entirely in the STE. This is the same
construct a hypervisor uses for an IOMMU domain, and it matches the descriptor
bit convention the core already emits (`AP[2:1]` = read/write permission).
It also matches §8.2: the IOVA is the address Linux uses, translated to whatever
Ame physical page owns it.

### Core ownership and backend routing

Platform code registers disjoint stream ranges through `register_backend` only
once translation is ready. `backend_for` checks the route's live readiness.
Hardware backends supply every publication/invalidation/completion/stream hook,
descriptor format and explicit IOVA/PA limits. The null backend is a software
model: it cannot authorize native DMA or a Dango assignment. A domain cannot be
attached through another backend or moved over an uncertain binding.

The current walker supports three levels, a 4 KiB granule and aligned 2 MiB
blocks. Descriptor bits belong to the backend; other walk geometries require
another walker. Mapping bounds include physical output width, preventing silent
address truncation. Page-owner queries, DMA classes and permissions establish
which pinned extents belong to each domain. Native devices have separate domains;
registration overlap across owners is denied. Unregister completes revocation
before the caller can free the physical arena, and gives fresh owner identities
on reuse. Unsupported routes return `E_NOTIMPL`, with negative probe coverage.

Mapping and unmapping are cold operations over fixed tables. The complete batch
is validated before descriptor mutation. Dirty table flags include existing leaf
and parent tables, not just newly allocated tables. Publication errors propagate.
An allocation rollback clears partial descriptors, unlinks new empty tables,
publishes the removals and completes scoped invalidation before reclaiming them.
A failed publication, invalidation or completion retains attempted records and
pins and stops new mappings. Owners keep pages pinned until successful teardown.

Unmap retires empty tables and invalidates their whole walk-cache coverage:
2 MiB for an L3 table, 1 GiB for L2. A retired parent's coverage absorbs its
children's coverage. Tables and payload pages are reclaimed after one completion
for the batch. Destroy first denies all attached streams, then publishes cleared
translations and completes a domain invalidation. Failed detach retains its
binding for retry. Quarantined metadata alone does not prove DMA denial.

This keeps allocation and table work out of USB transfer submission. The dense
mapping array and table-address, dirty and retired-coverage columns are bounded;
there is no per-transfer IOMMU allocation or cache maintenance. The current
registered pool bounds remain eight domains (one permanent deny slot), 2,048
mapping records and 1,024 table pages per domain, and 64 live/uncertain stream
bindings. Proven-denied binding slots can be reused during repeated hotplug.

### Stream table

* Linear (single-level), `STRTAB_BASE_CFG.FMT = 0`, `LOG2SIZE = 8` → 256 entries
  × 64 B = 16 KiB, 16 KiB-aligned, a static array in `.bss`.
* Every entry is initialised to zero. An all-zero STE has `Valid = 0`, so the
  stream faults (`C_BAD_STE`) — default deny. There is no bypass STE, no
  identity map, and no "temporarily trust the device" mode.
* A stream id `>= sid_limit` (`min(8, IDR1.SIDSIZE)`) is rejected by
  `smmuv3_program_stream()` with `E_INVALID`, so an out-of-range requester can
  never be silently attached.
* Replacement first invalidates the old STE, publishes it, issues
  `CFGI_STE(sid)` and invalidates the old VMID before completion. The new
  payload is published while invalid; the valid word is published last, followed
  by `CFGI_STE` and completion. Detach programmes a *faulting* STE: stage-2
  enabled with an all-zero root table, so every lookup misses and is recorded as
  an `F_TRANSLATION` fault. That is "switch to the faulting domain" from §8.12,
  not a bypass.

### STE (stage-2)

```
Config = 0b110 (S2 translate), S2TTB = domain root table, S2VMID = domain index,
S2T0SZ = 25 (39-bit IOVA), S2SL0 = 1 (walk starts at level 1), S2TG = 0 (4 KiB),
S2PS = min(IDR5.OAS, 48-bit encoding), S2AA64 = 1, S2R = 1 (record faults)
```

Per-domain VMIDs make invalidation scoped: `TLBI_S12_VMALL(vmid)` for a domain
invalidation and `TLBI_S2_IPA(vmid, iova, pages)` for a range. Global
invalidation is never issued (`IOMMU_TLBI_GLOBAL` stays zero).

### Queues

* Command queue: up to 256 × 16 B = 4 KiB static. Active depth is bounded by
  `IDR1.CMDQS`; event depth is likewise bounded by `EVENTQS`. A full queue
  waits for ownership with a bounded deadline and never overwrites pending commands. Submission writes
  the record, cleans it, and advances `CMDQ_PROD`; completion is polled through
  `CMDQ_CONS` after a `CMD_SYNC` (bounded spin). A `CMDQ_CONS.ERR` is surfaced as
  `E_FAULT` so the core quarantines rather than continuing on a broken command
  stream.
* Event queue: 256 × 32 B = 8 KiB static. The SMMU is the producer; the kernel
  pops each record at `EVENTQ_CONS`, advances and publishes the consumer index.
  Overflow is detected by the `EVENTQ_PROD/CONS` overflow-bit mismatch, not
  an event-record bit. Lost events or active global errors disable the controller
  under acknowledged `GBPA.ABORT`; failed routes stop authorizing DMA and pins
  remain retained. Drains are bounded per call.

### Backend hooks

* `publish_page_table_range` — order coherent writes, or clean the range to PoC
  and complete cache maintenance before queue publication. Cache-line size comes
  from `CTR_EL0`, avoiding a fixed 64-byte assumption.
* `invalidate_iotlb_range` — `TLBI_S12_VMALL` for a domain (`length == ~0`) or
  `TLBI_S2_IPA` for a range; a range longer than the 5-bit `NUM × SCALE`
  encoding is aligned and rounded to cover both its leading offset and tail.
  Without `IDR3.RIL`, small ranges use legacy single-page commands (reserved
  range fields remain zero) and large ranges use a VMID-scoped invalidation.
* `sync_command_stream` — `CMD_SYNC` + wait.
* `attach_stream` / `detach_stream` — replace the STE with ordered publication
  and checked completion. Optional hooks belong only to the explicit software model.

## Fault handling

`kernel/iommu/faults.cc` decodes each record (requester id, IOVA, R/W, S2/class,
reason) with rate-limited logging, records it through the core's `note_fault`
(increments `IOMMU_FAULTS`), and counts per-stream faults. An unmapped fault is
never answered by mapping a page.

All 256 supported stream IDs have indexed fault records; there is no smaller
tracking table that an untrusted device can exhaust. Repeated faults contain the
stream through the core, changing its binding only after denial is proven. PCI
bus-master shutdown is read back before being reported. Failed containment keeps
pins and bindings; logs distinguish it from success. New assignments reset fault
accounting so a previously quarantined stream remains containable after recovery.
Hardware/core operations run outside the statistics lock, preserving lock order.
Faults remain timer-polled pending interrupt routing.

`IDR0.COHACC` describes only the SMMU's walks, structures and queues. It does
not establish payload-DMA coherency for USB or another client. `CR1` and STE walk
attributes match that capability. Non-coherent structures are explicitly cleaned;
event storage is initially cleaned, then CPU read-only and invalidated before
consumption. Client payload arenas retain their independently established memory
attributes; xHCI continues using uniformly Normal Non-Cacheable CPU aliases.

## Early bring-up and bus-master containment

`iommu::smmuv3_bring_up()` runs early in `kernel_main()` — after the MMU is up
and before the GIC, timer, scheduler and any userland service. It sets
`GBPA.ABORT` first (fail closed: if anything below fails, or while `SMMUEN` is
not yet effective, every stream terminates instead of bypassing), builds the
empty stream table, programmes the queues, clears `IRQ_CTRL`/`CR2`, and enables
`CR0 = SMMUEN | CMDQEN | EVENTQEN`, confirming `CR0ACK`. Bring-up waits for GBPA and disable acknowledgements before replacing firmware
state, validates granule/format/queue geometry and registers the route only after
command completion. With no SMMU, no hardware route is registered.

`pci::contain_assignable()` holds every assignable (non-native, non-bridge) PCI
function non-bus-mastering until an assignment is constructed. Native xHCI discovery disables bus mastering too; activation follows
verified DMA registration before authority is issued. Development QEMU builds
can explicitly run without an IOMMU, while release activation requires a route.
It gets a translation context through `register_native_dma_arena()` (IOVA == PA over its
pinned DMA arena, a scoped set of kernel-owned pages — not an identity map).

## Driving QEMU `iommu-testdev`

Topology (`scripts/test/boot.py`, opt-in):

```
-machine virt,virtualization=on,gic-version=2,iommu=smmuv3,default-bus-bypass-iommu=off
-device iommu-testdev        # 1b36:0005, conventional PCI, BAR0 4 KiB MMIO
```

The device is driven straight from its BAR0 by the kernel. Because a direct
`-kernel` boot has no firmware, no BAR is assigned; the selftest assigns BAR0
into the QEMU virt 32-bit PCI MMIO aperture (`0x1010_0000`, clear of the xHCI's
`0x1000_0000`), enables memory decoding, and maps the 4 KiB window into the
kernel address space.

BAR0 register file (little-endian 32-bit accesses, from `hw/misc/iommu-testdev.c`):

| Offset | Name | Meaning |
|---|---|---|
| 0x00 | `DMA_TRIGGERING` | **read** runs the armed DMA |
| 0x04/0x08 | `DMA_GVA_LO/HI` | IOVA the device writes to (translated by the SMMU) |
| 0x0C | `DMA_LEN` | byte count |
| 0x10 | `DMA_RESULT` | `0` ok; `0xffffffff` idle, `0xfffffffe` busy; `0xdead0002` TX fail (translation/permission abort) |
| 0x14 | `DMA_DBELL` | bit 0 arms the request |
| 0x18 | `DMA_ATTRS` | secure/space attributes (0 = non-secure) |
| 0x1C/0x20 | `DMA_GPA_LO/HI` | physical address the device reads back from — **bypasses the SMMU** |

When triggered, the device writes the constant `0x12345678` to the IOVA through
its IOMMU address space and then reads the same bytes back from the GPA via
`address_space_memory`. Because the readback is physical, the only translation
under test is the device's write. The kernel only reads the result register and
the test pages, so the SMMU is the sole translation provider.

## Verification

```
# default gate — unchanged (no SMMU, no testdev, no new row)
scripts/verify.sh

# Phase C: SMMUv3 + iommu-testdev
AME_VERIFY_IOMMU=1 scripts/verify.sh
# or the same check standalone
python3 scripts/test/iommu_dma.py --build out/verify
```

`scripts/test/iommu_dma.py` boots the opt-in topology and asserts:

| # | Claim |
|---|---|
| (a) | DMA to a mapped appliance IOVA succeeds and lands on the backing page. |
| (b) | DMA to an unmapped IOVA faults; `IOMMU_FAULTS` advances; the canary page (deliberately set to the physical address an identity translation would have hit) is unchanged. |
| (c) | A read-only mapping rejects a device write. |
| (d) | A detached requester faults rather than bypassing translation. |
| (e) | Repeated faults (16 injections) neither overflow the event queue (`GERROR` clear) nor escape quarantine: the stream ends `state = 3` (bus-master disabled) and `IOMMU_QUARANTINES` advances. |
| (f) | An IOVA unmapped and synchronised can no longer be reached: the range-scoped `TLBI` (`IOMMU_TLBI_RANGE`) dropped the cached translation and the released page stayed untouched. |

Host regressions run with ASan/UBSan. `tests/iommu_ordering_probe.cc` uses a
cached device-walker model against the real core and injects allocation,
publication, attach and detach failures. It verifies existing-table publication,
whole-batch rejection, retained pins and roots, full retired-table coverage,
recovery and binding reuse. `tests/smmuv3_probe.cc` injects failure at every STE
replacement step and checks aligned range coverage, queue wrap/full arithmetic,
coherent attributes and output-width encodings. `tests/native_dma_probe.cc`
checks registration isolation, unrouted devices, failure retention and fresh
identities after unregister. The live QEMU test still proves hostile DMA through
actual SMMUv3 translation and the unchanged 22-check kernel selftest.

Architectural reference: [Arm IHI 0070 G.a](https://documentation-service.arm.com/static/66c5c097882fec713ef4a8ff),
particularly coherency considerations, structure update procedures and queue
register formats. Non-coherent physical hardware and Huawei USB stream routing
still require platform evidence; QEMU does not establish those paths.

## Known gaps

* The SMMU event-queue interrupt (SPI 74) is not routed to a handler; the kernel
  polls the event queue from the timer tick instead. Routing it is Phase D
  (interrupt virtualization) work.
* Only one SMMU instance / PCI segment 0 is supported; a non-zero segment is
  rejected rather than mis-programmed.
* Stage-1 translation is not implemented: a device whose SMMU advertises no
  stage-2 support cannot be isolated by this backend and bring-up fails closed
  with the SMMU in global-abort (no bypass).
* Non-coherent cache maintenance is implemented, but QEMU's coherent backend
  does not validate a physical non-coherent interconnect.
* Huawei USB routing, a board IOMMU backend, alternative walk geometries and
  four-level/x86 translation backends remain unimplemented. They must supply
  verified routes and completion mechanisms rather than using the null model.
