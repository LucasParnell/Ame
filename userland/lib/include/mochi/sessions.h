#pragma once
#include "midix/abi/session.h"
#include "mochi/boot.h"
#include "mochi/endpoint.h"

namespace mochi {

// Bounded reply window for one client request. A server that cannot push a
// reply inside this window treats the peer as stalled: the session is dropped
// (see drop()) and re-established only when init hands the slot a new client
// with a newer generation. The value is deliberately much larger than a single
// reply push so a merely busy scheduler never trips it; it only fires when a
// peer stops draining its reply ring entirely.
constexpr mx_u64_t kSessionReplyTimeoutNs = 250'000'000ull;

struct ServerSessions {
    MxSessionDirectory* directory = nullptr;
    SpscEndpoint endpoints[MX_MAX_SESSIONS]{};
    uint64_t generations[MX_MAX_SESSIONS]{};
    bool live[MX_MAX_SESSIONS]{};
    bool mapped[MX_MAX_SESSIONS]{};

    bool map(const MxBootInfo* boot, uint64_t directory_va = va::sessions::kDirectory) {
        auto cap = boot::handle(boot, "sessions");
        if (!cap || sys_map_page(cap, directory_va, RIGHT_READ | RIGHT_WRITE)) return false;
        auto* candidate = reinterpret_cast<MxSessionDirectory*>(directory_va);
        if (candidate->magic != MX_SESSION_MAGIC || candidate->count > MX_MAX_SESSIONS) {
            (void)sys_unmap_page(directory_va);
            return false;
        }
        directory = candidate;
        return true;
    }

    // The generation currently acknowledged for slot i. Handles opened for a
    // client are tagged with this value so a token minted before a restart can
    // never be replayed against the replacement client.
    uint64_t generation_of(uint32_t i) const { return i < MX_MAX_SESSIONS ? generations[i] : 0; }

    // Peer liveness for slot i. sweep_process_caps() revokes a dead client's
    // process cap out of the server's table, so identify() fails once the
    // client has exited even if init has not yet bumped the generation.
    bool client_alive(uint32_t i) const {
        return directory && i < directory->count && i < MX_MAX_SESSIONS &&
               sys_cap_identify(directory->slots[i].client) == MX_CAP_PROCESS;
    }

    template<class Close> bool refresh(uint32_t i, Close close) {
        if (!directory || i >= directory->count || i >= MX_MAX_SESSIONS) return false;
        auto& slot = directory->slots[i];
        const auto generation = __atomic_load_n(&slot.generation, __ATOMIC_ACQUIRE);
        if (generation != generations[i]) {
            close(i);
            const uint64_t request_va = va::sessions::request(i);
            const uint64_t reply_va = va::sessions::reply(i);
            if (mapped[i]) {
                (void)sys_unmap_page(request_va);
                (void)sys_unmap_page(reply_va);
                mapped[i] = false;
            }
            endpoints[i] = {};
            live[i] = false;
            if (!generation || slot.order != MX_DEFAULT_RING_ORDER || !slot.client ||
                !slot.request_doorbell || !slot.reply_doorbell) return false;
            if (sys_map_page(slot.request_ring, request_va, RIGHT_READ | RIGHT_WRITE)) return false;
            if (sys_map_page(slot.reply_ring, reply_va, RIGHT_READ | RIGHT_WRITE)) {
                (void)sys_unmap_page(request_va);
                return false;
            }
            mapped[i] = true;
            auto* request = reinterpret_cast<RingPage*>(request_va);
            auto* reply = reinterpret_cast<RingPage*>(reply_va);
            // The old client has exited and the replacement is suspended.
            request->head.v = request->tail.v = 0;
            request->flags = {};
            reply->head.v = reply->tail.v = 0;
            reply->flags = {};
            endpoints[i] = {reply, request, slot.order, slot.reply_doorbell, slot.request_doorbell};
            generations[i] = generation;
            live[i] = true;
            __atomic_store_n(&slot.acknowledged, generation, __ATOMIC_RELEASE);
        }
        if (live[i] && !client_alive(i)) { close(i); live[i] = false; }
        return live[i];
    }

    // Drop a client whose session can no longer make progress. The slot stays
    // closed until init publishes a new generation, which refresh() maps and
    // acknowledges. Endpoints are cleared but the fixed mapping is left for the
    // next generation to unmap before remapping, so no VA is leaked.
    template<class Close> void drop(uint32_t i, Close close) {
        if (i >= MX_MAX_SESSIONS) return;
        close(i);
        endpoints[i] = {};
        live[i] = false;
    }
};
}
