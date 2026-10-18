// Minimal SDHCI eMMC transport. Register/PHY definitions checked against Huawei
// hi6250 Linux 65e545cbaa11d132e243287b150146bc2a51c45e (sdhci.h,
// sdhci-of-arasan.c, mmc.h). Polling PIO/ADMA2; no tuning or IRQ ownership.
#include "midix/drivers/emmc.h"

namespace drivers::emmc {
namespace {
constexpr u32 kPresent = 0x24, kInterrupt = 0x30;
constexpr u32 kErrors = 0xffff8000, kCardErrors = 0xffff8000 | (1u << 7);
constexpr u16 kR1 = 0x1a, kR1b = 0x1b, kR2 = 0x09, kR3 = 0x02;
constexpr u64 kCommandUs = 100000, kDataUs = 2000000;
u16 be16(const u8 *p) { return (u16(p[0]) << 8) | p[1]; }
void put16(u8 *p, u16 v) {
    p[0] = u8(v);
    p[1] = u8(v >> 8);
}
void put32(u8 *p, u32 v) {
    for (unsigned n = 0; n < 4; ++n)
        p[n] = u8(v >> (8 * n));
}
void publish_dma() {
#if defined(__aarch64__)
    asm volatile("dmb oshst" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}
void consume_dma() {
#if defined(__aarch64__)
    asm volatile("dmb oshld" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}
} // namespace
u32 Driver::rd(u32 o, unsigned w) const { return io_.read(io_.context, o, w); }
void Driver::wr(u32 o, u32 v, unsigned w) { io_.write(io_.context, o, w, v); }
Error Driver::fail(Error e, u32 irq, u32 status) {
    failure_.error = e;
    failure_.interrupt = irq;
    failure_.card_status = status;
    return e;
}
void Driver::delay(u64 us) {
    const u64 start = io_.now_us(io_.context);
    while (io_.now_us(io_.context) - start < us) {
    }
}
Error Driver::wait(u32 o, unsigned w, u32 mask, u32 value, u64 timeout) {
    const u64 start = io_.now_us(io_.context);
    do {
        if ((rd(o, w) & mask) == value)
            return Error::Ok;
    } while (io_.now_us(io_.context) - start < timeout);
    return fail(Error::Timeout);
}
Error Driver::interrupt(u32 mask, u64 timeout) {
    const u64 start = io_.now_us(io_.context);
    do {
        const u32 status = rd(kInterrupt);
        // Errors win even when completion/ready bits are also set.
        if (status & kErrors)
            return fail((status & 0x00110000) ? Error::Timeout : Error::Controller, status);
        if (status & mask) {
            wr(kInterrupt, mask);
            return Error::Ok;
        }
    } while (io_.now_us(io_.context) - start < timeout);
    return fail(Error::Timeout, rd(kInterrupt));
}
Error Driver::reset(u8 mask) {
    wr(0x2f, mask, 1);
    return wait(0x2f, 1, mask, 0, kCommandUs);
}
Error Driver::clock(u32 input, u32 target) {
    if (!input || !target)
        return fail(Error::Invalid);
    // SDHCI v3 divided clock: divisor is 1, or an even number through 2046.
    u32 divisor = 1;
    if (input > target) {
        divisor = u32((u64(input) + target - 1) / target);
        divisor = (divisor + 1) & ~1u;
    }
    if (divisor > 2046)
        return fail(Error::Unsupported);
    const u32 encoded = divisor == 1 ? 0 : divisor / 2;
    const u32 value = ((encoded & 0xff) << 8) | ((encoded & 0x300) >> 2) | 1;
    wr(0x2c, 0, 2);
    wr(0x2c, value, 2);
    auto e = wait(0x2c, 2, 2, 2, kCommandUs);
    if (e == Error::Ok)
        wr(0x2c, value | 4, 2);
    return e;
}
Error Driver::command(u32 index, u32 arg, u16 flags, u32 *response) {
    failure_.command = index;
    const bool data = flags & 0x20;
    const bool busy = (flags & 3) == 3;
    const u32 inhibit = 1 | ((data || busy) && index != 12 ? 2 : 0);
    auto e = wait(kPresent, 4, inhibit, 0, kCommandUs);
    if (e != Error::Ok)
        return e;
    wr(kInterrupt, 0xffffffff);
    wr(0x08, arg);
    wr(0x0e, (index << 8) | flags, 2);
    e = interrupt(1, kCommandUs);
    if (e != Error::Ok)
        return e;
    const u32 status = rd(0x10);
    if (response)
        *response = status;
    if ((flags & 3) >= 2 && (flags & 0x10) && (status & kCardErrors))
        return fail(Error::Card, 0, status);
    if (busy) {
        e = interrupt(2, kDataUs);
        if (e == Error::Ok)
            e = wait(kPresent, 4, 2, 0, kDataUs);
    }
    return e;
}
Error Driver::ready_status() {
    const u64 start = io_.now_us(io_.context);
    do {
        u32 status = 0;
        auto e = command(13, 1u << 16, kR1, &status);
        if (e != Error::Ok)
            return e;
        if ((status & 0x1f00) == 0x900)
            return Error::Ok; // READY + TRAN
    } while (io_.now_us(io_.context) - start < kDataUs);
    return fail(Error::Timeout);
}
Error Driver::select(u8 config) {
    auto e = command(6, (3u << 24) | (179u << 16) | (u32(config) << 8), kR1b);
    if (e == Error::Ok)
        e = ready_status();
    if (e == Error::Ok)
        partition_ = config;
    return e;
}
void Driver::recover() {
    // A timed-out CMD25 may already have written data. Abort once; never retry.
    const Failure original = failure_;
    bool ok = reset(6) == Error::Ok;
    if (ok)
        ok = command(12, 0, kR1b) == Error::Ok;
    if (ok)
        ok = ready_status() == Error::Ok;
    if (!ok)
        ready_ = false;
    failure_ = original;
}
Error Driver::transfer(u32 index, u32 arg, u32 count, u8 *buffer, bool write) {
    wr(0x04, 512, 2);
    wr(0x06, count, 2);
    // CMD23 supplies the card's count; AUTO_CMD12/23 and DMA stay disabled.
    wr(0x0c, 2 | (write ? 0 : 0x10) | (index == 18 || index == 25 ? 0x20 : 0), 2);
    auto e = command(index, arg, kR1 | 0x20);
    const u32 ready = write ? 0x10 : 0x20;
    for (u32 b = 0; e == Error::Ok && b < count; ++b) {
        e = interrupt(ready, kDataUs);
        if (e != Error::Ok)
            break;
        for (u32 i = 0; i < 512; i += 4) {
            u8 *p = buffer + b * 512 + i;
            if (write)
                wr(0x20, u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24));
            else {
                const u32 word = rd(0x20);
                for (unsigned j = 0; j < 4; ++j)
                    p[j] = u8(word >> (j * 8));
            }
        }
    }
    if (e == Error::Ok)
        e = interrupt(2, kDataUs);
    if (e == Error::Ok)
        e = ready_status();
    if (e != Error::Ok)
        recover();
    return e;
}
Error Driver::enable_dma(DmaArena arena) {
    if (!ready_)
        return fail(Error::NotReady);
    if (!arena.cpu || (reinterpret_cast<usize>(arena.cpu) & 4095) || !arena.physical ||
        (arena.physical & 4095) || arena.size < kDmaArenaSize || arena.physical >= (1ull << 32) ||
        arena.size > (1ull << 32) - arena.physical)
        return fail(Error::Invalid);
    if (!(rd(0x40) & (1u << 19)))
        return fail(Error::Unsupported);
    dma_ = arena;
    return Error::Ok;
}
Error Driver::transfer_dma(u32 index, u32 arg, u32 count, u8 *buffer, bool write) {
    const u32 bytes = count * kSectorSize;
    auto *payload = dma_.cpu + kDmaDataOffset;
    if (write)
        for (u32 n = 0; n < bytes; ++n)
            payload[n] = buffer[n];
    // ADMA2 32-bit descriptors: attributes, nonzero length, bus address. Split
    // at 32 KiB to avoid the ambiguous zero-length encoding for 64 KiB.
    for (u32 offset = 0, n = 0; offset < bytes; ++n) {
        const u32 length = bytes - offset > 32768 ? 32768 : bytes - offset;
        auto *descriptor = dma_.cpu + n * 8;
        put16(descriptor, u16(0x21 | (offset + length == bytes ? 2 : 0)));
        put16(descriptor + 2, u16(length));
        put32(descriptor + 4, u32(dma_.physical + kDmaDataOffset + offset));
        offset += length;
    }
    publish_dma();
    // Explicit legacy 32-bit ADMA2 selection; preserve the verified bus width.
    wr(0x28, (rd(0x28, 1) & ~0x18u) | 0x10u, 1);
    wr(0x58, u32(dma_.physical));
    wr(0x04, 512, 2);
    wr(0x06, count, 2);
    wr(0x0c, 1 | 2 | (write ? 0 : 0x10) | 0x20, 2);
    auto e = command(index, arg, kR1 | 0x20);
    if (e == Error::Ok)
        e = interrupt(2, kDataUs);
    if (e == Error::Ok)
        e = wait(kPresent, 4, 2, 0, kDataUs);
    if (e == Error::Ok)
        e = ready_status();
    if (e != Error::Ok) {
        recover();
        return e;
    }
    consume_dma();
    if (!write)
        for (u32 n = 0; n < bytes; ++n)
            buffer[n] = payload[n];
    return Error::Ok;
}
Error Driver::initialize(u32 input, bool arasan, u32 bus_width) {
    ready_ = false;
    info_ = {};
    failure_ = {};
    dma_ = {};
    if (!io_.read || !io_.write || !io_.now_us || (bus_width != 1 && bus_width != 8))
        return fail(Error::Invalid);
    // Require the bootloader's powered host and v3 divisor semantics. Supplies,
    // SoC gates and pinmux are deliberately left to the board/bootloader.
    if (!(rd(0x29, 1) & 1))
        return fail(Error::NotReady);
    if ((rd(0xfe, 2) & 0xff) < 2 || rd(0xfe, 2) == 0xffff)
        return fail(Error::Unsupported);
    auto e = reset(6);
    if (e != Error::Ok)
        return e;
    wr(0x38, 0);
    wr(0x34, 0xffff0033);                // Poll status, no host IRQ signals.
    wr(0x28, 0, 1);                      // 1-bit, legacy timing, PIO.
    wr(0x3e, rd(0x3e, 2) & ~0xbcc7u, 2); // v3 32-bit descriptors; retain voltage bit (3)
    wr(0x2e, 0x0e, 1);
    if (arasan) {
        wr(0x78, 0, 2); // enhanced strobe off
        u32 phy = rd(0x430);
        phy &= ~((0xffu << 8) | (7u << 22) | (1u << 7));
        wr(0x430, phy | (4u << 8) | (1u << 16) | (1u << 25) | 1);
        e = wait(0x440, 4, 0x41, 0x40, kCommandUs);
        if (e != Error::Ok)
            return e;
        wr(0x438, rd(0x438) & ~((1u << 8) | (1u << 14)));
        wr(0x434, 0xffu << 22); // open drain until RCA assignment
        delay(10);
    }
    e = clock(input, 400000);
    if (e != Error::Ok)
        return e;
    delay(1000); // >74 identification clocks
    e = command(0, 0, 0);
    if (e != Error::Ok)
        return e;
    const u64 start = io_.now_us(io_.context);
    u32 ocr = 0;
    do {
        e = command(1, 0x40ff8080, kR3, &ocr);
        if (e != Error::Ok)
            return e;
        if (ocr & 0x80000000)
            break;
        delay(1000);
    } while (io_.now_us(io_.context) - start < kDataUs);
    if (!(ocr & 0x80000000))
        return fail(Error::Timeout);
    if (!(ocr & 0x40000000))
        return fail(Error::Unsupported); // sector addressing
    if ((e = command(2, 0, kR2)) != Error::Ok)
        return e;
    if ((e = command(3, 1u << 16, kR1)) != Error::Ok)
        return e;
    if (arasan) {
        wr(0x430, rd(0x430) & ~1u);
        wr(0x434, 0x003ff7fc);
    }
    if ((e = command(7, 1u << 16, kR1b)) != Error::Ok)
        return e;
    if ((e = transfer(8, 0, 1, info_.ext_csd, false)) != Error::Ok)
        return e;
    // CMD0 resets volatile bus width, HS timing and command queue mode.
    if (info_.ext_csd[183] || info_.ext_csd[185] || info_.ext_csd[15] || info_.ext_csd[61])
        return fail(Error::Unsupported); // native 4K sectors are unsupported
    const u8 *p = info_.ext_csd + 212;
    info_.sectors = u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
    partition_ = info_.ext_csd[179];
    if (!info_.sectors)
        return fail(Error::Unsupported);
    if ((partition_ & 7) && (e = select(partition_ & ~7u)) != Error::Ok)
        return e;
    // Legacy eMMC supports up to 26 MHz; 12.5 MHz from the 200 MHz Anne input.
    if ((e = clock(input, 12500000)) != Error::Ok)
        return e;
    if (bus_width == 8 && (rd(0x40) & (1u << 18))) {
        // Board policy authorizes eight data lines; SDHCI must advertise them.
        // Validate a wider bus by rereading immutable EXT_CSD fields, as in
        // Linux mmc_compare_ext_csds(), rather than trusting a SWITCH ACK.
        e = command(6, (3u << 24) | (183u << 16) | (2u << 8), kR1b);
        if (e == Error::Ok)
            e = ready_status();
        bool verified = false;
        alignas(4) u8 check[512];
        if (e == Error::Ok) {
            wr(0x28, (rd(0x28, 1) & ~2u) | 0x20u, 1);
            e = transfer(8, 0, 1, check, false);
            if (e == Error::Ok) {
                verified = true;
                constexpr u16 fields[]{160, 181, 192, 194, 196, 212, 213, 214, 215,
                                       217, 221, 223, 224, 229, 230, 231, 232};
                for (auto field : fields)
                    if (check[field] != info_.ext_csd[field])
                        verified = false;
            }
        }
        if (verified) {
            info_.bus_width = 8;
            info_.ext_csd[183] = 2;
        } else {
            // CMD uses its independent command line. Restore both ends even
            // after an uncertain SWITCH, then prove one-bit reads still work.
            e = command(6, (3u << 24) | (183u << 16), kR1b);
            if (e == Error::Ok)
                e = ready_status();
            wr(0x28, rd(0x28, 1) & ~0x22u, 1);
            if (e != Error::Ok)
                return e;
            if ((e = transfer(8, 0, 1, check, false)) != Error::Ok)
                return e;
            constexpr u16 fields[]{160, 181, 192, 194, 196, 212, 213, 214, 215};
            for (auto field : fields)
                if (check[field] != info_.ext_csd[field])
                    return fail(Error::Controller);
            failure_ = {};
        }
    }
    ready_ = true;
    return Error::Ok;
}
Error Driver::blocks(u32 lba, u32 count, u8 *buffer, bool write) {
    failure_ = {};
    if (!ready_)
        return fail(Error::NotReady);
    if (!buffer || !count || count > kMaxUserSectors || lba >= info_.sectors || count > info_.sectors - lba)
        return fail(Error::Invalid);
    if (partition_ & 7)
        return fail(Error::NotReady);
    auto e = command(23, count, kR1);
    if (e != Error::Ok) {
        recover();
        return e;
    }
    e = dma_enabled() ? transfer_dma(write ? 25 : 18, lba, count, buffer, write)
                      : transfer(write ? 25 : 18, lba, count, buffer, write);
    return e;
}
Error Driver::read(u32 lba, u32 count, void *buffer) {
    return blocks(lba, count, static_cast<u8 *>(buffer), false);
}
Error Driver::write(u32 lba, u32 count, const void *buffer) {
    const auto result = write_buffered(lba, count, buffer);
    return result == Error::Ok ? flush() : result;
}
Error Driver::write_buffered(u32 lba, u32 count, const void *buffer) {
    return blocks(lba, count, const_cast<u8 *>(static_cast<const u8 *>(buffer)), true);
}
Error Driver::flush() {
    failure_ = {};
    if (!ready_ || (partition_ & 7))
        return fail(Error::NotReady);
    // Transfer completion already waits for READY/TRAN. Drain the card's optional
    // volatile cache only at a caller's durability boundary, once per batch.
    if (!info_.ext_csd[33])
        return Error::Ok;
    auto result = command(6, (3u << 24) | (32u << 16) | (1u << 8), kR1b);
    if (result == Error::Ok)
        result = ready_status();
    if (result != Error::Ok)
        recover();
    return result;
}
Error Driver::rpmb(const Frame *request, u32 count, Frame *response, u32 responses) {
    failure_ = {};
    if (!ready_)
        return fail(Error::NotReady);
    if (!request || !response || !count || count > 32 || !responses || responses > 32)
        return fail(Error::Invalid);
    if (!info_.ext_csd[168])
        return fail(Error::Unsupported);
    const u16 type = be16(request[0].bytes + 510);
    const u16 operation = type & 0xfff;
    if (operation != 1 && operation != 2 && operation != 3 && operation != 4)
        return fail(Error::Unsupported);
    if ((operation != 3 && count != 1) || (operation != 4 && responses != 1))
        return fail(Error::Invalid);
    if (operation == 1 && responses != 1)
        return fail(Error::Invalid);
    if (operation == 3) {
        if (count > 2 && (!(info_.ext_csd[166] & 0x10) || count != 32))
            return fail(Error::Unsupported);
        for (u32 i = 0; i < count; ++i)
            if (be16(request[i].bytes + 510) != type || be16(request[i].bytes + 506) != count)
                return fail(Error::Invalid);
    }
    // Hardware RPMB addresses count 256-byte data blocks, not 512-byte frames.
    if (operation == 3 || operation == 4) {
        const u32 address = be16(request[0].bytes + 504);
        const u32 capacity = u32(info_.ext_csd[168]) * 512;
        const u32 n = operation == 3 ? count : responses;
        if (address >= capacity || n > capacity - address || n > 65536u - address)
            return fail(Error::Invalid);
    }
    const u8 saved = partition_;
    auto e = select((saved & ~7u) | 3);
    auto send = [&](u8 *data, u32 n, bool write, bool reliable) {
        auto result = command(23, n | (reliable ? 0x80000000u : 0), kR1);
        if (result != Error::Ok) {
            recover();
            return result;
        }
        return transfer(write ? 25 : 18, 0, n, data, write);
    };
    if (e == Error::Ok)
        e = send(reinterpret_cast<u8 *>(const_cast<Frame *>(request)), count, true,
                 operation == 1 || operation == 3);
    if (e == Error::Ok && (operation == 1 || operation == 3)) {
        Frame status{};
        const u16 status_type = (type & 0xf000) | 5;
        status.bytes[510] = u8(status_type >> 8);
        status.bytes[511] = u8(status_type);
        e = send(status.bytes, 1, true, false);
    }
    if (e == Error::Ok)
        e = send(reinterpret_cast<u8 *>(response), responses, false, false);
    const Failure original = failure_;
    // Restore even after a failed SWITCH: it may have reached the card.
    if (select(saved) != Error::Ok) {
        ready_ = false;
        return fail(Error::RestoreFailed);
    }
    failure_ = original;
    if (e != Error::Ok)
        return e;
    // Huawei's MMC transport returns raw response frames even for malformed
    // response types/results. The secure caller validates these and the MAC;
    // transport success must not discard the device's evidence.
    return Error::Ok;
}
} // namespace drivers::emmc
