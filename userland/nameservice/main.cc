// Discovery uses one SPSC session per client. Init owns the live export catalog.
//
// Each client slot has its own request/reply rings and doorbell pair; the
// service keeps no global session state. Replies are bounded by
// mochi::kSessionReplyTimeoutNs: if a client has died or stopped draining its
// reply ring, the send expires and the slot is dropped, so nameservice never
// wedges on an unresponsive peer.
#include "mochi/sessions.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/nameservice.h"
#include "mochi/va_layout.h"

static void no_close(uint32_t) {}

extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::nameservice::kVaLogRing, "nameservice");
    static mochi::ServerSessions sessions;
    if (!sessions.map(boot)) mochi::boot::invalid_boot();
    for (uint32_t i = 0; i < sessions.directory->count; ++i) sessions.refresh(i, no_close);
    mochi::boot::ready(boot);
    for (;;) {
        bool progress = false;
        for (uint32_t i = 0; i < sessions.directory->count; ++i) {
            if (!sessions.refresh(i, no_close)) continue;
            const auto& endpoint = sessions.endpoints[i];
            RingSlot slot{};
            if (!endpoint.try_recv(&slot)) continue;
            progress = true;
            mx_status_t status = MX_ERR_BAD_PROTOCOL;
            MxEndpointDesc result{};
            if (slot.type == MSG_NS_RESOLVE && slot.length == sizeof(MxNsResolveRequest)) {
                char name[MX_NS_NAME_MAX]{};
                for (uint32_t n = 0; n + 1 < sizeof(name); ++n) name[n] = slot.payload[n];
                status = MX_ERR_NOT_FOUND;
                for (const auto& entry : sessions.directory->exports) {
                    if (!entry.name[0] || !mochi::boot::streq(entry.name, name)) continue;
                    const auto process = __atomic_load_n(&entry.process, __ATOMIC_ACQUIRE);
                    if (!process || sys_cap_identify(process) < 0) { status = MX_ERR_SERVICE_UNAVAILABLE; break; }
                    mx_handle_t granted = MX_NULL_HANDLE;
                    status = mx_cap_grant(sessions.directory->slots[i].client, process, entry.rights, &granted);
                    if (status == MX_OK) {
                        result = {MX_ENDPOINT_MAGIC, MX_ENDPOINT_VERSION, 1, entry.protocol,
                                  MX_ENDPOINT_FLAG_CLIENT_ALLOCATED, {granted, 0, 0, 0}};
                    }
                    break;
                }
            }
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
        if (!progress) sys_yield();
    }
}
