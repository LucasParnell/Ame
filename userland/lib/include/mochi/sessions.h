#pragma once
#include "midix/abi/session.h"
#include "mochi/boot.h"
#include "mochi/endpoint.h"

namespace mochi {
struct ServerSessions {
    MxSessionDirectory* directory = nullptr;
    SpscEndpoint endpoints[MX_MAX_SESSIONS]{};
    uint64_t generations[MX_MAX_SESSIONS]{};
    bool live[MX_MAX_SESSIONS]{};

    bool map(const MxBootInfo* boot, uint64_t va = 0x27000000) {
        auto cap = boot::handle(boot, "sessions");
        if (!cap || sys_map_page(cap, va, RIGHT_READ | RIGHT_WRITE)) return false;
        directory = reinterpret_cast<MxSessionDirectory*>(va);
        return directory->magic == MX_SESSION_MAGIC && directory->count <= MX_MAX_SESSIONS;
    }

    template<class Close> bool refresh(uint32_t i, Close close) {
        auto& slot = directory->slots[i];
        const auto generation = __atomic_load_n(&slot.generation, __ATOMIC_ACQUIRE);
        if (generation != generations[i]) {
            close(i);
            const uint64_t va = 0x28000000ull + i * 0x20000ull;
            if (generations[i]) { sys_unmap_page(va); sys_unmap_page(va + 0x10000); }
            endpoints[i] = {};
            live[i] = false;
            if (!generation || slot.order != MX_DEFAULT_RING_ORDER || !slot.client) return false;
            if (sys_map_page(slot.request_ring, va, RIGHT_READ | RIGHT_WRITE) ||
                sys_map_page(slot.reply_ring, va + 0x10000, RIGHT_READ | RIGHT_WRITE)) return false;
            auto* request = reinterpret_cast<RingPage*>(va);
            auto* reply = reinterpret_cast<RingPage*>(va + 0x10000);
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
        if (live[i] && sys_cap_identify(slot.client) < 0) { close(i); live[i] = false; }
        return live[i];
    }
};
}
