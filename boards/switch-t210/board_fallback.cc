// boards/switch-t210/board_fallback.cc — compile-time HwInfo for Nintendo Switch T210.
//
// platform::probe() falls back to these constants when the DTB is absent
// or corrupt.  Values confirmed from tegra210.dtsi and switchroot U-Boot.
//
// DRAM: 0x80000000, kernel loads at 0x90000000 (clear of U-Boot + TrustZone).
// UART: UARTA (16550 core) at 0x70006000.
// GIC:  GIC-400 (GICv2-compatible) dist=0x50041000 cpu=0x50042000.

#include "anko/hw_info.h"

namespace platform {

static const HwInfo kFallback = {
    .ram_base      = 0x8000'0000ull,
    .ram_size      = 2ull * 1024ull * 1024ull * 1024ull, // L4T-visible window; probe refines
    .console_kind  = ConsoleKind::Tegra16550,
    .console_base  = 0x7000'6000ull,   // UARTA
    .intc_kind     = IntcKind::GICv2,
    .gic_dist_base = 0x5004'1000ull,   // GIC-400 distributor
    .gic_cpu_base  = 0x5004'2000ull,   // GIC-400 CPU interface
    .timer_irq     = 30,               // architectural PPI, same as QEMU
    .boot_el       = 1,                // switchroot U-Boot hands off at EL1
    .has_fwcfg     = false,
    .has_virtio_blk = false,
    .has_pcie      = false,
    .fwcfg_base    = 0,
    .virtio_base   = 0,
    .ecam_base     = 0,
    // DC discovery happens in probe(); cleared here as safe default.
    .has_t210_dc   = false,
    .t210_dc_fb_phys   = 0,
    .t210_dc_fb_width  = 0,
    .t210_dc_fb_height = 0,
    // T210 XUSB FALCON firmware is pre-loaded by Hekate; never issue HCRST.
    .xhci_noreset  = true,
};

const HwInfo& get_board_fallback() { return kFallback; }

} // namespace platform
