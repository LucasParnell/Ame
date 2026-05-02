#pragma once
// boot/hw_info.h — hardware information discovered from DTB.
// Passed from boot stage to kernel_main.
//
// Spec ref: §8 V0 Boot Flow steps 4–5
// DTB fields needed: RAM, UART, GIC, timer freq, CPU count.

#include <cstdint>

struct HwInfo {
    // RAM — from /memory@*/reg (spec §5.2, §8 step 5)
    uint64_t ram_base;
    uint64_t ram_size;

    // UART — from /chosen stdout-path → resolved node reg
    uint64_t uart_base;

    // GIC — from /intc/reg (distributor, CPU interface)
    uint64_t gic_dist_base;
    uint64_t gic_cpu_base;

    // Timer — read from CNTFRQ_EL0; DTB may also carry it
    uint32_t timer_freq_hz;
    uint32_t timer_irq;

    // CPU topology — V0 only requires single-core correctness
    uint32_t cpu_count;

    // Physical address of the initrd (init ram disk with services + manifest)
    uint64_t initrd_base;
    uint64_t initrd_size;

    // Physical address of the DTB blob itself (kernel needs it for identity map teardown)
    uint64_t dtb_base;
    uint64_t dtb_size;
};
