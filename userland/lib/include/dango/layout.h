#pragma once

#include <cstdint>

#include "midix/abi/dango_channel_guest.h"

namespace dango {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kRamBase = 0x4000'0000ull;
constexpr uint64_t kRamSize = 128ull * 1024ull * 1024ull;
constexpr uint64_t kKernelLoad = 0x4020'0000ull;
constexpr uint64_t kInitramfsLoad = 0x4600'0000ull;
constexpr uint64_t kDtbBase = 0x4700'0000ull;
constexpr uint64_t kDtbSize = 0x0001'0000ull;
constexpr uint64_t kMmioBase = 0x0800'0000ull;
constexpr uint64_t kMmioSize = 0x0110'0000ull;
constexpr uint64_t kUartBase = 0x0900'0000ull;
constexpr uint64_t kGicDistBase = 0x0800'0000ull;
constexpr uint64_t kGicCpuBase = 0x0801'0000ull;
// `hello` profile: the channel window baked into dango-linux-minimal.dts.in.
constexpr uint64_t kChannelBase = 0x4780'0000ull;
constexpr uint64_t kChannelSize = 0x0001'0000ull;

// GPU profile: the guest binding is defined once in the shared ABI header
// (midix/abi/dango_channel_guest.h) so the manager, the DTS generator and the
// Linux driver cannot drift. dango-linux-gpu.dts.in is generated from the same
// macros, so these aliases describe exactly the device tree the guest sees.
constexpr uint64_t kChannelGuestBase = DANGO_CH_GUEST_BASE;
constexpr uint64_t kChannelGuestSize = DANGO_CH_GUEST_SIZE;
constexpr uint32_t kChannelGuestIrq = DANGO_CH_GUEST_IRQ;

constexpr uint64_t kGpuEcamBase = DANGO_GPU_ECAM_BASE;
constexpr uint64_t kGpuEcamSize = DANGO_GPU_ECAM_SIZE;
constexpr uint64_t kGpuBar4Ipa = DANGO_GPU_BAR4_IPA;
constexpr uint64_t kGpuBar4Size = DANGO_GPU_BAR4_SIZE;
// PCI INTx is routed to a GIC SPI: 32 + DANGO_GPU_INTX_SPI (see the guest
// binding header and the interrupt-map in dango-linux-gpu.dts.in).
constexpr uint32_t kGpuIntxIrq = DANGO_GIC_SPI_BASE + DANGO_GPU_INTX_SPI;

// Host-side VAs the manager maps the two channel rings at. These are in the
// manager's own address space, distinct from the guest binding above.
constexpr uint64_t kChannelHostTxVa = 0x3000'0000ull;
constexpr uint64_t kChannelHostRxVa = 0x3001'0000ull;

struct VmLayout {
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t kernel_load;
    uint64_t initramfs_load;
    uint64_t dtb_base;
    uint64_t dtb_size;
    uint64_t mmio_base;
    uint64_t mmio_size;
    uint64_t channel_base;
    uint64_t channel_size;
};

constexpr VmLayout v3_layout() {
    return {
        kRamBase,
        kRamSize,
        kKernelLoad,
        kInitramfsLoad,
        kDtbBase,
        kDtbSize,
        kMmioBase,
        kMmioSize,
        kChannelBase,
        kChannelSize,
    };
}

} // namespace dango
