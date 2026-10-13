#pragma once
// DesignWare I2C PIO executor. Caller owns request/result rows. Chip register
// meanings and allowed device addresses are deliberately outside this module.
#include "libk/mmio.h"
#include "libk/dma.h"
#include "midix/abi/protocols/i2c.h"
namespace mochi::i2c {
enum class Phase : mx_u8_t { Idle, Disabling, Running, Stopping, Done, Fault };
struct Executor {
    libk::Mmio mmio;
    Phase phase;
    MxI2cTransaction request;
    MxI2cReply result;
    mx_u64_t deadline;
    mx_u32_t queued, received;
    mx_u32_t abort_source;
    bool stop;
    mx_u32_t read(mx_u32_t reg) {
        mx_u32_t v = 0;
        if (!mmio.read32(reg, &v)) {
            phase = Phase::Fault;
        }
        return v;
    }
    void write(mx_u32_t reg, mx_u32_t v) {
        if (!mmio.write32(reg, v)) {
            phase = Phase::Fault;
        }
    }
    bool start(const MxI2cTransaction& next, mx_u64_t now) {
        if (phase != Phase::Idle || next.address > 127 || next.write_bytes > 16 ||
            next.read_bytes > 16 || (!next.write_bytes && !next.read_bytes) || next.reserved) {
            return false;
        }
        request = next;
        result = {next.id, MX_OK, 0, {}};
        queued = received = 0;
        abort_source = 0;
        stop = false;
        deadline = now + 80000000ull;
        write(0x6c, 0);
        phase = Phase::Disabling;
        return true;
    }
    void advance(mx_u64_t now) {
        if (phase == Phase::Idle || phase == Phase::Done || phase == Phase::Fault) {
            return;
        }
        if (now >= deadline) {
            result.status = MX_ERR_FAULT;
            write(0x6c, 0);
            phase = Phase::Stopping;
        }
        if (phase == Phase::Disabling) {
            if (read(0x9c) & 1) {
                return;
            }
            if (read(0xfc) != 0x44570140) {
                phase = Phase::Fault;
                return;
            }
            write(0x30, 0);
            (void)read(0x40);
            write(0, 1 | (1 << 1) | (1 << 5) | (1 << 6));
            write(4, request.address);
            // Signed board clock is 120 MHz. Conservative 100 kHz timing;
            // pads are muxed once by MX_PLATFORM_I2C_PINS before first use.
            write(0x14, 600);
            write(0x18, 650);
            write(0x38, 0);
            write(0x3c, 0);
            write(0x6c, 1);
            libk::dma::order_mmio();
            phase = Phase::Running;
        }
        if (phase == Phase::Running) {
            const auto interrupts = read(0x34);
            if (interrupts & (1 << 6)) {
                abort_source = read(0x80);
                (void)read(0x54);
                result.status = MX_ERR_FAULT;
                write(0x6c, 0);
                phase = Phase::Stopping;
            } else {
                const mx_u32_t total = request.write_bytes + request.read_bytes;
                for (mx_u32_t n = 0; n < 32 && queued < total && (read(0x70) & 2); ++n) {
                    const bool input = queued >= request.write_bytes;
                    mx_u32_t cmd = input ? (1u << 8) : request.data[queued];
                    if (input && queued == request.write_bytes && request.write_bytes) {
                        cmd |= 1u << 10;
                    }
                    if (queued + 1 == total) {
                        cmd |= 1u << 9;
                    }
                    write(0x10, cmd);
                    ++queued;
                }
                for (mx_u32_t n = 0; n < 16 && (read(0x70) & 8); ++n) {
                    const auto value = read(0x10);
                    if (received < request.read_bytes) {
                        result.data[received++] = static_cast<mx_u8_t>(value);
                    } else {
                        result.status = MX_ERR_OVERFLOW;
                    }
                }
                if (read(0x34) & (1 << 9)) {
                    (void)read(0x60);
                    stop = true;
                }
                if (stop && queued == total && received == request.read_bytes) {
                    result.bytes = received;
                    write(0x6c, 0);
                    phase = Phase::Stopping;
                }
            }
        }
        if (phase == Phase::Stopping && !(read(0x9c) & 1)) {
            phase = Phase::Done;
        }
        // A controller that refuses disable is quarantined, never reused.
        if (phase == Phase::Stopping && now >= deadline + 20000000ull) {
            phase = Phase::Fault;
        }
    }
    void release() {
        if (phase == Phase::Done) {
            phase = Phase::Idle;
        }
    }
};
} // namespace mochi::i2c
