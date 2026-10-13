#pragma once
#include "mochi/boot.h"
#include "mochi/state.h"
#include "libk/mmio.h"
#include "libk/str.h"
#include "libk/printf.h"
namespace mochi::driver {
// Startup failures must remain identifiable before the terminal is activated.
// This cold path goes directly to the retained debug sink and exits the service;
// it does not turn missing hardware authority into an ambient fallback.
[[noreturn]] inline void startup_failed(const char* service, const char* operation,
                                       mx_status_t status = MX_ERR_BAD_PROTOCOL) {
    char line[160];
    const int length = libk::snprintf(line, sizeof(line), "%s: startup failed: %s status=%lld\n",
                                     service, operation, static_cast<long long>(status));
    if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
        sys_debug_puts(line, length);
    }
    mx_exit(static_cast<mx_i32_t>(status));
    for (;;) mx_yield();
}
inline bool name(char (&out)[32], const char* prefix, const char* suffix) {
    const auto n = libk::str::length_n(prefix, 32), m = libk::str::length_n(suffix, 32);
    if (n + m >= 32) {
        return false;
    }
    libk::copy_n(libk::as_bytes(out), libk::as_bytes(prefix, n), n);
    libk::copy_n(libk::as_bytes(out + n, 32 - n), libk::as_bytes(suffix, m + 1), m + 1);
    return true;
}
inline SpscEndpoint link(const MxBootInfo* boot, const char* prefix, mx_u64_t va) {
    char tx[32], rx[32], td[32], rd[32];
    if (!name(tx, prefix, ".tx") || !name(rx, prefix, ".rx") || !name(td, prefix, ".tx.db") ||
        !name(rd, prefix, ".rx.db")) {
        return {};
    }
    return boot::map_endpoint(boot, {tx, va, td}, {rx, va + 0x10000, rd});
}
inline mx_u64_t value(const MxBootInfo* boot, const char* name, mx_u64_t fallback = 0) {
    const auto* entry = boot::find_type(boot, name, MX_BOOT_ENTRY_VALUE);
    return entry ? entry->aux0 : fallback;
}
inline libk::Mmio registers(const MxBootInfo* boot, const char* name, mx_u64_t va,
                            const char* service = nullptr) {
    const auto cap = boot::handle(boot, name);
    MxCapInfo info{};
    auto status = cap ? mx_cap_info(cap, &info) : MX_ERR_NOT_FOUND;
    if (status == MX_OK && (info.type != MX_CAP_MMIO || info.attr != MX_MMIO_DEVICE_nGnRnE)) {
        status = MX_ERR_BAD_PROTOCOL;
    }
    if (status == MX_OK) {
        status = mx_map_mmio(cap, va, MX_RIGHT_READ | MX_RIGHT_WRITE);
    }
    if (status != MX_OK) {
        if (service) startup_failed(service, name, status);
        return {};
    }
    return {reinterpret_cast<volatile mx_u8_t*>(va), static_cast<size_t>(info.size)};
}
// Arm every producer ring on a common receiver doorbell before checking work.
// Expiry is a normal wake; the caller always rechecks hardware and messages.
inline void idle(const SpscEndpoint* links, mx_u32_t count, mx_handle_t wake, mx_u64_t deadline) {
    const auto expected = mx_doorbell_read(wake);
    bool pending = false;
    for (mx_u32_t i = 0; i < count; ++i) {
        if (links[i].rx_ring) {
            links[i].rx().publish_sleep(expected);
        }
    }
    for (mx_u32_t i = 0; i < count; ++i) {
        if (links[i].rx_ring && links[i].rx().depth()) {
            pending = true;
        }
    }
    if (!pending) {
        (void)mx_doorbell_wait_until(wake, expected, deadline);
    }
    for (mx_u32_t i = 0; i < count; ++i) {
        if (links[i].rx_ring) {
            links[i].rx().clear_sleep();
        }
    }
}
inline bool receive(const SpscEndpoint& link, MxRingSlot& slot, mx_u64_t deadline) {
    while (!link.try_recv(&slot)) {
        if (mx_clock_ns() >= deadline) {
            return false;
        }
        idle(&link, 1, link.rx_doorbell, deadline);
    }
    return true;
}
} // namespace mochi::driver
