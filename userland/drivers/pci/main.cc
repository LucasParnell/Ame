// userland/drivers/pci/main.cc — drv.pci enumeration service (M10).
//
// NOTE: This is a PLACEHOLDER for M10. The actual PCI discovery is currently
// performed by kernel/drivers/pci_probe.cc (M3.5 temporary shortcut).
// This service will replace that shortcut when ready.
//
// When implemented, drv.pci will:
//  - Map PCI ECAM CAP_MMIO from BootInfo "pci.ecam"
//  - Enumerate config space (all buses, read class/vendor/device)
//  - Program BARs, allocate CAP_MMIO per BAR, CAP_IRQ for INTX/MSI
//  - Publish device capabilities to nameservice

#include "mochi/syscall.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"

constexpr uint64_t kVaBootInfo = 0x1000'0000ull;
constexpr uint64_t kVaLogRing  = 0x1800'0000ull;

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "drv.pci");

    mochi::log_raw_marker("drv.pci: placeholder — M10 not yet implemented\n");
    mochi::boot::ready(boot);
    while (true) { mx_yield(); }
}
