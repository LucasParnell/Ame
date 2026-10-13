#pragma once
// Controller mechanisms shared by the DesignWare host and device executors.
// The platform owner prepares clocks/PHY first; no board registers, USB class
// requests or role-selection policy belong here. Mutable state is caller-owned.
#include "midix/usb/dwc2.h"
#include "libk/mmio.h"
#include "libk/dma.h"
#include "libk/integer.h"

namespace mochi::usb::dwc2 {
using namespace midix::usb::dwc2;

// A small cold operations table permits a side-effectful register model in
// host tests. Real hardware uses libk::Mmio; ownership stays with its service.
struct Registers {
    void* context = nullptr;
    bool (*read)(void*, mx_u32_t, mx_u32_t*) = nullptr;
    bool (*write)(void*, mx_u32_t, mx_u32_t) = nullptr;
    bool (*receive)(void*, mx_u32_t, libk::bytes) = nullptr;
    bool (*transmit)(void*, mx_u32_t, libk::const_bytes) = nullptr;
    mx_u64_t bytes = 0;

    static Registers mapped(libk::Mmio& mmio) {
        return {&mmio,
            [](void* p, mx_u32_t reg, mx_u32_t* value) {
                return static_cast<libk::Mmio*>(p)->read32(reg, value);
            },
            [](void* p, mx_u32_t reg, mx_u32_t value) {
                return static_cast<libk::Mmio*>(p)->write32(reg, value);
            },
            [](void* p, mx_u32_t reg, libk::bytes buffer) {
                return static_cast<libk::Mmio*>(p)->read_fifo32(reg, buffer);
            },
            [](void* p, mx_u32_t reg, libk::const_bytes buffer) {
                return static_cast<libk::Mmio*>(p)->write_fifo32(reg, buffer);
            }, mmio.size};
    }
    bool valid() const { return context && read && write && receive && transmit; }
};

// The vendor core uses revision-dependent UTMI turnaround values. Width is
// chosen from GHWCFG4 before reset; timing is restored after reset and again
// when ENUMDONE establishes the bus speed. Neither helper selects a board role.
inline mx_u32_t utmi_turnaround(mx_u32_t core_id, bool wide, MxUsbSpeed speed) {
    if (speed != MxUsbSpeed::High) return 9;
    return (core_id & 0xffffu) >= 0x260au ? (wide ? 6u : 9u) : (wide ? 4u : 8u);
}

inline bool configure_utmi(const Registers& registers, const Geometry& geometry, bool host) {
    if (!registers.valid() || geometry.utmi_width > 2 ||
        (geometry.high_speed_phy != 1 && geometry.high_speed_phy != 3)) return false;
    mx_u32_t cfg = 0;
    if (!registers.read(registers.context, kUsbConfig, &cfg)) return false;
    const bool wide = geometry.utmi_width == 1;
    // Keep unrelated core options; disable legacy HNP/SRP and select UTMI.
    constexpr mx_u32_t mask = (1u << 29) | (1u << 30) | (15u << 10) | (1u << 3) |
                              (1u << 4) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 15);
    cfg = (cfg & ~mask) | (wide ? (1u << 3) : 0) |
          (utmi_turnaround(geometry.core_id, wide, MxUsbSpeed::High) << 10) |
          (host ? (1u << 29) : (1u << 30));
    if (!registers.write(registers.context, kUsbConfig, cfg)) return false;
    libk::dma::order_mmio();
    return true;
}

inline bool device_turnaround(const Registers& registers, mx_u32_t core_id, MxUsbSpeed speed) {
    mx_u32_t cfg = 0;
    if (!registers.read(registers.context, kUsbConfig, &cfg)) return false;
    const auto timing = utmi_turnaround(core_id, (cfg & (1u << 3)) != 0, speed);
    if (!registers.write(registers.context, kUsbConfig, (cfg & ~(15u << 10)) | (timing << 10))) {
        return false;
    }
    libk::dma::order_mmio();
    return true;
}

