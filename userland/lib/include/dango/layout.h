#pragma once

#include <cstdint>

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
constexpr uint64_t kChannelBase = 0x4780'0000ull;
constexpr uint64_t kChannelSize = 0x0001'0000ull;

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
