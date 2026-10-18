#pragma once
// Pre-refactor transport retained as a controlled benchmark reference.
// It uses current production wait/wake helpers, so comparisons isolate batching
// from the repaired lost-wakeup bug. Never select it for normal services.
#include "workload.h"
#include "mochi/ring.h"
#include "libk/cpu.h"
namespace ipc_bench::slotwise {
constexpr u32 kFirst = 1u << 30, kLast = 1u << 31;
template <typename Backend> void send(Backend& backend, const Message& message) {
    require(backend, message.size <= kMaxPayload, "outgoing length");
    u32 offset = 0;
    do {
        const u32 left = message.size - offset;
        const u32 chunk = left < MX_RING_INLINE_PAYLOAD ? left : MX_RING_INLINE_PAYLOAD;
        const u32 type = message.kind | (offset == 0 ? kFirst : 0) |
                         (offset + chunk == message.size ? kLast : 0);
        while (!ring_try_push(backend.outgoing.ring, backend.outgoing.order, type, message.payload + offset,
                              chunk, backend.outgoing.doorbell, &message.sequence)) {
            // The current ring contract wakes receivers, not producers
            // waiting for TX space. Yield under backpressure on one CPU.
            if (backend.spin) libk::cpu::relax(); else sys_yield();
        }
        offset += chunk;
    } while (offset < message.size);
}
template <typename Backend> void receive(Backend& backend, Message& message) {
    u32 bytes = 0, kind = 0;
    u64 sequence = 0;
    bool first = true;
    for (;;) {
        RingSlot slot{};
        if (backend.spin) {
            while (!backend.incoming.try_pop(&slot)) libk::cpu::relax();
        } else {
            require(backend, backend.incoming.wait_pop(&slot), "ring receive/wait");
        }
        require(backend, slot.length <= MX_RING_INLINE_PAYLOAD &&
              slot.length <= kMaxPayload - bytes &&
              ((slot.type & kFirst) != 0) == first, "fragment bounds/first flag");
        if (first) { kind = slot.type & ~(kFirst | kLast); sequence = slot.seq; }
        require(backend, (slot.type & ~(kFirst | kLast)) == kind && slot.seq == sequence &&
              (slot.length || (first && (slot.type & kLast))), "fragment sequence/type/progress");
        require(backend, libk::copy_n(libk::as_bytes(message.payload + bytes, kMaxPayload - bytes),
                          libk::as_bytes(slot.payload), slot.length), "fragment copy");
        bytes += slot.length;
        if (slot.type & kLast) break;
        first = false;
    }
    message.kind = kind; message.sequence = sequence; message.size = bytes;
}
} // namespace ipc_bench::slotwise
