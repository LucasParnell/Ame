# Phase G — guest-visible Dango channel

The Linux appliance and Mochi now exchange ordered messages through two shared
SPSC rings. UART carries test results only. There is no GPU logic in the channel
driver, and no device DMA mapping is created for its pages.

## Mapping and authority

`SYS_VM_CHANNEL_MAP(channel, vcpu, ipa, size, irq)` binds a channel before first
guest entry. It requires send/receive/notify/inspect authority and vCPU control,
checks common VM ownership, alignment, address limits, stage-2 capacity and
overlap, and rejects SGI/PPI vectors or vectors already assigned to a device or
another channel. Failed mapping rolls back its pages. Teardown destroys stage 2
before releasing channel backing storage and revokes exported endpoints.

The GPU profile reserves `0x48000000..0x48020000`, outside its 128 MiB RAM:

| Address | Contents | Guest CPU access |
|---|---|---|
| `0x48000000` | 80-byte layout in a zeroed descriptor page | read-only, XN |
| `0x48001000` | host → guest ring, 64 slots | normal WB, RW, XN |
| `0x48003000` | guest → host ring, 64 slots | normal WB, RW, XN |

Each ring occupies two pages. Remaining reserved addresses are unmapped. Channel
pages are not appliance `VmMemory` objects and cannot enter `collect_vm_ram()`'s
DMA authorization list: their DMA class is **None**. Shared staging remains Phase I.

## Notifications

Host publication uses `SYS_VM_CHANNEL_NOTIFY` and virtual edge IRQ **48**. Guest
publication uses HVC **`0xc6000001`** (`x1=channel_id`, `x2=RING_PUBLISH`,
`x3=published head`). HVC **`0xc6000002`** arms reception (`x1=channel_id`,
`x2=observed H2G head`). Anko compares that head after arming and injects an IRQ
if publication raced the guest. Notifications coalesce until the next ARM.
PSCI remains separate; HVC return PCs are not advanced twice.

`CallGuard` serializes ARM, host notification, guest entry and VM retirement.
No callback stores an unprotected channel/vCPU pointer. HVC selects only a live
channel attached to the calling vCPU; it cannot map memory or address another VM.
Guest notifications wake the existing G2H doorbell, including a sleeping Mochi
consumer. Ring payloads stay out of the kernel; indexing uses bounded masks and
publication uses release/acquire operations.

## Linux interface and build

`linux-artifacts/guest-driver/dango_channel.c` is built into Linux; modules remain
disabled. `/dev/dango-channel0` is root-only and admits one open endpoint. It
supports an 80-byte version/layout query, notify ioctl, exact-length shared
`mmap` of both rings, blocking `read` and `poll`. Read returns an eight-byte IRQ
count; userspace consumes the ring. Offsets, private/executable mappings and
unknown ioctls are rejected; future executable protection is disabled as well.

The fixed DT device has no userspace unbind path. Dynamic guest devices and
multiple guest channel instances are outside this bootstrap profile.

`scripts/linux/stage-gpu-source.py` constructs a shallow source overlay to add
the built-in driver without editing the supplied Linux source. The macOS Docker
builder continues mounting that source read-only and keeps objects in Ame's
build directory. **Rebuild the GPU kernel once for Phase G**; subsequent changes
to the userspace probe can use the native macOS iteration commands in `HANDOFF.md`.

## Verification

`AME_VERIFY_CHANNEL=1 scripts/verify.sh` adds the `dango-channel` row. It requires:

- the real Linux `virtio_gpu` probe and enforced SMMUv3;
- 256 native endpoint round trips before guest entry;
- 512 Linux ↔ Mochi ordered round trips, crossing ring wraparound repeatedly;
- blocking read and poll, a nonzero Linux virtual IRQ count, and exact host
  notification counts (512 H2G, 514 G2H, including READY/DONE);
- clean IOMMU counters sampled **after** channel completion.

The sanitizer probe exercises both ARM/publication orderings, coalescing,
counter wraparound, invalid IDs/reasons, dead channels, wrong vCPU and PSCI
separation. Kernel lifecycle tests exercise permission/geometry/overlap failures,
foreign-VM rejection and reclamation after explicit destruction and owner death.

Reference appliance boots use two host CPUs. The existing VM_FAIR bootstrap
runner can starve behind native polling services with only one host CPU; that
scheduler limitation is separate from the channel's single guest-vCPU interface.

Validated on macOS arm64 with Homebrew Clang 23.1.1, QEMU 11.1.1 and Linux 6.12:

- Default gate: build, host regressions, lifecycle on 1/2/4 CPUs, supervision and durability passed.
- IOMMU, assignment and GPU opt-in rows passed.
- KMS passed after allowing QEMU's local monitor socket (the sandbox initially denied `bind`).
- Channel on two host CPUs: 512/514 notifications, `faults=0 quarantines=0 smmu=1`.
- Channel on four host CPUs: the same assertions passed.
- Single-host-CPU experiment timed out before Linux executed, as described above.

Captured evidence: `out/validation/channel-final/channel-result.json`,
`out/validation/channel-4cpu/channel-result.json`, and
`out/validation/channel-kms/kms-result.json` / `kms.ppm`.

Next: **Phase H**, the `dango-gpud` control plane over this transport.
