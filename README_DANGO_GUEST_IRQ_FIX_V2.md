# Dango guest interrupt-routing fix v2

Extract this archive at the root of the Ame checkout.  It intentionally contains
complete replacement files, not patch hunks.

Files replaced:

- `kernel/arch/arm64/el2.S`
- `kernel/include/anko/vm.h`
- `kernel/vm/vcpu.cc`

Fixes included:

1. Split lower-EL EL2 vectors so guest IRQ/FIQ/SError exits do **not** read
   `ESR_EL2`.  `ESR_EL2` is not valid for asynchronous exceptions; using a stale
   MMIO data-abort ESR causes the host to re-emulate the previous MMIO access and
   advance the interrupted guest PC.  This was the real HCR interrupt-routing
   corruption behind the early Linux stall.
2. Keep `HCR_EL2.IMO/FMO/AMO` for isolation, but inject deliverable guest IRQs
   with `HCR_EL2.VI` from host VGIC state.
3. Add a small software GICv2 model for the Linux guest: distributor enable,
   pending, active, priority, targets, CPU interface `IAR/EOIR/DIR/HPPIR`, and
   timer PPI lines.
4. Preserve the full guest GPR state across exits, including `x18/x19`.  The
   first archive accidentally dropped those two restores while refactoring.
5. Avoid leaking the hypervisor's temporary timer IMASK bit back into the guest
   shadow timer registers.

Build check used:

```sh
cmake -S . -B build-check -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake
ninja -C build-check kernel/anko
```
