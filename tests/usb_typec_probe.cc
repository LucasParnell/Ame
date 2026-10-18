#include "mochi/usb/typec_policy.h"
#include <cassert>
#include <cstdio>

using namespace mochi::usb::typec;
struct Bus {
    mx_u8_t selected = 0;
    unsigned writes = 0;
    bool fail = false, mismatch = false;
    bool write(mx_u8_t reg, mx_u8_t value) {
        assert(reg == 2);
        if (fail) return false;
        selected = value;
        ++writes;
        return true;
    }
    bool read(mx_u8_t reg, mx_u8_t& value) {
        assert(reg == 2);
        value = mismatch ? 0 : selected;
        return !fail;
    }
};

struct ConfigBus {
    mx_u8_t registers[4] = {0, 0x12, 0x10, 0x37};
    unsigned transactions = 0;
    unsigned fail_at = 0;
    bool mismatch = false;
    bool transaction(libk::const_bytes out, libk::bytes in) {
        ++transactions;
        if (transactions == fail_at) return false;
        assert(out.size && out.data[0] < 4);
        const auto first = out.data[0];
        if (in.size) {
            assert(out.size == 1 && first + in.size <= 4);
            for (unsigned i = 0; i < in.size; ++i) in.data[i] = registers[first + i];
            if (mismatch && first == 2) in.data[1] ^= 2;
        } else {
            assert(first + out.size - 1 <= 4);
            for (unsigned i = 1; i < out.size; ++i) registers[first + i - 1] = out.data[i];
        }
        return true;
    }
};

static void startup_image() {
    for (unsigned inherited = 0; inherited < 256; ++inherited) {
        ConfigBus bus;
        SinkPreference policy;
        mx_u8_t id = 0;
        bus.registers[3] = inherited;
        assert(policy.configure(bus, 0, id) && id == 0x12);
        assert(bus.transactions == 3 && bus.registers[2] == kSinkMode);
        assert(bus.registers[3] == ((inherited & 1) | 2));
    }
    for (unsigned failure = 1; failure <= 3; ++failure) {
        ConfigBus bus;
        SinkPreference policy;
        mx_u8_t id = 0;
        bus.fail_at = failure;
        assert(!policy.configure(bus, 0, id) && bus.transactions == failure);
    }
    const unsigned bad_ids[] = {0, 255};
    for (const auto bad_id : bad_ids) {
        ConfigBus bus;
        SinkPreference policy;
        mx_u8_t id = 0;
        bus.registers[1] = bad_id;
        assert(!policy.configure(bus, 0, id) && bus.transactions == 1);
    }
    ConfigBus bad_readback;
    SinkPreference policy;
    mx_u8_t id = 0;
    bad_readback.mismatch = true;
    assert(!policy.configure(bad_readback, 0, id));
}

static void decoding() {
    const auto mac = decode(0x1d, 8), keyboard = decode(0x21, 16);
    assert(attached(mac, MxUsbRole::Device) && (mac.flags & MX_TYPEC_VBUS));
    assert(attached(keyboard, MxUsbRole::Host) && (keyboard.flags & MX_TYPEC_CC2));
    assert(can_start_source(keyboard, MX_TYPEC_CC2));
    assert(!can_start_source(keyboard, MX_TYPEC_CC1));
    assert(!can_start_source(mac, MX_TYPEC_CC1));
    assert(!can_start_source(decode(0x29, 16), MX_TYPEC_CC2));
    assert(decode(8, 0).flags == MX_TYPEC_VBUS); // Unattached, externally powered.
    for (unsigned status = 0; status < 256; ++status) {
        for (unsigned type = 0; type < 256; ++type) {
            const auto reply = decode(status, type);
            if (!reply.value) continue;
            assert(status & 1);
            assert(((status >> 4) & 3) == 1 || ((status >> 4) & 3) == 2);
            assert((type & 0x1b) == 8 || (type & 0x1b) == 16);
        }
    }
    assert(decode(0x11, 0x11).status != MX_OK); // Audio accessory plus sink.
    assert(decode(0x31, 16).status != MX_OK); // CC fault.
    assert(decode(0x11, 24).status != MX_OK); // Both partner roles.
}

