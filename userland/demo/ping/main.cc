// userland/demo/ping/main.cc — demo.ping (IPC sender).
//
// Spec ref: §7.5 demo.ping and demo.pong, §10 Performance Targets
//
// V1 context-switch reduction shape:
//   - Hot-path ping/pong uses ring_push + non-blocking ring_pop, no sys_yield
//     in the IPC receive path.
//   - Doorbell test forces pong to sleep, then sends a message that wakes it.
//   - Nameservice and one-shot reply waits use ring_wait_pop().

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/nameservice.h"
#include "midix/abi/protocols/nameservice.h"
#include "mochi/va_layout.h"

constexpr uint32_t kRingOrder    = 6;
constexpr uint32_t kMessageCount = 4096;
constexpr bool kEmitBootMetrics = false;

constexpr uint32_t MSG_PING  = 1;
constexpr uint32_t MSG_PONG  = 2;
constexpr uint32_t MSG_SLEEP_TEST_START = 0x534C4550; // 'SLEP'
constexpr uint32_t MSG_SLEEP_TEST_READY = 0x534C4551; // 'SLEQ'
constexpr uint32_t MSG_SLEEP_TEST_PING  = 0x534C4552; // 'SLER'
constexpr uint32_t MSG_SLEEP_TEST_PONG  = 0x534C4553; // 'SLES'
constexpr uint32_t MSG_CPU_QUERY = 0x43505551; // 'CPUQ'
constexpr uint32_t MSG_CPU_REPLY = 0x43505552; // 'CPUR'
constexpr uint32_t MSG_HOT_START = 0x484F5453; // 'HOTS'
constexpr uint32_t MSG_HOT_READY = 0x484F5452; // 'HOTR'

using namespace mochi::va::demo_ping;

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

static void try_console_log(const char* msg) {
    mochi::log_raw_marker(msg);
}

static void try_console_log_counter(const char* key, uint64_t value) {
    if constexpr (!kEmitBootMetrics) {
        (void)key;
        (void)value;
        return;
    }
    mochi::log_counter(key, value);
}

