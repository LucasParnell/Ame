# Experimental Midgard PanVK

Two patches apply in order to Mesa 24.3.4, SHA-256
`e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025`:

1. `0001-panvk-midgard-bringup.patch` — the architecture-5 forward port.
2. `0002-panvk-v5-tagged-framebuffer-pointer.patch` — the fix that made the
   first hardware frame render.

Build with `scripts/linux/build-panvk-mesa.sh`.

This is a partial architecture-5 forward port using modern PanVK and the
maintained Midgard compiler. It is neither a revert of the 2022 removal nor a
complete/conformant Vulkan implementation. Hardware rendering on ANE **is now
verified** for one static triangle: `diag gpu vulkan` on the T830 reports
`status=0 frames=1 checksum=355e1a23 display_restored=1` with the client's
red/green/blue interior probes verified. The five-second animation, broader
descriptor and copy paths, and Vulkan CTS are still outstanding. Unsupported
paths and evidence are tracked in
[the status document](../../../docs/ANNE_VULKAN_MIDGARD.md).

`0002` exists separately because `0001` was already applied to the working tree
when the fault was found; applying both in order reproduces the built tree
exactly, which was checked byte-for-byte against a pristine 24.3.4 extract.

Mesa source files retain their upstream MIT notices. New Midgard glue uses
SPDX MIT identifiers. The build script's glibc C11-name compatibility adjustment
is separate from the driver patch and is included in source provenance.
