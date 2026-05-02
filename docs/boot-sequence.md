# Boot sequence

## Overview

```
[ Power on / reset ]
        │
        ▼
[ Firmware (ATF / TF-A, EL3) ]
   - Secure world init
   - Drops to EL2
        │
        ▼
[ U-Boot (EL2) ]
   - Board init, DRAM init
   - Loads midix-boot.elf into DRAM
   - Sets x0 = DTB physical address
   - Jumps to _start
        │
        ▼
[ midix-boot — entry/start.S ]
   - Verify EL; drop EL2→EL1 if needed
   - Disable MMU + caches (clean slate)
   - Set up early stack (__boot_stack_top from linker script)
   - Zero BSS
   - Call boot_main(x0)  [x0 = dtb_phys]
        │
        ▼
[ midix-boot — entry/boot_main.cpp ]
   ┌─ 1. DTB parse  (boot/dtb/fdt.cpp)
   │     - Validate magic 0xD00DFEED
   │     - Walk structure block tokens
   │     - Extract: memory regions, chosen/stdout-path, UART MMIO base,
   │                CPU topology, GIC base, generic timer freq
   │
   ├─ 2. Physical memory map  (boot/memory/phys_map.cpp)
   │     - Build free list from DTB /memory nodes
   │     - Reserve: DTB blob, midix-boot image, page-table bump region
   │     - Reserve: /reserved-memory nodes
   │
   ├─ 3. MMU setup  (boot/mmu/)
   │     - Allocate PGD/PUD/PMD pages from bump allocator
   │     - Identity map: boot image + page tables (trampoline safety)
   │     - Kernel window: all DRAM → KERNEL_VA_BASE + phys
   │     - Device window: MMIO regions → DEVICE_VA_BASE + phys
   │     - Write MAIR_EL1, TCR_EL1, TTBR0_EL1, TTBR1_EL1
   │     - Enable MMU (SCTLR_EL1.M | I | C), ISB
   │     - Relocate SP to virtual address, unmap identity map
   │
   └─ 4. Jump to kernel_main(BootInfo&)
        │
        ▼
[ kernel — kernel_main() ]
   1. Early UART (pre-GIC console)
   2. Per-CPU data init
   3. GIC (GICv2 or GICv3) init
   4. Generic timer init
   5. Physical memory hand-off → arena bootstrap
   6. Capability table init
   7. IPC ring subsystem init
   8. Scheduler init (per-CPU run queues, MPSC pending-wake queues)
   9. Create init process, issue root capabilities
  10. Enter scheduler  [never returns]
```

## Exception level strategy

| Stage        | Target EL | Notes                                  |
|--------------|-----------|----------------------------------------|
| midix-boot   | EL1       | Drops from EL2 if U-Boot left us there |
| kernel       | EL1       | Standard OS privilege level            |
| User space   | EL0       | Unprivileged                           |
| Linux VM     | EL1 (guest) | Managed by Midix hypervisor (future) |

## Virtual memory layout (after MMU on)

| Region                         | VA range                              |
|--------------------------------|---------------------------------------|
| User space                     | 0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF |
| *(gap)*                        | non-canonical                         |
| Kernel image + data            | 0xFFFF_0000_0000_0000 + phys offset   |
| MMIO / device windows          | 0xFFFF_FFFF_0000_0000 + phys offset   |

TTBR0_EL1 → user (identity/process page tables)
TTBR1_EL1 → kernel window
