# Phase E — Linux `virtio-gpu-pci` probe (E.2–E.5)

Completes the appliance bring-up begun in [Phase E.1](gpu-appliance-phase-e1.md): Dango now
constructs the PCI assignment, the GPU artifact set is packaged, and the Linux guest binds
`virtio_gpu` and scans out behind the Ame SMMUv3 — with the INTx route delivered to the vGIC.

## Construction (E.2/E.3)

`userland/dango/manager/main.cc` performs, for the GPU profile and before the first `vcpu_run`:

1. `sys_vm_assign_device` — claim the assigned `virtio-gpu-pci` function (isolation group 3);
2. `sys_vm_assign_map_mmio` with `ASSIGN_FLAG_ASSIGN_BARS` — allocate the function's host BARs from
   the PCIe MMIO window (`0x10000000`, size `0x2eff0000`) and stage-2 **Device**-map the virtio
   modern BAR (`bar4 → ipa 0x50000000`); the MSI-X-bearing BAR is never mapped;
3. `sys_vm_assign_route_irq` — install the INTx level route;
4. `sys_vm_assign_expose_ecam` — publish the synthetic one-bus ECAM at `0x3f000000`.

`kernel/init/boot_caps.cc` grants `gpu.function` / `gpu.irq`; `userland/init/main.cc` passes them to
`dango.manager`; `userland/CMakeLists.txt` selects the GPU artifact set (`build/dango-linux-gpu`).

## INTx delivery and guest probe (E.4/E.5)

The host GIC specifier is `<type number flags>`; for `arm,gic` the SPI INTID is `32 + number`, so the
appliance slot is `32 + 3 + (device % 4)` = 38. The route installs and the guest's Linux `virtio_pci`
ISR fires; the guest appliance userspace (`linux-artifacts/agent/dango-agent.c`) mounts devtmpfs,
opens `/dev/dri/card0`, verifies `DRIVER_NAME == "virtio_gpu"` and the dumb-buffer capability, and
observes the virtio ISR in `/proc/interrupts`.

## Evidence

`AME_VERIFY_GPU=1 scripts/verify.sh` (row `gpu-appliance`, 10 checks) on macOS arm64,
Clang 23.1.1 / CMake 4.4.0 / QEMU 11.1.1:

```
kernel/init: gpu: function bus=0 dev=3 fn=0 vendor=1af4 device=1050 group=3 intx=38
kernel/vm:   assign: bars window=0x10000000 size=0x2eff0000 bar1=… bar4=…
kernel/vm:   assign: bar4 mapped ipa=0x50000000 host=… size=0x4000
kernel/vm:   assign: exposed fns=1 msi=1; ACTIVE device=1
dango.manager: gpu appliance ready / channel ready / service ready
Linux version 6.12.0 … Machine model: midix,dango-linux-gpu
dango: DRM card0 driver=virtio_gpu dumb_buffer=1
dango: Linux virtio ISR observed
dango: gpu probe passed
kernel/vm: gpu proof: faults=0 quarantines=0 routes=1 injected=1 smmu=1
```

Guards added for bugs found: BAR-size power-of-two assert, BAR-IPA alignment `static_assert`, and a
stage-2 headroom check after RAM mapping (`kMaxStage2Mappings` → 40960). The BAR allocator skips
regions the inventory already reports, so it no longer aliases the native xHCI BAR.

## Note on log interleaving

The guest UART shares the Ame host console serial file, so Ame log entries can split guest lines. The
harness de-interleaves guest output (`scripts/test/logfmt.py`) before matching markers; see
[Phase F](gpu-appliance-phase-f.md), which also uses this.

## Remaining

Phase F (standalone KMS self-test + host `screendump`) is done — see
[gpu-appliance-phase-f.md](gpu-appliance-phase-f.md). Phases G–S remain, per
`docs/GPU_APPLIANCE_TODO.md`.
