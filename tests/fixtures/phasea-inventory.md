# Phase A — PCI / IOMMU inventory evidence

Captured while adding the ECAM PCI inventory (`kernel/pci/enumerate.cc`), the
all-instances virtio-mmio + SMMUv3 DTB parsing (`kernel/platform/probe.cc`,
`kernel/include/anko/hw_info.h`), and the opt-in SMMUv3 QEMU topology in
`scripts/test/boot.py`.

## Files

| File | What it shows |
|------|---------------|
| `phasea-iommu-topology.dtb` | Compact DTB of the opt-in SMMUv3 topology (rebuilt from the QEMU `dumpdtb` dump). |
| `phasea-iommu-smmu-node.dts` | `smmuv3@9050000` and `pcie@10000000` nodes from that DTB. |
| `phasea-iommu-boot.txt` | Serial (host UART) excerpt proving the SMMU is discovered and the PCI functions are enumerated. |

## Reproduce

Default topology (unchanged; must stay green):

```sh
./scripts/verify.sh
```

Opt-in SMMUv3 topology (SMMU + `virtio-gpu-pci`, ramfb kept):

```sh
AME_QEMU_IOMMU=1 python3 scripts/test/boot.py --build out/verify --mode direct \
    --cpus 2 --marker "all services ready"
```

Rebuild the DTB evidence:

```sh
qemu-system-aarch64 -machine virt,virtualization=on,gic-version=2,iommu=smmuv3,default-bus-bypass-iommu=off \
    -cpu cortex-a57 -smp 2 -m 512M -device ramfb -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 -device virtio-gpu-pci \
    -display none -machine dumpdtb=phasea-iommu-topology.dtb
```

## Findings

Opt-in topology, `AME_QEMU_IOMMU=1`, 4 PCI functions enumerated:

| BDF | vendor:device | class | requester id | notes |
|-----|---------------|-------|--------------|-------|
| 0:0.0 | 1b36:0008 | 060000 | 0x0000 | PCIe host bridge (no BARs) |
| 0:1.0 | 1af4:1000 | 020000 | 0x0008 | virtio-net-pci (QEMU virt default NIC) |
| 0:2.0 | 1b36:000d | 0c0330 | 0x0010 | qemu-xhci; `cmd=0x6 bm=1 mem=1` after enablement, BAR0 `mmio64 base=0x10000000 size=0x4000` |
| 0:3.0 | 1af4:1050 | 038000 | 0x0018 | virtio-gpu-pci |

SMMUv3: `has_smmu=true`, base `0x9050000` (`arm,smmu-v3`, reg `0x9050000+0x20000`).
`virtio_n=32` — all 32 `virtio,mmio` transports are now recorded.

The guest still boots to `all services ready` with the SMMU present (unconfigured
by the guest); `drv.usb.xhci` reaches `controller ready`.

The PCI command register is only *observed* (`bm`/`mem`); `enumerate()` never
writes it, so the native xHCI bus-master enablement stays intact. BAR probing
writes `~0` and restores the original value for every BAR it touches.