static void resolve_pong(RingPage* request_ring, RingPage* reply_ring,
                         CapHandle ns_doorbell, CapHandle ns_reply_doorbell) {
    MxNsResolveRequest req{};
    mochi::nameservice::copy_name(req.name, sizeof(req.name), "demo.pong");
    while (!ring_push(request_ring, kRingOrder, MSG_NS_RESOLVE,
                      &req, sizeof(req), ns_doorbell)) {
        sys_yield();
    }

    RingSlot slot{};
    while (true) {
        if (!ring_wait_pop(reply_ring, kRingOrder, &slot, ns_reply_doorbell)) {
            sys_yield();
            continue;
        }
        if (slot.type == MSG_NS_ERROR) {
            try_console_log("nameservice: resolve failed\n");
            while (true) sys_yield();
        }
        if (slot.type != MSG_NS_RESOLVE_REPLY) continue;

        MxEndpointDesc endpoint{};
        if (slot.length >= sizeof(MxEndpointDesc)) {
            for (uint32_t i = 0; i < sizeof(MxEndpointDesc); ++i) {
                reinterpret_cast<uint8_t*>(&endpoint)[i] = slot.payload[i];
            }
            if (endpoint.magic == MX_ENDPOINT_MAGIC && endpoint.handle_count >= 1 &&
                sys_cap_identify(endpoint.handles[0]) == kCapTypeProcess) {
                return;
            }
        }
    }
}

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();
    const CapHandle send_cap = mochi::boot::require_handle(boot, "ipc.tx");
    const CapHandle recv_cap = mochi::boot::require_handle(boot, "ipc.rx");
    const CapHandle pong_doorbell = mochi::boot::require_handle(boot, "ipc.peer.db");
    const CapHandle ping_doorbell = mochi::boot::require_handle(boot, "ipc.self.db");
    const CapHandle log_cap = mochi::boot::require_handle(boot, "log.ring");
    const CapHandle log_doorbell = mochi::boot::require_handle(boot, "log.doorbell");
    const CapHandle done_doorbell = mochi::boot::require_handle(boot, "ipc.done");
    const CapHandle ns_req_cap = mochi::boot::require_handle(boot, "ns.tx");
    const CapHandle ns_reply_cap = mochi::boot::require_handle(boot, "ns.rx");
    const CapHandle ns_doorbell = mochi::boot::require_handle(boot, "ns.server.db");
    const CapHandle ns_reply_doorbell = mochi::boot::require_handle(boot, "ns.reply.db");

    auto* send_ring = wait_map<RingPage>(send_cap, kCapTypeSharedRing,
                                         kVaSendRing, RIGHT_READ | RIGHT_WRITE);
    auto* recv_ring = wait_map<RingPage>(recv_cap, kCapTypeSharedRing,
                                         kVaRecvRing, RIGHT_READ | RIGHT_WRITE);
    wait_cap(pong_doorbell, kCapTypeDoorbell);
    wait_cap(ping_doorbell, kCapTypeDoorbell);
    auto* log_ring = wait_map<RingPage>(log_cap, kCapTypeSharedRing,
                                        kVaConsoleRing, RIGHT_READ | RIGHT_WRITE);
    wait_cap(log_doorbell, kCapTypeDoorbell);
    mochi::log_set_sink(log_ring, kRingOrder, log_doorbell, "demo.ping");
    mochi::boot::ready(boot);

    auto* ns_request_ring = wait_map<RingPage>(ns_req_cap, kCapTypeSharedRing,
                                               kVaNsRequestRing, RIGHT_READ | RIGHT_WRITE);
    auto* ns_reply_ring = wait_map<RingPage>(ns_reply_cap, kCapTypeSharedRing,
                                             kVaNsReplyRing, RIGHT_READ | RIGHT_WRITE);
    wait_cap(ns_doorbell, kCapTypeDoorbell);
    wait_cap(ns_reply_doorbell, kCapTypeDoorbell);
    resolve_pong(ns_request_ring, ns_reply_ring, ns_doorbell, ns_reply_doorbell);

    RingSlot slot{};
    uint32_t total_sent = 0;
    uint32_t total_recv = 0;

    const uint32_t my_cpu = static_cast<uint32_t>(sys_thread_get_cpu());
    uint32_t cpu_query = 0;
    while (!ring_push(send_ring, kRingOrder, MSG_CPU_QUERY,
                      &cpu_query, sizeof(cpu_query), pong_doorbell)) {
        sys_yield();
    }
    ++total_sent;
    uint32_t pong_cpu = 0xFFFFu;
    while (pong_cpu == 0xFFFFu) {
        if (!ring_wait_pop(recv_ring, kRingOrder, &slot, ping_doorbell)) {
            sys_yield();
            continue;
        }
        if (slot.type == MSG_CPU_REPLY && slot.length >= sizeof(uint32_t)) {
            for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
                reinterpret_cast<uint8_t*>(&pong_cpu)[i] = slot.payload[i];
            }
            ++total_recv;
        }
    }
    if (my_cpu == 0 && pong_cpu == 1) {
        try_console_log("affinity ready\n");
    }

    uint32_t hot_count = kMessageCount;
    while (!ring_push(send_ring, kRingOrder, MSG_HOT_START,
                      &hot_count, sizeof(hot_count), pong_doorbell)) {
        sys_yield();
    }
    ++total_sent;
    bool hot_ready = false;
    while (!hot_ready) {
        if (!ring_wait_pop(recv_ring, kRingOrder, &slot, ping_doorbell)) {
            sys_yield();
            continue;
        }
        if (slot.type == MSG_HOT_READY) {
            hot_ready = true;
            ++total_recv;
        }
    }

    const uint64_t ctx_before_hot = static_cast<uint64_t>(sys_debug_counter(2));
    const uint64_t ipc_before_hot = static_cast<uint64_t>(sys_debug_counter(1));
    const uint64_t notify_before_hot = static_cast<uint64_t>(sys_debug_counter(10));

    uint32_t sent = 0;
    uint32_t recv = 0;
    // Test A: cross-core hot ring path.  Do not use sys_yield as IPC progress.
    while (recv < kMessageCount) {
        while (sent < kMessageCount &&
               ring_push(send_ring, kRingOrder, MSG_PING, &sent, sizeof(sent), pong_doorbell)) {
            ++sent;
            ++total_sent;
        }

        while (ring_pop(recv_ring, kRingOrder, &slot)) {
            if (slot.type == MSG_PONG) {
                ++recv;
                ++total_recv;
            }
        }
    }
    const uint64_t ctx_after_hot = static_cast<uint64_t>(sys_debug_counter(2));
    const uint64_t ipc_after_hot = static_cast<uint64_t>(sys_debug_counter(1));
    const uint64_t notify_after_hot = static_cast<uint64_t>(sys_debug_counter(10));

    if (sent != recv) {
        try_console_log("ipc: hot ring failed\n");
        while (true) sys_yield();
    }
    try_console_log("ipc: hot ring ready\n");
    try_console_log("ipc: cross-core hot ring ready\n");

    // Test B: force doorbell sleep/wake.
    uint32_t sleep_magic = 0x12345678;
    while (!ring_push(send_ring, kRingOrder, MSG_SLEEP_TEST_START,
                      &sleep_magic, sizeof(sleep_magic), pong_doorbell)) {
        sys_yield();
    }
    ++total_sent;

    bool got_ready = false;
    while (!got_ready) {
        if (!ring_wait_pop(recv_ring, kRingOrder, &slot, ping_doorbell)) {
            sys_yield();
            continue;
        }
        if (slot.type == MSG_SLEEP_TEST_READY) {
            got_ready = true;
            ++total_recv;
        }
    }

    // Pong sends READY, then returns to ring_wait_pop() on send_ring.  Wait for
    // its shared sleep flag so the next push must take the doorbell path.
    bool sleeping = false;
    for (uint32_t i = 0; i < 1000000; ++i) {
        if (__atomic_load_n(&send_ring->flags.receiver_about_to_sleep, __ATOMIC_ACQUIRE) != 0) {
            sleeping = true;
            break;
        }
    }
    if (!sleeping) {
        try_console_log("ipc: doorbell sleep failed\n");
        while (true) sys_yield();
    }

    uint32_t sleep_seq = 0xCAFEBABE;
    while (!ring_push(send_ring, kRingOrder, MSG_SLEEP_TEST_PING,
                      &sleep_seq, sizeof(sleep_seq), pong_doorbell)) {
        sys_yield();
    }
    ++total_sent;

    bool got_sleep_pong = false;
    while (!got_sleep_pong) {
        if (!ring_wait_pop(recv_ring, kRingOrder, &slot, ping_doorbell)) {
            sys_yield();
            continue;
        }
        if (slot.type == MSG_SLEEP_TEST_PONG) {
            got_sleep_pong = true;
            ++total_recv;
        }
    }
    try_console_log("ipc: doorbell sleep ready\n");
    try_console_log("ipc: cross-core doorbell ready\n");

    try_console_log_counter("metric.ring.messages.sent", total_sent);
    try_console_log_counter("metric.ring.messages.recv", total_recv);
    try_console_log_counter("metric.cross.core.ring.messages.sent", sent);
    try_console_log_counter("metric.cross.core.ring.messages.recv", recv);

    const uint64_t msg_count = total_recv ? total_recv : 1;
    const uint64_t ctx = static_cast<uint64_t>(sys_debug_counter(2));
    const uint64_t ipc = static_cast<uint64_t>(sys_debug_counter(1));
    try_console_log_counter("metric.context.switches.per.1k.msg", (ctx * 1000ull) / msg_count);
    try_console_log_counter("metric.ipc.syscalls.per.1k.msg", (ipc * 1000ull) / msg_count);
    try_console_log_counter("metric.cross.core.context.switches.per.1k.msg",
                            ((ctx_after_hot - ctx_before_hot) * 1000ull) / kMessageCount);
    try_console_log_counter("metric.cross.core.ipc.syscalls.per.1k.msg",
                            ((ipc_after_hot - ipc_before_hot) * 1000ull) / kMessageCount);
    try_console_log_counter("metric.cross.core.notify.calls",
                            notify_after_hot - notify_before_hot);

    if (sys_cap_identify(done_doorbell) == kCapTypeDoorbell) {
        sys_notify(done_doorbell);
    }

    while (true) sys_yield();
}
