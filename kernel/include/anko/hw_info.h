#pragma once
// anko/hw_info.h — runtime hardware description populated by platform::probe().
//
// g_hw is statically initialised from board_fallback.cc constants and
// overwritten by platform::probe() once the DTB is available.  All
// kernel subsystems should read from g_hw, not from platform.h constants,
// except for Phase-0 early UART which fires before probe().

#include "anko/types.h"

enum class ConsoleKind : u8 { PL011, Tegra16550 };
enum class IntcKind    : u8 { GICv2 };

struct HwInfo {
    PhysAddr    ram_base;
    u64         ram_size;

    ConsoleKind console_kind;
    PhysAddr    console_base;

    IntcKind    intc_kind;
    PhysAddr    gic_dist_base;
    PhysAddr    gic_cpu_base;

    u32         timer_irq;   // always 30 (architectural PPI) on both boards
    u32         boot_el;     // 1 or 2; gates Dango VM path

    // Device presence — probed from DTB; not assumed.
    bool has_fwcfg;
    bool has_virtio_blk;
    bool has_pcie;
    PhysAddr fwcfg_base;
    PhysAddr virtio_base;
    PhysAddr ecam_base;

    // T210 display controller (TegraDC0).  Populated by probe() when
    // console_kind == Tegra16550 by reading DC0 window-A registers directly.
    bool     has_t210_dc;
    PhysAddr t210_dc_fb_phys;   // physical base of active framebuffer
    u32      t210_dc_fb_width;  // pixels (horizontal)
    u32      t210_dc_fb_height; // lines  (vertical)

    // T210 XUSB: Hekate pre-loads FALCON firmware into IMEM.
    // When true, the xHCI driver must skip HCRST (which would erase FALCON).
    bool xhci_noreset;

    // virtio-mmio transports.  probe() records every "virtio,mmio" node it
    // finds in the DTB, not just the first.  virtio_bases[0] is the instance the
    // legacy has_virtio_blk/virtio_base fields point at.
    static constexpr u32 kMaxVirtioMmio = 32;
    u32      virtio_count = 0;
    PhysAddr virtio_bases[kMaxVirtioMmio] = {};

    // SMMUv3 (compatible = "arm,smmu-v3") presence and register base, from DTB.
    bool     has_smmu = false;
    PhysAddr smmu_base = 0;
};

// Single global hardware description.
extern HwInfo g_hw;

namespace platform {
// Fill g_hw from the DTB at dtb_phys; falls back to board constants on error.
// Must be called before mm::phys_init().
void probe(PhysAddr dtb_phys);
} // namespace platform
