# ARM SMMUv3 backend and hostile-DMA verification (Phase C)

Spec ref: `AME-GPU-DRIVER-APPLIANCE` §8.1 (isolation rule), §8.2 (IOVA model),
§8.7 (SMMUv3 backend), §8.8 (publication), §8.10 (faults), §8.12 (revoke),
§8.14 (verification).

## Layout

| File | Role |
|---|---|
| `kernel/include/anko/smmuv3_enc.h` | Pure register/record encoders (STE, commands, events, queue maths). Host-testable, no MMIO. |
| `kernel/include/anko/smmuv3.h` | Public backend API (bring-up, stream programming, fault drain, native arenas, selftest). |
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

### Stream table

* Linear (single-level), `STRTAB_BASE_CFG.FMT = 0`, `LOG2SIZE = 8` → 256 entries
  × 64 B = 16 KiB, 16 KiB-aligned, a static array in `.bss`.
* Every entry is initialised to zero. An all-zero STE has `Valid = 0`, so the
  stream faults (`C_BAD_STE`) — default deny. There is no bypass STE, no
  identity map, and no "temporarily trust the device" mode.
* A stream id `>= sid_limit` (`min(8, IDR1.SIDSIZE)`) is rejected by
  `smmuv3_program_stream()` with `E_INVALID`, so an out-of-range requester can
  never be silently attached.
* Attach writes the STE, cleans it to the point of coherency, issues
  `CFGI_STE(sid)` (a cached stream configuration must not keep translating with
  the old context) and `CMD_SYNC`. Detach programmes a *faulting* STE: stage-2
  enabled with an all-zero root table, so every lookup misses and is recorded as
  an `F_TRANSLATION` fault. That is "switch to the faulting domain" from §8.12,
  not a bypass.

### STE (stage-2)

```
Config = 0b110 (S2 translate), S2TTB = domain root table, S2VMID = domain index,
S2T0SZ = 25 (39-bit IOVA), S2SL0 = 1 (walk starts at level 1), S2TG = 0 (4 KiB),
S2PS = 4 (44-bit PA), S2AA64 = 1, S2R = 1 (record faults)
```

Per-domain VMIDs make invalidation scoped: `TLBI_S12_VMALL(vmid)` for a domain
invalidation and `TLBI_S2_IPA(vmid, iova, pages)` for a range. Global
invalidation is never issued (`IOMMU_TLBI_GLOBAL` stays zero).

### Queues

* Command queue: 256 × 16 B = 4 KiB static, `log2size = 8`. Submission writes
  the record, cleans it, and advances `CMDQ_PROD`; completion is polled through
  `CMDQ_CONS` after a `CMD_SYNC` (bounded spin). A `CMDQ_CONS.ERR` is surfaced as
  `E_FAULT` so the core quarantines rather than continuing on a broken command
  stream.
* Event queue: 256 × 32 B = 8 KiB static. The SMMU is the producer; the kernel
  pops each record at `EVENTQ_CONS`, advances and publishes the consumer index.
  `EVT_Q_OVERFLOW` is detected and logged; drains are bounded per call.

### Backend hooks

* `publish_page_table_range` — clean the range to the point of coherency.
* `invalidate_iotlb_range` — `TLBI_S12_VMALL` for a domain (`length == ~0`) or
  `TLBI_S2_IPA` for a range; a range longer than the 5-bit `NUM × SCALE`
  encoding is rounded up to the next representable granularity (never
  under-covers).
* `sync_command_stream` — `CMD_SYNC` + wait.
* `attach_stream` / `detach_stream` — programme the STE (added to the core's
  vtable; a null hook stays a no-op, so the host regression and platforms
  without a programmable IOMMU are unaffected).

## Fault handling

`kernel/iommu/faults.cc` decodes each record (requester id, IOVA, R/W, S2/class,
reason) with rate-limited logging, records it through the core's `note_fault`
(increments `IOMMU_FAULTS`), and counts per-stream faults. An unmapped fault is
never answered by mapping a page.