enum class ResetState : mx_u8_t { Idle, WaitingAhb, WaitingReset, Settling, WaitingFlush, Ready, Fault };
struct Reset {
    ResetState state = ResetState::Idle;
    mx_u64_t deadline_ns = 0;
    mx_u64_t settle_until_ns = 0;
    // Begin and advance never spin. The event loop owns the timeout and wakes;
    // reset waits are cold-path work, separate from transfer publication.
    bool begin(const Registers& registers, mx_u64_t deadline) {
        if (!registers.valid() || !deadline ||
            (state != ResetState::Idle && state != ResetState::Ready)) return false;
        // Stop interrupt delivery before destroying either role's state.
        if (!registers.write(registers.context, kInterruptMask, 0) ||
            !registers.write(registers.context, kAhbConfig, 0)) { state = ResetState::Fault; return false; }
        libk::dma::order_mmio();
        deadline_ns = deadline;
        state = ResetState::WaitingAhb;
        return true;
    }
    ResetState advance(const Registers& registers, mx_u64_t now_ns) {
        if (state == ResetState::Idle || state == ResetState::Ready || state == ResetState::Fault)
            return state;
        mx_u32_t value = 0;
        if (now_ns >= deadline_ns || !registers.read(registers.context, kReset, &value)) {
            state = ResetState::Fault;
            return state;
        }
        if (state == ResetState::WaitingAhb && (value & kAhbIdle)) {
            if (!registers.write(registers.context, kReset, kSoftReset)) state = ResetState::Fault;
            else state = ResetState::WaitingReset;
        } else if (state == ResetState::WaitingReset && !(value & kSoftReset) && (value & kAhbIdle)) {
            // Allow at least three PHY clocks after soft-reset deassertion.
            // 1 us covers the USB2 6/48/60 MHz PHY clocks without a spin loop.
            if (!libk::checked_add(now_ns, 1000, &settle_until_ns)) state = ResetState::Fault;
            else state = ResetState::Settling;
        } else if (state == ResetState::Settling && now_ns >= settle_until_ns) {
            // TXFNUM=0x10 selects all transmit FIFOs; receive flush is separate.
            if (!registers.write(registers.context, kReset, kRxFlush | kTxFlush | (0x10u << 6)))
                state = ResetState::Fault;
            else state = ResetState::WaitingFlush;
        } else if (state == ResetState::WaitingFlush && !(value & (kRxFlush | kTxFlush))) {
            libk::dma::order_mmio();
            state = ResetState::Ready;
        }
        return state;
    }
};

inline bool identify(const Registers& registers, Geometry* geometry) {
    if (!registers.valid() || !geometry || registers.bytes < fifo_register(0) + 4) return false;
    mx_u32_t id = 0, hw1 = 0, hw2 = 0, hw3 = 0, hw4 = 0;
    if (!registers.read(registers.context, kCoreId, &id) ||
        !registers.read(registers.context, kHardware1, &hw1) ||
        !registers.read(registers.context, kHardware2, &hw2) ||
        !registers.read(registers.context, kHardware3, &hw3) ||
        !registers.read(registers.context, kHardware4, &hw4)) return false;
    Geometry candidate{};
    if (!decode_geometry(id, hw1, hw2, hw3, hw4, &candidate)) return false;
    const auto fifos = candidate.host_channels > candidate.device_endpoints ?
                       candidate.host_channels : candidate.device_endpoints;
    if (registers.bytes < fifo_register(fifos - 1) + 4) return false;
    *geometry = candidate;
    return true;
}

// Call only after reset completion and after observing host mode. The caller
// chooses a FIFO budget; fixed-FIFO cores must match it exactly, without writes.
inline bool prepare_host_pio(const Registers& registers, const Geometry& geometry,
                              const HostFifos& layout) {
    mx_u32_t mode = 0;
    if (!registers.valid() || !geometry.host || !valid_host_fifos(layout, geometry.fifo_words) ||
        !registers.read(registers.context, kInterruptStatus, &mode) || !(mode & kHostMode)) return false;
    const mx_u32_t offsets[] = {kRxFifoSize, kNonperiodicTxSize, kPeriodicTxSize};
    const mx_u32_t values[] = {layout.rx.words, fifo_size(layout.nonperiodic), fifo_size(layout.periodic)};
    for (mx_u32_t i = 0; i < 3; ++i) {
        if (geometry.dynamic_fifo) {
            if (!registers.write(registers.context, offsets[i], values[i])) return false;
        } else {
            mx_u32_t observed = 0;
            if (!registers.read(registers.context, offsets[i], &observed) || observed != values[i]) return false;
        }
    }
    if (!registers.write(registers.context, kHostInterruptMask, 0) ||
        !registers.write(registers.context, kAhbConfig, kGlobalInterruptEnable)) return false;
    libk::dma::order_mmio();
    mx_u32_t ahb = 0;
    // PIO never grants bus-master authority or installs a physical DMA address.
    return registers.read(registers.context, kAhbConfig, &ahb) && !(ahb & kDmaEnable);
}
} // namespace mochi::usb::dwc2
