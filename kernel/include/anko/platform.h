#pragma once
// anko/platform.h — MMIO base addresses and hardware constants.
//
// Values are for the QEMU 'virt' machine (-machine virt -cpu cortex-a57).
// Override per-board once real hardware bring-up starts.
// Actual base addresses are also discoverable from DTB; these are fallbacks
// for the early pre-DTB UART used in phase 0.

#include "anko/types.h"

// ---------------------------------------------------------------------------
// QEMU virt defaults (used before DTB is parsed)
// ---------------------------------------------------------------------------
namespace platform {

// PL011 UART — only used for early boot log and panic.
// After DTB parse, the address confirmed from /uart@.../reg.
constexpr PhysAddr kUartBase    = 0x0900'0000;
constexpr PhysAddr kFwCfgBase   = 0x0902'0000; // QEMU fw_cfg MMIO (DEVICE_BIG_ENDIAN)
constexpr PhysAddr kGuestUartBase = 0x0904'0000;
constexpr u32      kUartDR      = 0x000; // data register
constexpr u32      kUartFR      = 0x018; // flag register
constexpr u32      kUartFR_TXFF = 1u << 5; // TX FIFO full

// First virtio-mmio transport on QEMU virt. scripts/run-qemu.sh attaches a
// virtio-blk-device here for the native filesystem service.
constexpr PhysAddr kVirtioBlkMmioBase = 0x0A00'0000;
constexpr u64      kVirtioMmioSize = 0x4000;

// PCIe ECAM window. The temporary xHCI probe only scans buses 0..15, so the
// bootstrap map can stay to the first 16 MiB until the userland PCI service
// owns full ECAM discovery.
constexpr PhysAddr kPciEcamBase = 0x40'1000'0000ull;
constexpr u64      kPciEcamProbeSize = 16ull * 32ull * 8ull * 4096ull;

// GICv2 — QEMU virt uses GICv2 by default.
constexpr PhysAddr kGicDistBase = 0x0800'0000; // distributor
constexpr PhysAddr kGicCpuBase  = 0x0801'0000; // CPU interface

// ARM Generic Timer — interrupt IDs.
constexpr u32 kTimerIrqPhysNS   = 30; // PPI 14 (ID 30) — non-secure physical timer

// RAM — QEMU virt starts DRAM here; confirmed from DTB /memory.
constexpr PhysAddr kRamBase     = 0x4000'0000;

// Kernel virtual base — top half, TTBR1.
constexpr VirtAddr kKernelVaBase   = 0xFFFF'0000'0000'0000ULL;
constexpr VirtAddr kKernelDevBase  = 0xFFFF'FFFF'0000'0000ULL;

// Page table config for MAIR_EL1: two attribute indices.
constexpr u8 kMairNormal    = 0; // normal, inner/outer write-back cacheable
constexpr u8 kMairNormalNc  = 2; // normal, outer/inner noncacheable
constexpr u8 kMairDevice    = 1; // device-nGnRnE

} // namespace platform
