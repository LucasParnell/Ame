// userland/nameservice/main.cc — V0 nameservice.
//
// Spec ref: §7.3 Nameservice
//
// Maps static service names to capability handles.
// V0: a fixed flat array — no persistence, no hierarchy, no complex policy.
//
// V1 note:
//   Nameservice blocks on its request doorbell when idle instead of yield-polling.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"

constexpr uint32_t kRingOrder = 6;
constexpr uint32_t RIGHT_READ = 1u << 0;
constexpr uint32_t RIGHT_WRITE = 1u << 1;
constexpr uint32_t MSG_NS_RESOLVE = 0x4E530001;
constexpr uint32_t MSG_NS_RESOLVE_REPLY = 0x4E530002;
constexpr uint32_t MSG_NS_ERROR = 0x4E5300FF;
constexpr int64_t kCapTypeSharedRing = 4;
constexpr int64_t kCapTypeDoorbell = 5;
constexpr int kMaxServices = 32;

struct NameEntry {
    char       name[64];
    CapHandle  cap;
    bool       occupied;
};

[[maybe_unused]] static NameEntry g_table[kMaxServices];

constexpr CapHandle cap_slot(uint32_t idx) {
    return (static_cast<CapHandle>(1) << 32) | idx;
}

constexpr CapHandle kRequestRing = cap_slot(0);
constexpr CapHandle kReplyRing = cap_slot(1);
constexpr CapHandle kMyDoorbell = cap_slot(2);
constexpr CapHandle kClientDoorbell = cap_slot(3);
constexpr CapHandle kClientProcess = cap_slot(4);
constexpr CapHandle kPongProcess = cap_slot(5);
constexpr CapHandle kDangoProcess = cap_slot(6);
constexpr CapHandle kLogRing = cap_slot(7);
constexpr CapHandle kLogDoorbell = cap_slot(8);
constexpr uint64_t kVaRequestRing = 0x2000'0000ull;
constexpr uint64_t kVaReplyRing = 0x2001'0000ull;
constexpr uint64_t kVaLogRing = 0x2002'0000ull;

static bool streq(const char* a, const char* b) {
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == b[i];
}

template <typename T>
static T* wait_map(CapHandle cap, int64_t expected_type, uint64_t va) {
    while (true) {
        if (sys_cap_identify(cap) == expected_type) {
            if (sys_map_page(cap, va, RIGHT_READ | RIGHT_WRITE) == 0) {
                return reinterpret_cast<T*>(va);
            }
        }
        sys_yield();
    }
}

static void wait_cap(CapHandle cap, int64_t expected_type) {
    while (sys_cap_identify(cap) != expected_type) {
        sys_yield();
    }
}

static void send_reply(RingPage* replies, uint32_t type, const void* data, uint32_t len) {
    while (!ring_push(replies, kRingOrder, type, data, len, kClientDoorbell)) {
        sys_yield();
    }
}

extern "C" [[noreturn]] void _start() {
    auto* requests = wait_map<RingPage>(kRequestRing, kCapTypeSharedRing, kVaRequestRing);
    auto* replies = wait_map<RingPage>(kReplyRing, kCapTypeSharedRing, kVaReplyRing);
    wait_cap(kMyDoorbell, kCapTypeDoorbell);
    wait_cap(kClientDoorbell, kCapTypeDoorbell);
    auto* log_ring = wait_map<RingPage>(kLogRing, kCapTypeSharedRing, kVaLogRing);
    wait_cap(kLogDoorbell, kCapTypeDoorbell);
    mochi::log_set_sink(log_ring, kRingOrder, kLogDoorbell, "nameservice");

    g_table[0].occupied = true;
    g_table[0].cap = kPongProcess;
    const char name[] = "demo.pong";
    for (uint32_t i = 0; i < sizeof(name); ++i) {
        g_table[0].name[i] = name[i];
    }

    g_table[1].occupied = true;
    g_table[1].cap = kDangoProcess;
    const char dango_name[] = "dango.manager";
    for (uint32_t i = 0; i < sizeof(dango_name); ++i) {
        g_table[1].name[i] = dango_name[i];
    }

    RingSlot slot{};
    while (true) {
        if (!ring_wait_pop(requests, kRingOrder, &slot, kMyDoorbell)) {
            sys_yield();
            continue;
        }

        do {
            if (slot.type != MSG_NS_RESOLVE) continue;

            const char* requested = reinterpret_cast<const char*>(slot.payload);
            int64_t granted = -1;
            for (int i = 0; i < kMaxServices; ++i) {
                if (!g_table[i].occupied) continue;
                if (!streq(g_table[i].name, requested)) continue;
                granted = sys_cap_grant(kClientProcess, g_table[i].cap, RIGHT_READ);
                break;
            }

            if (granted > 0) {
                const uint64_t handle = static_cast<uint64_t>(granted);
                send_reply(replies, MSG_NS_RESOLVE_REPLY, &handle, sizeof(handle));
            } else {
                const uint64_t error = static_cast<uint64_t>(granted);
                send_reply(replies, MSG_NS_ERROR, &error, sizeof(error));
            }
        } while (ring_pop(requests, kRingOrder, &slot));
    }
}
