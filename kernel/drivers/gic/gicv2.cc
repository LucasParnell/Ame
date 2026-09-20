// kernel/drivers/gic/gicv2.cc — GICv2 interrupt controller.
//
// Spec ref: §5.10 IRQ and Timer
//
// QEMU -machine virt uses GICv2 by default.
// Distributor base: platform::kGicDistBase
// CPU interface base: platform::kGicCpuBase
//
// V0 needs:
//   - Enable distributor (GICD_CTLR).
//   - Enable CPU interface (GICC_CTLR), set GICC_PMR to allow all priorities.
//   - Enable the generic timer PPI (IRQ 30).
//   - handle_irq(): read GICC_IAR → dispatch → GICC_EOIR.

#include "anko/platform.h"
#include "anko/hw_info.h"
#include "anko/types.h"
#include "anko/smp.h"

namespace drivers::timer { void handle_tick(); }

namespace drivers::gic {

// GICv2 distributor register offsets
constexpr u32 GICD_CTLR     = 0x000;
constexpr u32 GICD_ISENABLER = 0x100; // +n*4 for IRQs 32n..32n+31
constexpr u32 GICD_ITARGETSR = 0x800; // interrupt target CPU mask
constexpr u32 GICD_SGIR      = 0xF00;

// GICv2 CPU interface register offsets
constexpr u32 GICC_CTLR = 0x000;
constexpr u32 GICC_PMR  = 0x004;
constexpr u32 GICC_IAR  = 0x00C;
constexpr u32 GICC_EOIR = 0x010;

static volatile u32* g_dist = nullptr;
static volatile u32* g_cpu  = nullptr;

void init(PhysAddr dist_base, PhysAddr cpu_base) {
    g_dist = reinterpret_cast<volatile u32*>(dist_base);
    g_cpu = reinterpret_cast<volatile u32*>(cpu_base);

    g_dist[GICD_CTLR / 4] = 0;
    g_cpu[GICC_CTLR / 4] = 0;
    g_cpu[GICC_PMR / 4] = 0xFF;
    g_dist[GICD_CTLR / 4] = 1;
    g_cpu[GICC_CTLR / 4] = 1;
}

void init_cpu() {
    if (!g_cpu) return;
    g_cpu[GICC_CTLR / 4] = 0;
    g_cpu[GICC_PMR / 4] = 0xFF;
    g_cpu[GICC_CTLR / 4] = 1;
}

void irq_enable(u32 irq_id) {
    if (!g_dist) return;
    g_dist[(GICD_ISENABLER / 4) + (irq_id / 32)] = 1u << (irq_id % 32);
    if (irq_id >= 32) {
        g_dist[(GICD_ITARGETSR / 4) + irq_id] = 0x01;
    }
}

void send_sgi(u32 cpu_id, u32 sgi_id) {
    if (!g_dist || cpu_id >= 8 || sgi_id >= 16) return;
    g_dist[GICD_SGIR / 4] = (1u << (16 + cpu_id)) | (sgi_id & 0xFu);
}

// Called from EL1h IRQ vector.
void handle_irq() {
    if (!g_cpu) return;
    const u32 iar = g_cpu[GICC_IAR / 4];
    const u32 irq_id = iar & 0x3FFu;
    if (irq_id == g_hw.timer_irq) {
        drivers::timer::handle_tick();
    } else if (irq_id == 1) {
        smp::handle_ipi();
    }
    if (irq_id != 1023) {
        g_cpu[GICC_EOIR / 4] = iar;
    }
}

} // namespace drivers::gic

extern "C" void irq_dispatch() {
    drivers::gic::handle_irq();
}
