// Discovery uses one SPSC session per client. Init owns the live export catalog.
//
// Each client has separate request/reply rings. Incoming requests and session
// generation changes share one wake doorbell; reply wakes stay per client.
// Replies are bounded by
// mochi::kSessionReplyTimeoutNs: if a client has died or stopped draining its
// reply ring, the send expires and the slot is dropped, so nameservice never
// wedges on an unresponsive peer.
#include "mochi/sessions.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/nameservice.h"
#include "mochi/va_layout.h"
#include "mochi/inbox.h"

static void no_close(uint32_t) {}

// Resolve against init's fixed export table, then grant only the advertised rights.
static mx_status_t resolve(const MxSessionDirectory& directory, mx_u32_t client,
                           const RingSlot& slot, MxEndpointDesc* result) {
    MxNsResolveRequest request{};
    if (!mochi::message_get(slot, &request, MSG_NS_RESOLVE) ||
        libk::str::length_n(request.name, sizeof(request.name)) == sizeof(request.name)) {
        return MX_ERR_BAD_PROTOCOL;
    }
    for (const auto& entry : directory.exports) {
        if (!entry.name[0] || !mochi::boot::streq(entry.name, request.name)) continue;
        const auto process = __atomic_load_n(&entry.process, __ATOMIC_ACQUIRE);
        if (!process || sys_cap_identify(process) != MX_CAP_PROCESS) return MX_ERR_SERVICE_UNAVAILABLE;
        mx_handle_t granted = MX_NULL_HANDLE;
        const mx_status_t status = mx_cap_grant(directory.slots[client].client, process,
                                               entry.rights, &granted);
        if (status == MX_OK) {
            *result = {MX_ENDPOINT_MAGIC, MX_ENDPOINT_VERSION, 1, entry.protocol,
                       MX_ENDPOINT_FLAG_CLIENT_ALLOCATED, {granted, 0, 0, 0}};
        }
        return status;
    }
    return MX_ERR_NOT_FOUND;
}

extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::nameservice::kVaLogRing, "nameservice");
    static mochi::ServerSessions sessions;
    if (!sessions.map(boot)) mochi::boot::invalid_boot();
    const auto wake = mochi::boot::require_handle(boot, "sessions.doorbell");
    // Eight incoming addresses fit one cache line. Build this view only after
    // refreshing generations; the larger endpoint/session records stay cold
    // while deciding whether to sleep.
    alignas(64) static RingPage* incoming[MX_MAX_SESSIONS];
    for (uint32_t i = 0; i < sessions.directory->count; ++i) sessions.refresh(i, no_close);
    mochi::boot::ready(boot);
    for (;;) {
        const auto expected = sys_doorbell_read(wake);
        bool progress = false;
        for (uint32_t i = 0; i < sessions.directory->count; ++i) {
            const bool live = sessions.refresh(i, no_close);
            incoming[i] = live ? sessions.endpoints[i].rx_ring : nullptr;
            if (!live) continue;
            const auto& endpoint = sessions.endpoints[i];
            RingSlot slot{};
            if (!endpoint.try_recv(&slot)) continue;
            progress = true;
            MxEndpointDesc result{};
            const mx_status_t status = resolve(*sessions.directory, i, slot, &result);
            const mx_u64_t deadline = mx_clock_ns() + mochi::kSessionReplyTimeoutNs;
            const bool sent = (status == MX_OK)
                ? endpoint.send_bounded(MSG_NS_RESOLVE_REPLY, &result, sizeof(result), deadline)
                : endpoint.send_bounded(MSG_NS_ERROR, &status, sizeof(status), deadline);
            if (!sent) {
                // Peer dead or stalled: drop the slot; init re-establishes it
                // with a newer generation. The resolve result is discarded.
                sessions.drop(i, no_close);
            }
        }
        if (!progress && mochi::InboxWait{incoming, sessions.directory->count,
                MX_DEFAULT_RING_ORDER, wake}.wait(expected) < 0)
            mochi::boot::invalid_boot();
    }
}
