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
#include "anko/irq.h"
#include "anko/hw_info.h"
#include "anko/types.h"
#include "anko/smp.h"
#include "midix/abi/cpu_limits.h"

namespace drivers::timer { void handle_tick(); }

namespace drivers::gic {

// GICv2 distributor register offsets
constexpr u32 GICD_CTLR      = 0x000;
constexpr u32 GICD_ISENABLER = 0x100; // +n*4 for IRQs 32n..32n+31
constexpr u32 GICD_ICENABLER = 0x180; // write-1-to-clear enable
constexpr u32 GICD_ISPENDR   = 0x200; // write-1-to-set pending
constexpr u32 GICD_ICPENDR   = 0x280; // write-1-to-clear pending
constexpr u32 GICD_IPRIORITYR = 0x400; // byte per IRQ
constexpr u32 GICD_ITARGETSR = 0x800; // byte per IRQ (SPIs)
constexpr u32 GICD_ICFGR     = 0xC00; // 2 bits per IRQ; bit+1 = edge
constexpr u32 GICD_SGIR      = 0xF00;

// GICv2 CPU interface register offsets
constexpr u32 GICC_CTLR = 0x000;
constexpr u32 GICC_PMR  = 0x004;
constexpr u32 GICC_IAR  = 0x00C;
constexpr u32 GICC_EOIR = 0x010;

static volatile u32* g_dist = nullptr;
static volatile u32* g_cpu  = nullptr;
static u32 g_cpu_targets[MX_HOST_MAX_CPUS]{};

static void record_cpu_target() {
    u64 logical;
    asm volatile("mrs %0, tpidr_el1" : "=r"(logical));
    if (logical >= MX_HOST_MAX_CPUS) return;
    u32 target = 0;
    // SGI/PPI target bytes are banked and identify this CPU's GIC interface.
    for (u32 i = 0; i < 8 && !target; ++i) {
        const u32 bytes = g_dist[GICD_ITARGETSR / 4 + i];
        target = (bytes | (bytes >> 8) | (bytes >> 16) | (bytes >> 24)) & 0xff;
    }
    __atomic_store_n(&g_cpu_targets[logical], target, __ATOMIC_RELEASE);
}

u32 cpu_target(u32 logical) {
    return logical < MX_HOST_MAX_CPUS ? __atomic_load_n(&g_cpu_targets[logical], __ATOMIC_ACQUIRE) : 0;
}

void init(PhysAddr dist_base, PhysAddr cpu_base) {
    g_dist = reinterpret_cast<volatile u32*>(dist_base);
    g_cpu = reinterpret_cast<volatile u32*>(cpu_base);

    g_dist[GICD_CTLR / 4] = 0;
    g_cpu[GICC_CTLR / 4] = 0;
    g_cpu[GICC_PMR / 4] = 0xFF;
    g_dist[GICD_CTLR / 4] = 1;
    g_cpu[GICC_CTLR / 4] = 1;
    record_cpu_target();
}

void init_cpu() {
    if (!g_cpu) return;
    g_cpu[GICC_CTLR / 4] = 0;
    g_cpu[GICC_PMR / 4] = 0xFF;
    g_cpu[GICC_CTLR / 4] = 1;
    record_cpu_target();
}

void irq_enable(u32 irq_id) {
    if (!g_dist) return;
    g_dist[(GICD_ISENABLER / 4) + (irq_id / 32)] = 1u << (irq_id % 32);
    if (irq_id >= 32) {
        g_dist[(GICD_ITARGETSR / 4) + irq_id] = 0x01;
    }
}

void send_sgi(u32 cpu_id, u32 sgi_id) {
    const u32 target = cpu_target(cpu_id);
    if (!g_dist || !target || sgi_id >= 16) return;
    asm volatile("dsb ishst" ::: "memory");
    g_dist[GICD_SGIR / 4] = (target << 16) | (sgi_id & 0xFu);
}

// --- GICv2 SPI programming (drivers for the irq binding table, see anko/irq.h) --
//
// Written as bit/byte helpers because the distributor mixes both layouts:
// enable/pending state is one bit per IRQ, priority/target/trigger are per-IRQ
// fields sharing a 32-bit word. Only SPIs (>= 32) are ever touched here.

static void dist_write_bit(u32 base, u32 irq_id) {
    const u32 word = (base / 4) + (irq_id / 32);
    g_dist[word] = 1u << (irq_id % 32);
}

static void dist_write_byte(u32 base, u32 irq_id, u8 value) {
    const u32 word = (base / 4) + (irq_id / 4);
    const u32 shift = (irq_id % 4) * 8u;
    u32 v = g_dist[word];
    v = (v & ~(0xFFu << shift)) | (static_cast<u32>(value) << shift);
    g_dist[word] = v;
}

void spi_configure(u32 irq_id, u32 trigger, u32 target_cpu) {
    if (!g_dist || irq_id < 32 || irq_id >= 1020) return;
    dist_write_byte(GICD_IPRIORITYR, irq_id, 0xA0); // below PMR (0xFF)
    // ICFGR: bit (2*(n%16)+1) = 1 selects edge, 0 selects level.
    {
        const u32 word = (GICD_ICFGR / 4) + (irq_id / 16);
        const u32 bit = ((irq_id % 16) * 2u) + 1u;
        u32 v = g_dist[word];
        if (trigger) v |= (1u << bit);
        else         v &= ~(1u << bit);
        g_dist[word] = v;
    }
    const u32 targets = (target_cpu < 8) ? (1u << target_cpu) : 1u;
    dist_write_byte(GICD_ITARGETSR, irq_id, static_cast<u8>(targets));
    asm volatile("dsb sy" ::: "memory");
}

void spi_enable(u32 irq_id) {
    if (!g_dist || irq_id < 32 || irq_id >= 1020) return;
    dist_write_bit(GICD_ISENABLER, irq_id);
}

void spi_disable(u32 irq_id) {
    if (!g_dist || irq_id < 32 || irq_id >= 1020) return;
    dist_write_bit(GICD_ICENABLER, irq_id);
}

void spi_set_pending(u32 irq_id) {
    if (!g_dist || irq_id < 32 || irq_id >= 1020) return;
    dist_write_bit(GICD_ISPENDR, irq_id);
}

void spi_clear_pending(u32 irq_id) {
    if (!g_dist || irq_id < 32 || irq_id >= 1020) return;
    dist_write_bit(GICD_ICPENDR, irq_id);
}

u32 iar_id() {
    if (!g_cpu) return 1023;
    return g_cpu[GICC_IAR / 4] & 0x3FFu;
}

void eoi(u32 irq_id) {
    if (!g_cpu || irq_id >= 1023) return;
    g_cpu[GICC_EOIR / 4] = irq_id;
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
    } else if (irq_id < 1023) {
        // Bound SPI → wake the driver's doorbell. Timer and IPI are claimed
        // above; irq::deliver() also rejects any non-SPI id.
        irq::deliver(irq_id);
    }
    if (irq_id != 1023) {
        g_cpu[GICC_EOIR / 4] = iar;
    }
}

} // namespace drivers::gic

extern "C" void irq_dispatch() {
    drivers::gic::handle_irq();
}
