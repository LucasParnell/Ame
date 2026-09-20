# Phase E.1 — synthetic ECAM for the INTx bootstrap profile

The kernel now emulates an assigned PCI function's configuration through actual guest stage-2
translation faults. This completes the ECAM transport milestone; it does **not** prove a Linux
`virtio_gpu` probe, DRM device, interrupt handler or scanout.

## Construction contract

1. Create the VM and appliance RAM; attach the function's complete isolation group through the
   existing assignment API and its SMMUv3 domain.
2. Map owned BAR apertures with `VM_ASSIGN_MAP_MMIO`, before first guest entry. Only complete,
   naturally aligned BAR mappings become visible in config. Partial mappings retain the earlier
   low-level mapping contract but are not advertised as usable PCI BARs. Relocation is unsupported.
3. Install the INTx route. Publish the synthetic bus with syscall 60,
   `VM_ASSIGN_EXPOSE_ECAM(assignment, ipa, 1 MiB)`, requiring `RIGHT_VM_CONTROL`.
   The aperture is unmapped and reserved against later RAM/MMIO insertion. Publication seals BAR
   construction. Functions occupy bus 0, device 1, functions 0–7; sparse host function groups are
   rejected. The manager still needs to implement this sequence.
4. Enter the guest. Valid aligned scalar AArch64 accesses of 1/2/4 bytes trap and complete through
   the assignment; invalid syndromes and unsupported accesses take the existing fault exit.

The GPU DTS uses ECAM `0x3f000000..0x3f100000` and MMIO `0x50000000..0x60000000`, outside its
128 MiB RAM and channel region. ECAM is never a stage-2 mapping of host config space.

## Isolation and implementation

- Pure config operations live in `kernel/pci/ecam.cc`. Records, bounded arrays and free functions;
  no heap allocation or host ECAM access for config reads, BAR probes or capability filtering.
- Missing functions read all ones; extended config for a present function reads zero. Linux-style
  all-ones BAR writes return sizing masks; restoring the fixed guest address succeeds. Other
  relocation, ROM, capability-list and read-only header writes leave state unchanged.
- Only supported virtio MMIO capabilities referring to published BARs survive the bounded list
  filter. Lengths, cycles, overlapping retained records and aperture bounds are checked before
  atomic publication. MSI/MSI-X and indirect PCI_CFG capabilities are hidden in this INTx profile.
- Raw BAR pages containing an MSI-X table or pending-bit array are rejected, including when the
  MSI-X capability is hidden. Earlier assignment-model MSI tests do not establish safe guest
  MSI-X passthrough in this transport.
- Command writes combine subword data without overwriting status. BME requires an ACTIVE assignment
  and ACTIVE IOMMU domain. Host command updates use halfword writes, avoiding status W1C side effects.
- Config emulation, assignment changes and teardown are serialized by the existing VM `CallGuard`.
  Device-leaf revocation works after first guest entry, without relaxing RAM mapping rules. Guest
  entry invalidates the VMID's stage-2 translations before resume. ECAM reservations remain until
  VM teardown; a revoked assignment reads absent and cannot re-enable bus mastering.

## Evidence

Validated on macOS arm64, Homebrew Clang 23.1.1 / CMake 4.4.0 / QEMU 11.1.1:

- `AME_VERIFY_BUILD=out/qemu-dev AME_VERIFY_IOMMU=1 AME_VERIFY_ASSIGN=1 scripts/verify.sh`:
  configure/build, host regressions, provenance, direct boot, 1/2/4-CPU lifecycle, supervision,
  durable persistence, SMMUv3 hostile DMA, and assignment all passed on the final code.
  The host suite contains 16 tests. Gate log: `out/validation/ecam-verify.log`.
- The assignment proof contains 43 checks, including a real AArch64 guest that traps on
  config loads/stores, probes/restores a BAR, attempts forbidden relocation, reads absent devices
  and sign-extends an absent-function byte. A subsequent revoke removes an active VM's BAR leaf.
- Host ASan/UBSan probes exercise the full one-bus address decoder, invalid access widths/alignment,
  register boundaries, 32/64-bit BAR masks, malformed capability lists, MSI-X table/PBA exclusion,
  syndrome decoding, trap reservation overlap/capacity and active Device-only revocation.
- Native macOS `build-dtb.sh` generated the GPU DTB; `fdtget` confirmed ECAM and MMIO ranges.
- Boot harness regression proves stale serial logs cannot pass a failed boot in a reused directory.

## Remaining

Phase E.2/E.3: grant actual PCI/IRQ authority to Dango, construct the GPU assignment and routes,
select/package the GPU artifacts, then boot Linux. Phase E.4/E.5 must prove actual INTx→vGIC delivery,
`virtio_gpu` binding and `/dev/dri/card0` without unexpected IOMMU faults. F–J remain as listed in
`docs/GPU_APPLIANCE_TODO.md`. No physical GPU or AMD-specific work is included here.