A stream that reaches the fault threshold moves `ACTIVE → FAULTED →
BUS_MASTER_DISABLED`: its STE is reprogrammed to the faulting context, the PCI
command register's **Bus Master Enable** is cleared, `IOMMU_QUARANTINES`
advances, and the owning domain is put into the quarantined state (no new
mappings). The event queue is drained from the timer tick (throttled, bounded),
so faults are handled without a routed SMMU interrupt.

## Early bring-up and bus-master containment

`iommu::smmuv3_bring_up()` runs early in `kernel_main()` — after the MMU is up
and before the GIC, timer, scheduler and any userland service. It sets
`GBPA.ABORT` first (fail closed: if anything below fails, or while `SMMUEN` is
not yet effective, every stream terminates instead of bypassing), builds the
empty stream table, programmes the queues, clears `IRQ_CTRL`/`CR2`, and enables
`CR0 = SMMUEN | CMDQEN | EVENTQEN`, confirming `CR0ACK`. With no SMMU present it
is a no-op and the core stays on the null backend.

`pci::contain_assignable()` holds every assignable (non-native, non-bridge) PCI
function non-bus-mastering until an assignment is constructed. The native xHCI
function is left untouched so the native USB path keeps working; it gets a
translation context through `register_native_dma_arena()` (IOVA == PA over its
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

Observed evidence (QEMU 11.1.1, `out/qemu-dev`):

```
iommu/smmuv3: idr0=0xd44101b idr1=0x2730010 idr5=0x74 sid_bits=16 cmdq_bits=19 evtq_bits=19 oas=4
iommu/smmuv3: enabled base=0x9050000 sid_limit=256 cr0ack=0xd
iommu/selftest: ste sid=32 valid_config=0xd s2cfg=0x40c0059 s2ttb=0x41426000/0x0 root=0x41426000
iommu/selftest: (a) mapped DMA ok result=0x0
iommu/selftest: (f) unmap + scoped TLBI ok result=0xdead0002 tlbi_range=1
iommu/selftest: (b) unmapped fault ok result=0xdead0002 faults=2 canary=0xa5a5a5a5
iommu/selftest: (c) read-only rejection ok result=0xdead0002
iommu/selftest: (d) detached fault ok result=0xdead0002
iommu/faults: quarantine: sid=32 faults=8 generation=2 (STE faulting, bus-master cleared)
iommu/selftest: (e) repeated faults ok injections=16 stream_faults=20 state=3 quarantines=2 gerror=0x0
iommu/selftest: counters IOMMU_FAULTS=20 IOMMU_QUARANTINES=2 IOMMU_TLBI_RANGE=1 events=20
iommu/selftest: iommu dma test passed (22 checks)
iommu/native: arena registered seg=0 sid=16 base=0x4186d000 size=0x10000   # native xHCI
```

Host-side encoder regression: `tests/smmuv3_probe.cc` (run by
`tests/test_smmuv3.py` via `ctest`) checks the STE, command and event encodings,
the TLBI coalescing and the queue index arithmetic. `tests/iommu_probe.cc` was
extended to assert the level-3 page descriptor type (`[1:0] = 0b11`).

## Known gaps

* The SMMU event-queue interrupt (SPI 74) is not routed to a handler; the kernel
  polls the event queue from the timer tick instead. Routing it is Phase D
  (interrupt virtualization) work.
* Only one SMMU instance / PCI segment 0 is supported; a non-zero segment is
  rejected rather than mis-programmed.
* Stage-1 translation is not implemented: a device whose SMMU advertises no
  stage-2 support cannot be isolated by this backend and bring-up fails closed
  with the SMMU in global-abort (no bypass).
* §8.14 item 4 ("a page subsequently reused by another process") is covered to
  the extent that the released page provably stays untouched after unmap; real
  page recycling needs Phase D/E assignment plumbing.
