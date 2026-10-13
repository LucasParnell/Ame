#pragma once
#include "mochi/boot.h"
#include "mochi/state.h"
#include "mochi/endpoint.h"
#include "libk/bytes.h"

namespace mochi::usb {
inline SpscEndpoint map_link(const MxBootInfo* boot, bool controller,
                             mx_u64_t tx_va, mx_u64_t rx_va) {
    const char* tx = controller ? "usb.controller.tx" : "usb.host.tx";
    const char* rx = controller ? "usb.controller.rx" : "usb.host.rx";
    const char* td = controller ? "usb.controller.tx.db" : "usb.host.tx.db";
    const char* rd = controller ? "usb.controller.rx.db" : "usb.host.rx.db";
    auto t = boot::map_ring(boot, {tx, tx_va, td});
    auto r = boot::map_ring(boot, {rx, rx_va, rd});
    if (t.order != r.order) return {};
    return {t.ring, r.ring, t.order, t.doorbell, r.doorbell};
}
inline libk::bytes map_payload(const MxBootInfo* boot, const char* name, mx_u64_t va) {
    const auto cap = boot::handle(boot, name);
    MxCapInfo info{};
    if (!cap || mx_cap_info(cap, &info) != MX_OK || info.type != MX_CAP_ARENA || info.size < 4096 ||
        info.attr != MX_ARENA_MEMORY_WRITE_BACK ||
        mx_map_page(cap, va, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) return {};
    return libk::as_bytes(reinterpret_cast<void*>(va), info.size);
}
using mochi::make_state;
} // namespace mochi::usb
