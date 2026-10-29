#pragma once
// Application construction/wiring belongs here; mapped transport belongs in
// framed_channel.h. No fixed slots, hidden service, or boot-time installer.
#include "mochi/app_process.h"
#include "mochi/framed_channel.h"

namespace mochi::app {
class ChannelPair {
    mx_handle_t caps_[6]{}; // requests, replies, data doorbells, space doorbells
    FrameGeometry geometry_{};
    static bool name(char (&out)[MX_BOOT_NAME_MAX], const char* prefix, const char* suffix) {
        libk::str::Buffer text(out);
        text.append(prefix);
        text.append(suffix);
        return !text.truncated;
    }

  public:
    ChannelPair() = default;
    ChannelPair(const ChannelPair&) = delete;
    ChannelPair& operator=(const ChannelPair&) = delete;
    ~ChannelPair() {
        close();
    }
    void close() {
        for (auto& cap : caps_) {
            if (cap)
                sys_cap_revoke(cap);
            cap = 0;
        }
        geometry_ = {};
    }
    mx_status_t create(const Process& receiver, FrameGeometry geometry, bool handoff = true,
                       uint32_t features = MX_RING_PACKED_FRAMES) {
        if (caps_[0] || !geometry.valid())
            return MX_ERR_INVALID;
        geometry_ = geometry;
        for (uint32_t i = 0; i < 6; ++i) {
            if (i == 2 && handoff) {
                const auto rc = receiver.handoff_doorbell(&caps_[i]);
                if (rc < 0) {
                    close();
                    return rc;
                }
            } else {
                const auto cap =
                    i < 2 ? sys_ring_create(geometry.order, features) : sys_doorbell_create();
                if (cap <= 0) {
                    close();
                    return static_cast<mx_status_t>(cap);
                }
                caps_[i] = static_cast<mx_handle_t>(cap);
            }
        }
        return MX_OK;
    }
    // The same six names map for either role. Geometry travels with the ring
    // entries; child code never needs to repeat the creator's order constant.
    mx_status_t share(Process& child, bool server, const char* prefix = "ipc") const {
        if (!caps_[5] || !prefix || !*prefix)
            return MX_ERR_INVALID;
        char names[6][MX_BOOT_NAME_MAX]{};
        constexpr const char* suffixes[] = {".tx",    ".rx",       ".tx.db",
                                            ".rx.db", ".tx.space", ".rx.space"};
        for (uint32_t i = 0; i < 6; ++i)
            if (!name(names[i], prefix, suffixes[i]))
                return MX_ERR_INVALID;
        auto rc = child.share_ring(names[0], caps_[server ? 1 : 0], geometry_.order);
        if (rc == MX_OK)
            rc = child.share_ring(names[1], caps_[server ? 0 : 1], geometry_.order);
        if (rc == MX_OK)
            rc = child.share_doorbell(names[2], caps_[server ? 3 : 2], RIGHT_NOTIFY);
        if (rc == MX_OK)
            rc = child.share_doorbell(names[3], caps_[server ? 2 : 3], RIGHT_WAIT);
        if (rc == MX_OK)
            rc = child.share_doorbell(names[4], caps_[server ? 5 : 4], RIGHT_WAIT);
        if (rc == MX_OK)
            rc = child.share_doorbell(names[5], caps_[server ? 4 : 5], RIGHT_NOTIFY);
        return rc;
    }
    static SpscEndpoint map(const MxBootInfo* boot, mx_vaddr_t tx, mx_vaddr_t rx,
                            const char* prefix = "ipc") {
        if (!prefix || !*prefix)
            return {};
        char names[6][MX_BOOT_NAME_MAX]{};
        constexpr const char* suffixes[] = {".tx",    ".rx",       ".tx.db",
                                            ".rx.db", ".tx.space", ".rx.space"};
        for (uint32_t i = 0; i < 6; ++i)
            if (!name(names[i], prefix, suffixes[i]))
                return {};
        return boot::map_endpoint(boot, {names[0], tx, names[2], 0, names[4]},
                                  {names[1], rx, names[3], 0, names[5]});
    }
};
} // namespace mochi::app
