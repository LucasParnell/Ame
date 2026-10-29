#include "anko/hw_info.h"

namespace platform {
// UART serial0 and GIC registers come from the captured running Huawei DTB.
// Begin with 128 MiB of the first bank; firmware holes are reserved before alloc.
static const HwInfo kFallback = {
    .ram_base = 0x00400000,
    .ram_size = 128ull * 1024 * 1024,
    .console_kind = ConsoleKind::PL011,
    .console_base = 0xfdf02000,
    .intc_kind = IntcKind::GICv2,
    .gic_dist_base = 0xe82b1000,
    .gic_cpu_base = 0xe82b2000,
    .timer_irq = 30,
    .boot_el = 1,
    .has_fwcfg = false,
    .has_virtio_blk = false,
    .has_pcie = false,
    .fwcfg_base = 0,
    .virtio_base = 0,
    .ecam_base = 0,
    .has_t210_dc = false,
    .t210_dc_fb_phys = 0,
    .t210_dc_fb_width = 0,
    .t210_dc_fb_height = 0,
    .xhci_noreset = false,
};
const HwInfo& get_board_fallback() { return kFallback; }
}
