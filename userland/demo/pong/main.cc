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
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/handle.h"
#include "midix/abi/protocols/demo.h"
#include "mochi/va_layout.h"

using namespace mochi::va::demo_pong;

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();
    const auto ipc = mochi::boot::map_endpoint(boot,
        {"ipc.tx", kVaSendRing, "ipc.peer.db"},
        {"ipc.rx", kVaRecvRing, "ipc.self.db"});
    if (!ipc.valid()) mochi::boot::invalid_boot();
    auto* send_ring = ipc.tx_ring;
    auto* recv_ring = ipc.rx_ring;
    const auto peer_doorbell = ipc.tx_doorbell;
    const auto my_doorbell = ipc.rx_doorbell;
    const uint32_t ipc_order = ipc.order;
    if (!mochi::log_map_from_boot(boot, kVaLogRing, "demo.pong")) {
        mochi::boot::invalid_boot();
    }
    RingSlot slot{};
    mochi::boot::ready(boot);
    while (true) {
        if (!ring_wait_pop(recv_ring, ipc_order, &slot, my_doorbell)) {
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
            while (!ring_push(send_ring, ipc_order, MSG_CPU_REPLY,
                              &cpu, sizeof(cpu), peer_doorbell)) {
                sys_yield();
            }
            continue;
        }

        if (slot.type == MSG_HOT_START) {
            uint32_t target = 4096;
            (void)mochi::message_get(slot, &target, MSG_HOT_START);
            uint32_t ready = 1;
            while (!ring_push(send_ring, ipc_order, MSG_HOT_READY,
                              &ready, sizeof(ready), peer_doorbell)) {
                sys_yield();
            }

            uint32_t received = 0;
            while (received < target) {
                if (!ring_pop(recv_ring, ipc_order, &slot)) continue;
                if (slot.type != MSG_PING) continue;
                ++received;
                uint64_t seq = slot.seq;
                while (!ring_push(send_ring, ipc_order, MSG_PONG,
                                  &seq, sizeof(seq), peer_doorbell)) {
                    // The peer runs on another CPU and drains concurrently.
                }
            }
            continue;
        }

        if (slot.type == MSG_SLEEP_TEST_START) {
            uint32_t ready = 1;
            while (!ring_push(send_ring, ipc_order, MSG_SLEEP_TEST_READY,
                              &ready, sizeof(ready), peer_doorbell)) {
                sys_yield();
            }
            continue;
        }

        if (slot.type == MSG_SLEEP_TEST_PING) {
            uint32_t pong = 1;
            while (!ring_push(send_ring, ipc_order, MSG_SLEEP_TEST_PONG,
                              &pong, sizeof(pong), peer_doorbell)) {
                sys_yield();
            }
            continue;
        }

        uint64_t seq = slot.seq;
        while (!ring_push(send_ring, ipc_order, MSG_PONG, &seq, sizeof(seq), peer_doorbell)) {
            sys_yield();
        }
    }
}
