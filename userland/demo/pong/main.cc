// userland/demo/pong/main.cc — demo.pong (IPC receiver + supervision test).
//
// Spec ref: §7.5 demo.ping and demo.pong, §9 Phase 9 (Supervision)
//
// V1 note:
//   The receive loop uses ring_wait_pop(), not sys_yield polling.  This is the
//   path that proves doorbell sleep/wake and reduces IPC-driven context switch
//   churn.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"

constexpr uint32_t MSG_PING  = 1;
constexpr uint32_t MSG_PONG  = 2;
constexpr uint32_t MSG_CRASH = 0xDEAD;
constexpr uint32_t MSG_SLEEP_TEST_START = 0x534C4550; // 'SLEP'
constexpr uint32_t MSG_SLEEP_TEST_READY = 0x534C4551; // 'SLEQ'
constexpr uint32_t MSG_SLEEP_TEST_PING  = 0x534C4552; // 'SLER'
constexpr uint32_t MSG_SLEEP_TEST_PONG  = 0x534C4553; // 'SLES'
constexpr uint32_t MSG_CPU_QUERY = 0x43505551; // 'CPUQ'
constexpr uint32_t MSG_CPU_REPLY = 0x43505552; // 'CPUR'
constexpr uint32_t MSG_HOT_START = 0x484F5453; // 'HOTS'
constexpr uint32_t MSG_HOT_READY = 0x484F5452; // 'HOTR'

constexpr uint32_t kOrder = 6;
constexpr int64_t kCapTypeSharedRing = 4;
constexpr int64_t kCapTypeDoorbell = 5;

constexpr CapHandle cap_slot(uint32_t idx) {
    return (static_cast<CapHandle>(1) << 32) | idx;
}

constexpr CapHandle kRecvRing     = cap_slot(0);
constexpr CapHandle kSendRing     = cap_slot(1);
constexpr CapHandle kMyDoorbell   = cap_slot(2);
constexpr CapHandle kPeerDoorbell = cap_slot(3);
constexpr CapHandle kLogRing      = cap_slot(4);
constexpr CapHandle kLogDoorbell  = cap_slot(5);
constexpr uint32_t RIGHT_READ  = 1u << 0;
constexpr uint32_t RIGHT_WRITE = 1u << 1;
constexpr uint64_t kVaRecvRing = 0x2000'0000ull;
constexpr uint64_t kVaSendRing = 0x2001'0000ull;
constexpr uint64_t kVaLogRing = 0x2002'0000ull;

template <typename T>
static T* wait_map(CapHandle cap, int64_t expected_type, uint64_t va, uint32_t rights) {
    while (true) {
        if (sys_cap_identify(cap) == expected_type) {
            if (sys_map_page(cap, va, rights) == 0) return reinterpret_cast<T*>(va);
        }
        sys_yield();
    }
}

static void wait_cap(CapHandle cap, int64_t expected_type) {
    while (sys_cap_identify(cap) != expected_type) {
        sys_yield();
    }
}

extern "C" [[noreturn]] void _start() {
    auto* recv_ring = wait_map<RingPage>(kRecvRing, kCapTypeSharedRing,
                                         kVaRecvRing, RIGHT_READ | RIGHT_WRITE);
    auto* send_ring = wait_map<RingPage>(kSendRing, kCapTypeSharedRing,
                                         kVaSendRing, RIGHT_READ | RIGHT_WRITE);
    wait_cap(kMyDoorbell, kCapTypeDoorbell);
    wait_cap(kPeerDoorbell, kCapTypeDoorbell);
    auto* log_ring = wait_map<RingPage>(kLogRing, kCapTypeSharedRing,
                                        kVaLogRing, RIGHT_READ | RIGHT_WRITE);
    wait_cap(kLogDoorbell, kCapTypeDoorbell);
    mochi::log_set_sink(log_ring, kOrder, kLogDoorbell, "demo.pong");

    RingSlot slot{};
    while (true) {
        if (!ring_wait_pop(recv_ring, kOrder, &slot, kMyDoorbell)) {
            // Invalid doorbell/cap should not happen in the wired V1 demo.
            sys_yield();
            continue;
        }

        if (slot.type == MSG_CRASH) {
            asm volatile("brk #0");
            sys_exit(1);
        }

        if (slot.type == MSG_CPU_QUERY) {
            uint32_t cpu = static_cast<uint32_t>(sys_thread_get_cpu());
            while (!ring_push(send_ring, kOrder, MSG_CPU_REPLY,
                              &cpu, sizeof(cpu), kPeerDoorbell)) {
                sys_yield();
            }
            continue;
        }

        if (slot.type == MSG_HOT_START) {
            uint32_t target = 4096;
            if (slot.length >= sizeof(uint32_t)) {
                target = 0;
                for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
                    reinterpret_cast<uint8_t*>(&target)[i] = slot.payload[i];
                }
            }
            uint32_t ready = 1;
            while (!ring_push(send_ring, kOrder, MSG_HOT_READY,
                              &ready, sizeof(ready), kPeerDoorbell)) {
                sys_yield();
            }

            uint32_t received = 0;
            while (received < target) {
                if (!ring_pop(recv_ring, kOrder, &slot)) continue;
                if (slot.type != MSG_PING) continue;
                ++received;
                uint64_t seq = slot.seq;
                while (!ring_push(send_ring, kOrder, MSG_PONG,
                                  &seq, sizeof(seq), kPeerDoorbell)) {
                    // The peer runs on another CPU and drains concurrently.
                }
            }
            continue;
        }

        if (slot.type == MSG_SLEEP_TEST_START) {
            uint32_t ready = 1;
            while (!ring_push(send_ring, kOrder, MSG_SLEEP_TEST_READY,
                              &ready, sizeof(ready), kPeerDoorbell)) {
                sys_yield();
            }
            continue;
        }

        if (slot.type == MSG_SLEEP_TEST_PING) {
            uint32_t pong = 1;
            while (!ring_push(send_ring, kOrder, MSG_SLEEP_TEST_PONG,
                              &pong, sizeof(pong), kPeerDoorbell)) {
                sys_yield();
            }
            continue;
        }

        uint64_t seq = slot.seq;
        while (!ring_push(send_ring, kOrder, MSG_PONG, &seq, sizeof(seq), kPeerDoorbell)) {
            sys_yield();
        }
    }
}
