#pragma once
// anko/irq.h — IRQ → doorbell delivery bindings.
//
// Spec ref: §5.10 IRQ and Timer, §18 Masking-at-Entry
//
// A driver hands the kernel an IRQ line capability plus a doorbell capability
// (SYS_IRQ_BIND_DOORBELL). The kernel programmes the interrupt controller so
// the line is delivered to the driver's CPU, then on delivery notifies the
// doorbell. The binding table is a fixed, power-of-two-indexed array: a hostile
// irq_id can never index outside it, even speculatively (Architecture_3.md §18).
//
// Owned lines only: SGIs (0-15) and PPIs (16-31) belong to the kernel (timer,
// IPI) and are never redirected to a driver.

#include "anko/types.h"
#include "anko/cap.h"
#include "anko/syscall.h"

struct Doorbell;

// Interrupt-controller primitives. In the kernel they are backed by the
// board's GICv2 distributor/CPU interface (drivers/gic/gicv2.cc); the host
// regression substitutes a fake controller. SPI ids are >= 32 by definition.
namespace drivers::gic {
void spi_configure(u32 irq_id, u32 trigger, u32 target_cpu);
void spi_enable(u32 irq_id);
void spi_disable(u32 irq_id);
void spi_set_pending(u32 irq_id);
void spi_clear_pending(u32 irq_id);
u32  iar_id();  // acknowledged interrupt id from the CPU interface, or 1023
void eoi(u32 irq_id);
} // namespace drivers::gic

namespace irq {

// One binding slot per interrupt id in a power-of-two range. 256 covers every
// SPI both supported GICv2 distributors expose (QEMU virt, Tegra T210).
constexpr u32 kIrqSlots = 256; // power of two
constexpr u32 kFirstSpi = 32;  // SGIs (0-15) and PPIs (16-31) are kernel-owned

// Mask an untrusted interrupt id into the table's range *before* any table
// access, then reject anything the mask aliased (sentinel, PPI/SGI, or an id
// beyond the distributor). Masking is independent of the range check so the
// speculative path never leaves the array.
constexpr u32 irq_index(u32 irq_id) { return irq_id & (kIrqSlots - 1); }
constexpr bool irq_bindable(u32 irq_id) {
    return irq_id >= kFirstSpi && irq_id < kIrqSlots;
}

// Bind `line` to `doorbell`. `owner` is the caller's process identity, used to
// scope teardown. Programmes and enables the SPI. E_BUSY if the line is bound
// to a different doorbell; E_OK (idempotent) if already bound to this one.
SysError bind(IrqLine* line, Doorbell* doorbell, u64 owner, u32 flags);

// Acknowledge a delivery. Level lines are paused on delivery so a still-asserted
// source cannot storm the CPU; the driver clears its device and acks to re-arm.
// Edge lines are EOI'd at IRQ and need no re-arm, so ack is a no-op.
SysError ack(IrqLine* line, u64 owner);

// Tear a line down (capability revocation). `owner` must match the binder.
void unbind_line(IrqLine* line, u64 owner);
// Tear every line bound by a process down (process exit).
void unbind_owner(u64 owner);

// Delivery entry point. Called from the IRQ vector with the acknowledged id.
void deliver(u32 irq_id);

// Debug counters (SYS_DEBUG_COUNTERS).
u64 delivered_count();
u64 binding_count();
u64 selftest_status(); // 0 = not run, 1 = pass, 2 = fail

// Boot-time end-to-end delivery check (dev builds). Binds a spare SPI, raises
// it through the distributor, and confirms the doorbell fires. IRQs must be
// masked by the caller; returns the status code it records.
u64 run_selftest();

} // namespace irq
