#pragma once
#include "display_diagnostics.h"
#include "midix/abi/protocols/gpu_diag.h"

// Fixed Hi6250 video-mode path. Register offsets and update sequence follow
// pinned soc_dss_interface.h / hisi_overlay_utils.c. No caller-supplied MMIO.
namespace anne::scanout {
// Hi6250's ISR uses GLB_GLB_CPU_ITF0_INTS, not the unused LDI-local bank.
constexpr u32 kInterrupt = 0x180, kSync = 1u << 4, kStart = 1u << 7;
constexpr u32 kUnderflow = 1u << 2;
constexpr u32 kMutex = 0x10804;
constexpr u32 kVstate = 0x7d054, kActive = 0x8; // hisi_overlay_utils.h LDI_VSTATE_VACTIVE0
constexpr u32 kWords = 13;
struct Registers {
    u32 offset[kWords], console[kWords], frame[kWords];
};
inline bool layout(const native_display::Pipeline& p, Registers& r) {
    if (native_display::rejection(p) || ((p.dma.control >> 3) & 31) < 5) {
        return false;
    }
    u32 layer = 6;
    for (u32 i = 0; i < 6; ++i) {
        if (!(p.layer_config[i] & 1)) {
            continue;
        }
        if (layer != 6 || ((p.channel_select >> ((i + 1) * 4)) & 15) != p.index) {
            return false;
        }
        layer = i;
    }
    if (layer == 6) {
        return false;
    }
    u32 dma = 0;
    for (auto c : native_display::kChannels) {
        if (c.index == p.index) {
            dma = c.offset;
        }
    }
    if (!dma) {
        return false;
    }
    const u32 offsets[kWords] = {0,
                                 4,
                                 8,
                                 12,
                                 0x18,
                                 0x1c,
                                 0x60,
                                 0x64,
                                 0x6c,
                                 0x100,
                                 0x60014 + layer * 0x3c,
                                 0x60018 + layer * 0x3c,
                                 0x60004};
    constexpr u32 x = (1080 - AME_GPU_DIAG_WIDTH) / 2;
    constexpr u32 y = (2280 - AME_GPU_DIAG_HEIGHT) / 2;
    const u32 values[kWords] = {0,
                                0,
                                AME_GPU_DIAG_WIDTH * 4 / 16 - 1,
                                AME_GPU_DIAG_HEIGHT - 1,
                                AME_GPU_DIAG_HEIGHT - 1,
                                p.dma.control | (2u << 9),
                                0,
                                AME_GPU_DIAG_WIDTH * 4 / 16,
                                AME_GPU_DIAG_WIDTH * AME_GPU_DIAG_HEIGHT / 4,
                                ((AME_GPU_DIAG_WIDTH - 1) << 16) | (AME_GPU_DIAG_HEIGHT - 1),
                                (y << 16) | x,
                                ((y + AME_GPU_DIAG_HEIGHT - 1) << 16) |
                                    (x + AME_GPU_DIAG_WIDTH - 1),
                                0xff000000};
    for (u32 i = 0; i < kWords; ++i) {
        r.offset[i] = offsets[i] + (i < 10 ? dma : 0);
        r.frame[i] = values[i];
    }
    return true;
}
struct State {
    Registers registers{};
    u32 channel = 0;
    int active = -1, pending = -2; // -1: preserved console, -2: no transition
    u32 phase = 0;
    u64 deadline = 0, flips = 0, underflows = 0;
    bool ready = false, faulted = false;
    template <class Io> bool prepare(Io& io, const native_display::Pipeline& p) {
        if (ready) {
            return true;
        }
        if (!layout(p, registers) || (io.read(kMutex) & 1)) {
            return false;
        }
        for (u32 i = 0; i < kWords; ++i) {
            registers.console[i] = io.read(registers.offset[i]);
        }
        channel = p.index;
        ready = true;
        return true;
    }
    template <class Io> void program(Io& io, int slot, u64 address) {
        io.write(kMutex, 1);
        for (u32 i = 0; i < kWords; ++i) {
            u32 value = slot < 0 ? registers.console[i] : registers.frame[i];
            // Vendor FLIP_V starts at the final source row; hardware steps back.
            if (slot >= 0 && i == 6) {
                value =
                    static_cast<u32>(address + (AME_GPU_DIAG_HEIGHT - 1) * AME_GPU_DIAG_WIDTH * 4);
            }
            io.write(registers.offset[i], value);
        }
        io.write(0x10100 + channel * 4, 1); // selected read-channel flush
        io.write(0x10128, 0xf);             // vendor first-block overlay 0 flush
        io.barrier();
        io.write(kMutex, 0);
        io.barrier();
    }
    // Nonblocking: 1 pending, 0 completed, -1 failed. No IRQ-disabled vblank wait.
    template <class Io> int step(Io& io, int slot, u64 address, u64 now) {
        if (!ready || slot < -1 || slot > 1 || (faulted && slot >= 0)) {
            return -1;
        }
        if (!phase) {
            if (slot == active && !faulted) {
                return 0;
            }
            pending = slot;
            deadline = now + 100000000;
            phase = 1;
            // Like the vendor compositor, program the shadow configuration
            // under MCTL mutex. Completion requires a NEW sync boundary and
            // active interval after unlock, not a possibly stale START bit.
            program(io, slot, address);
            io.write(kInterrupt, kSync | kStart | kUnderflow);
            io.barrier();
            return 1;
        }
        if (slot != pending) {
            return 1;
        }
        if (now >= deadline) {
            faulted = true;
            phase = 0;
            return -1;
        }
        const u32 events = io.read(kInterrupt);
        if ((events & kUnderflow) && slot >= 0) {
            ++underflows;
            faulted = true;
            phase = 0;
            return -1;
        }
        if (phase == 1 && (events & kSync)) {
            phase = 2;
        }
        if (phase == 2 && io.read(kVstate) == kActive) {
            const u32 expected = slot < 0 ? registers.console[6]
                                          : static_cast<u32>(address + (AME_GPU_DIAG_HEIGHT - 1) *
                                                                           AME_GPU_DIAG_WIDTH * 4);
            if (io.read(registers.offset[6]) != expected) {
                return 1;
            }
            // A fresh active interval after the shadow update retires the old
            // frame. Live timing state avoids interpreting a coalesced/stale
            // START event as current activity, without adding another refresh.
            active = slot;
            pending = -2;
            phase = 0;
            ++flips;
            if (slot < 0) {
                faulted = false;
            }
            return 0;
        }
        return 1;
    }
    bool live() const {
        return active >= 0 || phase || faulted;
    }
};
} // namespace anne::scanout
