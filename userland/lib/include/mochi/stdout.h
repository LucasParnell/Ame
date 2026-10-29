#pragma once
// App stdout: ordered byte runs from a child process to the parent that spawned
// it, over the IPC log ring the parent grants in the child's BootInfo.
//
// The parent owns the ring and drains it; the app only publishes. Two delivery
// policies exist because they answer different questions:
//   write()     - guaranteed: spins until the parent makes room. Use for output
//                 the program must not lose (results, diagnostics).
//   try_write() - non-blocking: publishes what fits and reports the count, so a
//                 real-time path never stalls on a slow reader.
// With no ring (running under a different launcher) both fall back to the
// kernel debug channel, so call sites never need to branch.
#include "mochi/boot.h"
#include "mochi/ring.h"
#include "mochi/syscall.h"

namespace mochi::appio {

// Log slot type the parent's drain loop expects (see dev.apps drain_logs).
constexpr uint32_t kLogSlotType = 1;

struct Sink {
    Ring ring{};
    bool attached = false;
};

inline Sink& sink() {
    static Sink instance;
    return instance;
}

inline bool attached() {
    return sink().attached;
}

// Map the parent-granted "log.ring"/"log.doorbell" pair at `va`.
inline bool attach(const MxBootInfo* boot, mx_vaddr_t va) {
    if (!boot)
        return false;
    const Ring ring = boot::map_ring(boot, {"log.ring", va, "log.doorbell"});
    if (!ring.valid())
        return false;
    sink().ring = ring;
    sink().attached = true;
    return true;
}

inline void detach() {
    sink().ring = {};
    sink().attached = false;
}

// Guaranteed run: returns bytes accepted (0 only when nothing is attached).
inline uint32_t write(const void* data, uint32_t len) {
    if (!data || !len)
        return 0;
    if (!sink().attached) {
        sys_debug_puts(static_cast<const char*>(data), len);
        return len;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t sent = 0;
    while (sent < len)
        sent += sink().ring.push_bytes(kLogSlotType, bytes + sent, len - sent, 1024);
    return sent;
}

// Non-blocking run: publishes into whatever room exists right now.
inline uint32_t try_write(const void* data, uint32_t len) {
    if (!data || !len)
        return 0;
    if (!sink().attached) {
        sys_debug_puts(static_cast<const char*>(data), len);
        return len;
    }
    return sink().ring.push_bytes(kLogSlotType, data, len, 0);
}

inline void puts(const char* text) {
    if (!text)
        return;
    uint32_t n = 0;
    while (text[n])
        ++n;
    write(text, n);
}

inline void putc(char c) {
    write(&c, 1);
}

inline void put_dec(uint64_t value) {
    char digits[20];
    uint32_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    char out[20];
    for (uint32_t i = 0; i < n; ++i)
        out[i] = digits[n - 1 - i];
    write(out, n);
}

inline void put_hex(uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    char out[18] = {'0', 'x'};
    for (uint32_t i = 0; i < 16; ++i)
        out[2 + i] = kDigits[(value >> ((15 - i) * 4)) & 0xf];
    write(out, sizeof(out));
}

} // namespace mochi::appio
