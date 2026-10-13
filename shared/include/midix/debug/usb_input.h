#pragma once
// Development key injection wire format, independent of USB transport.
// Header LE: magic u32, sequence u32, type u16, payload bytes u16, CRC32 u32.
// CRC covers the first 12 header bytes followed by payload, using libk IEEE.
// Key payload: keyboard usage u16, modifier mask u8, pressed u8. ACK payload:
// status i32. A valid ACK is emitted only after input events are published.
#include "libk/bytes.h"
#include "libk/endian.h"
#include "libk/hash.h"
namespace midix::debug {
constexpr mx_u32_t kInputMagic = 0x314b4d41; // AMK1
constexpr mx_u16_t kHello = 1, kKey = 2, kRelease = 3, kAck = 0x8000;
constexpr mx_u32_t kMaxPayload = 64, kHeader = 16;
inline mx_u32_t checksum(libk::const_bytes frame) {
    if (frame.size < kHeader) {
        return 0;
    }
    auto state = libk::crc32_ieee_update(libk::kCrc32IeeeInit, libk::slice(frame, 0, 12));
    return ~libk::crc32_ieee_update(state, libk::slice(frame, kHeader, frame.size - kHeader));
}
struct Parser {
    mx_u8_t bytes[kHeader + kMaxPayload];
    mx_u32_t used, target;
    bool complete;
    bool push(mx_u8_t byte) {
        if (complete) {
            return false;
        }
        bytes[used++] = byte;
        // Search for magic without allocating or retaining an unbounded stream.
        while (used >= 4 && libk::load_le<mx_u32_t>(bytes) != kInputMagic) {
            libk::move_n(libk::as_bytes(bytes), libk::as_bytes(bytes + 1, used - 1), used - 1);
            --used;
        }
        if (used == kHeader) {
            const auto length = libk::load_le<mx_u16_t>(bytes + 10);
            if (length > kMaxPayload) {
                reset();
                return false;
            }
            target = kHeader + length;
        }
        if (target && used == target) {
            complete = checksum(libk::as_bytes(bytes, used)) == libk::load_le<mx_u32_t>(bytes + 12);
            if (!complete) {
                reset();
            }
        }
        return complete;
    }
    void reset() {
        used = target = 0;
        complete = false;
    }
};
} // namespace midix::debug