static void mac_and_keyboard() {
    Bus bus;
    SinkPreference port;
    assert(port.begin(bus, 0) && bus.selected == kSinkMode);
    assert(port.observe(bus, 0x1d, 8, 0, 1, false).value == 2);
    // An attached Mac never gets periodic role toggles or source offers.
    for (unsigned i = 1; i < 100; ++i) {
        assert(port.observe(bus, 0x1d, 8, 0, i * kSinkPreferenceNs, false).value == 2);
    }
    assert(bus.writes == 1);
    auto reply = port.observe(bus, 0, 0, 2, 100 * kSinkPreferenceNs, false);
    assert(!reply.value && bus.selected == kSinkMode);
    const auto start = 100 * kSinkPreferenceNs;
    assert(!port.observe(bus, 0, 0, 0, start + kSinkPreferenceNs - 1, false).value);
    assert(bus.selected == kSinkMode);
    assert(!port.observe(bus, 0, 0, 0, start + kSinkPreferenceNs, false).value);
    assert(bus.selected == kDualRoleMode);
    // A newly discovered sink gets one sink-preference window (dual-role Mac).
    assert(!port.observe(bus, 0x11, 16, 0, start + kSinkPreferenceNs + 1, false).value);
    assert(bus.selected == kSinkMode);
    const auto fallback = start + 2 * kSinkPreferenceNs + 1;
    assert(!port.observe(bus, 0, 0, 2, fallback, false).value);
    assert(bus.selected == kDualRoleMode);
    // A real keyboard stays a sink. Accept it, then tolerate our own VBUS.
    assert(port.observe(bus, 0x11, 16, 2, fallback + 1, false).value == 1);
    assert(port.observe(bus, 0x19, 16, 0, fallback + 2, false).value == 1);
    const auto writes = bus.writes;
    assert(port.observe(bus, 0x19, 16, 0, fallback + 3, false).value == 1);
    assert(bus.writes == writes);
    // Detach/replug between samples revokes the old attachment before reporting
    // the replacement, even when the new keyboard has the same CC orientation.
    assert(!port.observe(bus, 0x11, 16, 2, fallback + 4, false).value);
    assert(bus.selected == kSinkMode);
}

static void prefer_mac_and_failures() {
    Bus bus;
    SinkPreference port;
    assert(port.begin(bus, 0));
    port.observe(bus, 0, 0, 0, kSinkPreferenceNs, false);
    port.observe(bus, 0x11, 16, 0, kSinkPreferenceNs + 1, false);
    // A dual-role partner changes to source during our preference window.
    assert(port.observe(bus, 0x1d, 8, 2, kSinkPreferenceNs + 2, false).value == 2);
    assert(bus.selected == kSinkMode);

    Bus powered;
    SinkPreference held;
    assert(held.begin(powered, 0));
    held.observe(powered, 8, 0, 0, 10 * kSinkPreferenceNs, false);
    assert(powered.selected == kSinkMode); // Never probe against existing VBUS.

    Bus old;
    SinkPreference legacy;
    assert(legacy.begin(old, 0));
    assert(!legacy.observe(old, 0x11, 16, 0, 10 * kSinkPreferenceNs, true).value);
    assert(old.selected == kSinkMode);

    Bus failed;
    SinkPreference retry;
    assert(retry.begin(failed, 0));
    failed.fail = true;
    assert(retry.observe(failed, 0, 0, 0, kSinkPreferenceNs, false).status != MX_OK);
    assert(retry.phase == SinkPreference::Phase::SinkFirst);
    failed.fail = false;
    retry.observe(failed, 0, 0, 0, kSinkPreferenceNs + 1, false);
    failed.mismatch = true;
    assert(retry.observe(failed, 0x11, 16, 0, kSinkPreferenceNs + 2, false).status != MX_OK);
    assert(retry.phase == SinkPreference::Phase::Seeking);
}

