#include "runtime.h"
#include "fs_store.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "midix/abi/protocols/fido2.h"
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "usb.fn.fido2");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 131072);
    auto* r = arena.alloc<mochi::fido2::Runtime>();
    if (!r) mochi::boot::invalid_boot();
    r->initialize();
    r->link = mochi::driver::link(boot, "function", mochi::va::native_driver::kLinks);
    r->prompt.ring = mochi::boot::map_ring(boot, {"prompt.ring",
        mochi::va::native_driver::kLinks + mochi::va::native_driver::kLinkStride, "prompt.doorbell"});
    r->generation = mochi::driver::value(boot, "usb.generation");
    const auto gpio_cap = mochi::boot::handle(boot, "presence.mmio");
    const auto seed_cap = mochi::boot::handle(boot, "fido2.seed");
    MxCapInfo info{};
    if (!r->link.valid() || !r->prompt.ring.valid() || !r->generation || !gpio_cap || !seed_cap ||
        mx_cap_info(gpio_cap, &info) != MX_OK || info.type != MX_CAP_MMIO || info.size != 4096 ||
        (info.rights & MX_RIGHT_WRITE) ||
        mx_map_mmio(gpio_cap, mochi::va::native_driver::kRegisters, MX_RIGHT_READ) != MX_OK)
        mochi::boot::invalid_boot();
    r->gpio = {reinterpret_cast<volatile mx_u8_t*>(mochi::va::native_driver::kRegisters), 4096};
    constexpr mx_u64_t seed_va = mochi::va::native_driver::kPayload;
    if (mx_cap_info(seed_cap, &info) != MX_OK || info.type != MX_CAP_ARENA || info.size != 4096 ||
        (info.rights & MX_RIGHT_WRITE) || mx_map_page(seed_cap, seed_va, MX_RIGHT_READ) != MX_OK)
        mochi::boot::invalid_boot();
    const auto* seed = reinterpret_cast<const MxFido2Seed*>(seed_va);
    if (seed->version == MX_FIDO2_SEED_VERSION && seed->provisioned == 1) {
        libk::copy_n(libk::as_bytes(r->hid.auth.master), libk::as_bytes(seed->bytes), 32);
        r->hid.auth.provisioned = true;
    }
    (void)sys_unmap_page(seed_va);
    (void)sys_cap_revoke(seed_cap);
    auto* storage = arena.alloc<mochi::fido2::FileStore>();
    if (!storage || !storage->map(boot,r->hid.auth.master) || !storage->load(r->hid.auth.store))
        mochi::driver::startup_failed("usb.fn.fido2","private persistent state");
    r->hid.auth.enable_store();
    r->hid.auth.prepare_attestation();
    mochi::log_raw_marker(r->hid.auth.attestation_ready
        ? "attestation: packed/x5c\n"
        : "attestation: packed/self (certificate unavailable)\n");
    mochi::log_raw_marker(r->hid.auth.provisioned ? "ready; key provisioned\n" : "ready; key unprovisioned\n");
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return r->service(); })) { mx_yield(); continue; }
        mochi::driver::idle(&r->link, 1, r->link.rx_doorbell, r->deadline(),
            r->outgoing_chunk == 2 && !r->hid.output_pending ? 1 : 0);
    }
}
