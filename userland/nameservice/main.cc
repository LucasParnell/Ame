// userland/nameservice/main.cc — V0 nameservice.
//
// Nameservice is now a consumer of the generic BootInfo ABI instead of a fixed
// cap-slot layout. Init registers boot-time endpoints by placing
// MX_BOOT_ENTRY_ENDPOINT entries named "service.<name>" in this service's
// BootInfo page. Resolve replies are endpoint descriptors small enough to fit in
// one ring slot.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "midix/abi/protocols/nameservice.h"

constexpr uint32_t kRingOrder = 6;
constexpr int kMaxServices = 32;
constexpr uint64_t kVaRequestRing = 0x2000'0000ull;
constexpr uint64_t kVaReplyRing = 0x2001'0000ull;
constexpr uint64_t kVaLogRing = 0x2002'0000ull;

struct NameEntry {
    char       name[MX_NS_NAME_MAX];
    CapHandle  cap;
    uint32_t   protocol;
    uint32_t   flags;
    uint32_t   grant_rights;
    bool       occupied;
};

[[maybe_unused]] static NameEntry g_table[kMaxServices];

static bool streq(const char* a, const char* b) {
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == b[i];
}

static bool starts_with(const char* s, const char* prefix) {
    uint32_t i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return false;
        ++i;
    }
    return true;
}

static void copy_name(char* dst, uint32_t dst_len, const char* src) {
    if (!dst || dst_len == 0) return;
    uint32_t i = 0;
    if (src) {
        while (i + 1 < dst_len && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
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

static void register_boot_services(const MxBootInfo* boot) {
    const char prefix[] = "service.";
    for (uint16_t i = 0; i < boot->entry_count && i < MX_BOOT_MAX_ENTRIES; ++i) {
        const MxBootEntry& e = boot->entries[i];
        if (e.type != MX_BOOT_ENTRY_ENDPOINT) continue;
        if (!starts_with(e.name, prefix)) continue;
        for (int slot = 0; slot < kMaxServices; ++slot) {
            if (g_table[slot].occupied) continue;
            g_table[slot].occupied = true;
            copy_name(g_table[slot].name, sizeof(g_table[slot].name), e.name + sizeof(prefix) - 1);
            g_table[slot].cap = e.handle;
            g_table[slot].protocol = static_cast<uint32_t>(e.aux0);
            g_table[slot].grant_rights = e.aux1 ? static_cast<uint32_t>(e.aux1) : RIGHT_READ;
            g_table[slot].flags = e.flags;
            break;
        }
    }
}

static void send_reply(RingPage* replies, uint32_t type, const void* data, uint32_t len,
                       CapHandle client_doorbell) {
    while (!ring_push(replies, kRingOrder, type, data, len, client_doorbell)) {
        sys_yield();
    }
}

static void send_endpoint_reply(RingPage* replies, const MxEndpointDesc& endpoint,
                                CapHandle client_doorbell) {
    send_reply(replies, MSG_NS_RESOLVE_REPLY, &endpoint, sizeof(endpoint), client_doorbell);
}

static void send_error(RingPage* replies, mx_status_t status, CapHandle client_doorbell) {
    send_reply(replies, MSG_NS_ERROR, &status, sizeof(status), client_doorbell);
}

static void request_name_from_slot(const RingSlot& slot, char* out, uint32_t out_len) {
    if (!out || out_len == 0) return;
    if (slot.length >= sizeof(MxNsResolveRequest)) {
        const auto* req = reinterpret_cast<const MxNsResolveRequest*>(slot.payload);
        copy_name(out, out_len, req->name);
        return;
    }
    // Backward-compatible V0: payload is directly a nul-terminated name.
    copy_name(out, out_len, reinterpret_cast<const char*>(slot.payload));
}

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();
    const CapHandle request_cap = mochi::boot::require_handle(boot, "ns.requests");
    const CapHandle reply_cap = mochi::boot::require_handle(boot, "ns.replies");
    const CapHandle my_doorbell = mochi::boot::require_handle(boot, "ns.server.db");
    const CapHandle client_doorbell = mochi::boot::require_handle(boot, "ns.client.db");
    const CapHandle client_process = mochi::boot::require_handle(boot, "client.process");
    const CapHandle log_cap = mochi::boot::require_handle(boot, "log.ring");
    const CapHandle log_doorbell = mochi::boot::require_handle(boot, "log.doorbell");

    auto* requests = wait_map<RingPage>(request_cap, kCapTypeSharedRing, kVaRequestRing);
    auto* replies = wait_map<RingPage>(reply_cap, kCapTypeSharedRing, kVaReplyRing);
    wait_cap(my_doorbell, kCapTypeDoorbell);
    wait_cap(client_doorbell, kCapTypeDoorbell);
    wait_cap(client_process, kCapTypeProcess);
    auto* log_ring = wait_map<RingPage>(log_cap, kCapTypeSharedRing, kVaLogRing);
    wait_cap(log_doorbell, kCapTypeDoorbell);
    mochi::log_set_sink(log_ring, kRingOrder, log_doorbell, "nameservice");

    register_boot_services(boot);

    RingSlot slot{};
    while (true) {
        if (!ring_wait_pop(requests, kRingOrder, &slot, my_doorbell)) {
            sys_yield();
            continue;
        }

        do {
            if (slot.type != MSG_NS_RESOLVE) continue;

            char requested[MX_NS_NAME_MAX]{};
            request_name_from_slot(slot, requested, sizeof(requested));
            mx_status_t status = MX_ERR_NOT_FOUND;
            MxEndpointDesc endpoint{};
            for (int i = 0; i < kMaxServices; ++i) {
                if (!g_table[i].occupied) continue;
                if (!streq(g_table[i].name, requested)) continue;

                mx_handle_t granted = MX_NULL_HANDLE;
                status = mx_cap_grant(client_process,
                                      g_table[i].cap,
                                      g_table[i].grant_rights,
                                      &granted);
                if (status == MX_OK) {
                    endpoint.magic = MX_ENDPOINT_MAGIC;
                    endpoint.version = MX_ENDPOINT_VERSION;
                    endpoint.handle_count = 1;
                    endpoint.protocol = g_table[i].protocol;
                    endpoint.flags = g_table[i].flags;
                    endpoint.handles[0] = granted;
                }
                break;
            }

            if (status == MX_OK) {
                send_endpoint_reply(replies, endpoint, client_doorbell);
            } else {
                send_error(replies, status, client_doorbell);
            }
        } while (ring_pop(requests, kRingOrder, &slot));
    }
}
