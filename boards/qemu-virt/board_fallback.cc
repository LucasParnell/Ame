// boards/qemu-virt/board_fallback.cc — compile-time HwInfo for QEMU virt.
//
// platform::probe() falls back to these constants when the DTB is absent
// or corrupt.  Values are for: -machine virt -cpu cortex-a57 -m 512M.

#include "anko/hw_info.h"
#include "anko/platform.h"

namespace platform {

static const HwInfo kFallback = {
    .ram_base      = platform::kRamBase,           // 0x40000000
    .ram_size      = 512ull * 1024ull * 1024ull,   // 512 MiB
    .console_kind  = ConsoleKind::PL011,
    .console_base  = platform::kUartBase,           // 0x09000000
    .intc_kind     = IntcKind::GICv2,
    .gic_dist_base = platform::kGicDistBase,        // 0x08000000
    .gic_cpu_base  = platform::kGicCpuBase,         // 0x08010000
    .timer_irq     = platform::kTimerIrqPhysNS,     // 30
    .boot_el       = 2,                             // updated at runtime
    // QEMU virt board has these devices at fixed MMIO addresses.
    // Used when DTB is absent (legacy -kernel path or corrupted DTB).
    .has_fwcfg      = true,
    .has_virtio_blk = true,
    .has_pcie       = true,
    .fwcfg_base     = 0x09020000ull,   // fw_cfg MMIO (QEMU virt fixed)
    .virtio_base    = 0x0a000000ull,   // first virtio-mmio slot
    .ecam_base      = 0x3f000000ull,   // PCIe ECAM (QEMU virt fixed)
    .has_t210_dc   = false,
    .t210_dc_fb_phys   = 0,
    .t210_dc_fb_width  = 0,
    .t210_dc_fb_height = 0,
    .xhci_noreset  = false,
};

const HwInfo& get_board_fallback() { return kFallback; }

} // namespace platform
