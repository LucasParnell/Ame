# Guest memory layout for the lavapipe guest (blocker #1)

## Current state (2026-09-22)

The latest committed work (`0da9833`, following `8b195c9` and `3c437c2`) maps
aligned RAM and assigned MMIO using stage-2 2 MiB blocks. The record table has
2,112 entries: 64 for the current 128 MiB RAM plus 2,048 for device/shared
mappings and partial splits. At 32 bytes per record this is 66 KiB per VM,
rather than the previous 1.25 MiB. `mapped_pages` measures coverage;
`mapped_records` measures table consumption.

The continuation fixes the failure paths exposed by that change:

- A partial block split checks space for the **511 extra records** before
  mutation and reuses the original record. Allocation/capacity failure leaves
  the block, counters and tables unchanged. Mapping over a block returns
  `E_BUSY` without splitting it.
- Complete RAM/MMIO ranges are removed without splitting or allocating. This
  is used for mapping rollback, memory unmap and live device revocation. A
  range cutting a block is rejected before mutation; single-page unmap is the
  explicit partial-split operation. Device revocation rejects ordinary RAM.
- Capacity checks share `stage2_has_capacity`; they count records and check
  before subtracting. The old shared-arena/assignment checks still used page
  coverage, which could either reject valid attachments or wrap into false
  capacity after block mapping.
- Eligible block-map errors propagate and unwind. Page mappings are selected
  for incompatible geometry, not as retries that conceal a failed block map.

`tests/stage2_probe.cc` independently decodes hardware descriptors, maps
**512 MiB with 256 records**, exercises exact-capacity splits and allocation
failures, and revokes block-mapped Device memory with a full record table and
allocations disabled. The real shared-arena test covers attachment after block
mapping. These are capacity/lifetime proofs, **not a 512 MiB appliance boot**.

## Still required for the libc guest

The measured rootfs is about 186 MiB unpacked / 58 MiB compressed. Its standalone
lavapipe acceptance uses 512 MiB RAM; the appliance still has 128 MiB RAM and a
16 MiB initramfs load window.

1. Introduce a shared layout selection for the kernel, manager, manifest and
   guest DTS. `DANGO_LINUX_GUEST=minimal|lavapipe` currently selects artifacts;
   it does not expand kernel memory bounds. Update the RAM range, per-object
   allocation limit and metadata budgets together, retaining the minimal default.
2. Move the fixed channel, staging and shared-region windows together with the
   RAM boundary. Their shared ABI headers are authoritative; regenerate the DTS
   and retain overlap/alignment checks. Also reconcile assigned BAR placement.
3. Deliver the libc rootfs through a guest-visible block device (or explicitly
   budget a larger initramfs). The native OS's virtio-blk `/data` path is not a
   block device automatically available to this assigned Linux guest. Guest
   authority, driver configuration and root mounting need an actual implementation.
4. Measure the smallest viable RAM size using the real executor and then boot it
   inside Dango. Keep standalone acceptance distinct from appliance acceptance.

After those dependencies: connect per-session arenas and the native Vulkan broker
to the guest executor, implement graphics/WSI, and run native offscreen/triangle
acceptance. Physical/amdgpu work remains out of scope.
