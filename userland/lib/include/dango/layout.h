#pragma once

#include <cstdint>

#include "midix/abi/dango_channel_guest.h"

namespace dango {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kRamBase = DANGO_RAM_BASE;
constexpr uint64_t kRamSize = DANGO_RAM_SIZE;
constexpr uint64_t kKernelLoad = DANGO_KERNEL_LOAD;
constexpr uint64_t kInitramfsLoad = DANGO_INITRAMFS_LOAD;
constexpr uint64_t kDtbBase = DANGO_DTB_LOAD;
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

// GPU shared staging window (spec §11.2/§12.2): the fixed guest-visible buffer
// `drv.gpu.linux` renders into and registers with the appliance. Like the
// channel binding above it is aliased from the shared ABI header rather than
// retyped, so the manager, the DTS generator and the Linux driver agree.
constexpr uint64_t kStagingGuestIpa = DANGO_GPU_STAGING_BASE;
constexpr uint64_t kStagingSize = DANGO_GPU_STAGING_SIZE;

constexpr uint32_t kGpuModeWidth = DANGO_GPU_MODE_WIDTH;
constexpr uint32_t kGpuModeHeight = DANGO_GPU_MODE_HEIGHT;
constexpr uint32_t kGpuModeBytesPerPixel = DANGO_GPU_MODE_BYTES_PER_PIXEL;

// Host-side VAs the manager maps the two channel rings at. These are in the
// manager's own address space, distinct from the guest binding above.
constexpr uint64_t kChannelHostTxVa = 0x3000'0000ull;
constexpr uint64_t kChannelHostRxVa = 0x3001'0000ull;
// Host-side VA reserved for the staging window, alongside the ring VAs above.
// The manager fills the staging buffer through sys_vm_memory_write (a
// CAP_VM_MEMORY cannot be mapped into a process), so this slot stays unmapped
// today; it is named here so the manager's host window map is stated in one
// place, and the asserts below keep it disjoint from the ring mappings.
constexpr uint64_t kStagingHostVa = 0x3002'0000ull;

// Windows are disjoint when neither contains the start of the other. Every
// constant here is small enough that base + size cannot wrap.
constexpr bool windows_disjoint(uint64_t a_base, uint64_t a_size,
                                uint64_t b_base, uint64_t b_size) {
    return a_base + a_size <= b_base || b_base + b_size <= a_base;
}

// The GPU-profile apertures are distinct stage-2 mappings with different
// cacheability and DMA ownership: the guest MMIO window (GIC/UART), the
// assigned function's BAR4 aperture, the shared staging buffer and the guest
// RAM must never overlap. A collision would otherwise surface late, as a
// stage-2 mapping error at bring-up.
static_assert(windows_disjoint(kMmioBase, kMmioSize, kGpuBar4Ipa, kGpuBar4Size),
              "guest MMIO window overlaps the GPU BAR4 aperture");
static_assert(windows_disjoint(kMmioBase, kMmioSize, kStagingGuestIpa, kStagingSize),
              "guest MMIO window overlaps the GPU staging window");
static_assert(windows_disjoint(kGpuBar4Ipa, kGpuBar4Size, kStagingGuestIpa, kStagingSize),
              "GPU BAR4 aperture overlaps the GPU staging window");
static_assert(windows_disjoint(kRamBase, kRamSize, kStagingGuestIpa, kStagingSize),
              "GPU staging window must live outside guest RAM");
static_assert(windows_disjoint(kChannelGuestBase, kChannelGuestSize,
                               kStagingGuestIpa, kStagingSize),
              "GPU staging window overlaps the channel window");

// The manager's own window VAs must not collide either, or one mapping would
// silently shadow another.
static_assert(windows_disjoint(kChannelHostTxVa, kPageSize, kChannelHostRxVa, kPageSize) &&
                  windows_disjoint(kChannelHostTxVa, kPageSize, kStagingHostVa, kPageSize) &&
                  windows_disjoint(kChannelHostRxVa, kPageSize, kStagingHostVa, kPageSize),
              "manager host-side window VAs must not overlap");

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