static void stable_roles() {
    StableRole policy;
    auto keyboard = decode(0x11, 16);
    assert(policy.observe(keyboard, true, false, MxUsbRole::Detached, 0) == MxUsbRole::Detached);
    assert(policy.observe(keyboard, true, false, MxUsbRole::Detached, kAttachStableNs - 1) == MxUsbRole::Detached);
    assert(policy.observe(keyboard, true, false, MxUsbRole::Detached, kAttachStableNs) == MxUsbRole::Host);
    keyboard.flags |= MX_TYPEC_VBUS; // Our active supply must not look like a fault.
    assert(policy.observe(keyboard, true, false, MxUsbRole::Host, kAttachStableNs + 1) == MxUsbRole::Host);
    assert(policy.observe(keyboard, false, false, MxUsbRole::Host, kAttachStableNs + 2) == MxUsbRole::Detached);
    // A stale host observation with VBUS cannot restart a stopped source.
    assert(policy.observe(keyboard, true, false, MxUsbRole::Detached, 2 * kAttachStableNs) == MxUsbRole::Detached);
    keyboard.flags &= ~MX_TYPEC_VBUS;
    assert(policy.observe(keyboard, true, true, MxUsbRole::Detached, 3 * kAttachStableNs) == MxUsbRole::Detached);
    policy.observe(keyboard, true, false, MxUsbRole::Detached, 4 * kAttachStableNs);
    assert(policy.observe(keyboard, true, false, MxUsbRole::Detached, 5 * kAttachStableNs) == MxUsbRole::Host);
    auto flipped = decode(0x29, 16);
    assert(policy.observe(flipped, true, false, MxUsbRole::Host, 5 * kAttachStableNs + 1) == MxUsbRole::Detached);
    const auto mac = decode(0x2d, 8);
    policy.observe(mac, true, false, MxUsbRole::Detached, 6 * kAttachStableNs);
    assert(policy.observe(mac, true, false, MxUsbRole::Detached, 7 * kAttachStableNs) == MxUsbRole::Device);
    assert(policy.observe(decode(0x21, 8), true, false, MxUsbRole::Device, 7 * kAttachStableNs + 1) == MxUsbRole::Detached);
}

static void source_disconnect_and_mac_replacement() {
    SourceSession source;
    MxBoardReply port{0, MX_OK, 10, MX_ROLE_HOST_ACTIVE, 0};
    source.begin(0);
    assert(source.keep(port, true, 0)); // Bounded time for keyboard pull-up.
    assert(source.keep(port, true, 999999999));
    assert(!source.keep(port, true, 1000000000)); // No data: no ongoing charge.
    source.begin(0);
    port.flags |= MX_ROLE_DATA_CONNECTED;
    assert(source.keep(port, true, 1));
    assert(source.keep(port, true, 10000000000ull)); // Idle keyboard remains powered.
    port.flags &= ~MX_ROLE_DATA_CONNECTED;
    assert(!source.keep(port, true, 10000000001ull)); // Detach cuts without waiting for CC.
    port.flags |= MX_ROLE_DATA_CONNECTED;
    port.value += 2; // Detach/reattach between queries, same CC and USB state.
    assert(!source.keep(port, true, 10000000002ull));
    source.begin(0);
    assert(!source.keep(port, false, 1));
    port.flags = 0;
    assert(!source.keep(port, true, 1));
}

int main() {
    startup_image();
    decoding();
    mac_and_keyboard();
    prefer_mac_and_failures();
    stable_roles();
    source_disconnect_and_mac_replacement();
    std::puts("usb typec switching probe ok");
}
