#pragma once
#include "midix/abi/protocols/board_usb.h"
#include "libk/bytes.h"

namespace mochi::usb::typec {
constexpr mx_u64_t kSinkPreferenceNs = 600000000ull;
constexpr mx_u64_t kAttachStableNs = 100000000ull;
constexpr mx_u64_t kDeviceDetachStableNs = 100000000ull;
constexpr mx_u32_t kOrientation = MX_TYPEC_CC1 | MX_TYPEC_CC2;
constexpr mx_u8_t kSinkMode = 0x04, kDualRoleMode = 0x10;

// TYPE describes the partner: Source means we are a device, Sink means host.
// Preserve VBUS even while unattached so residual/external power blocks boost.
inline MxBoardReply decode(mx_u8_t status, mx_u8_t type) {
    MxBoardReply out{0, MX_OK, 0, (status & 8) ? MX_TYPEC_VBUS : 0u, 0};
    if (!(status & 1)) {
        return out;
    }
    const auto cc = (status >> 4) & 3;
    const auto partner = type & 0x1b; // Reject accessory and ambiguous partners.
    if ((cc != 1 && cc != 2) || (partner != 8 && partner != 16)) {
        out.status = MX_ERR_INVALID;
        return out;
    }
    out.value = static_cast<mx_u32_t>(partner == 8 ? MxUsbRole::Device : MxUsbRole::Host);
    out.flags |= MX_TYPEC_ATTACHED | (cc == 1 ? MX_TYPEC_CC1 : MX_TYPEC_CC2);
    return out;
}

inline bool attached(const MxBoardReply& status, MxUsbRole role) {
    const auto cc = status.flags & kOrientation;
    return status.status == MX_OK && !status.reserved &&
           status.value == static_cast<mx_u32_t>(role) && (status.flags & MX_TYPEC_ATTACHED) &&
           (cc == MX_TYPEC_CC1 || cc == MX_TYPEC_CC2);
}

inline bool can_start_source(const MxBoardReply& status, mx_u32_t expected_cc) {
    return attached(status, MxUsbRole::Host) && !(status.flags & MX_TYPEC_VBUS) &&
           (status.flags & kOrientation) == expected_cc;
}

// One small, service-owned control row; no heap, locks or per-poll history.
// Start as a sink for a Mac/charger. If DRP later finds a sink, give the partner
// one more chance to become the source before accepting a keyboard/OTG role.
// The 600 ms preference is our policy, not an undocumented chip register bit.
struct SinkPreference {
    enum class Phase : mx_u8_t { SinkFirst, Seeking, PreferSink, HostFallback };
    mx_u64_t deadline = 0;
    Phase phase = Phase::SinkFirst;
    MxUsbRole reported = MxUsbRole::Detached;

    // ID/Modes/Control are contiguous. Construct and verify one register image
    // in three I2C round trips, including ID validation. Individual register
    // read/modify/write calls serialize extra service wakeups on the boot path.
    template <typename Bus> bool configure(Bus& bus, mx_u64_t now, mx_u8_t& id) {
        const mx_u8_t first = 1, modes = 2;
        mx_u8_t initial[3]{}, verified[2]{};
        if (!bus.transaction(libk::as_bytes(&first, 1), libk::as_bytes(initial))) {
            return false;
        }
        id = initial[0];
        if (!id || id == 255) {
            return false;
        }
        // Default USB advertisement, sink-biased DRP dwell, inherited IRQ mask.
        // Reserved control bits are written as zero.
        const mx_u8_t image[] = {modes, kSinkMode, static_cast<mx_u8_t>((initial[2] & 1) | 0x02)};
        if (!bus.transaction(libk::as_bytes(image), {}) ||
            !bus.transaction(libk::as_bytes(&modes, 1), libk::as_bytes(verified)) ||
            (verified[0] & 0x3f) != kSinkMode || (verified[1] & 0x37) != image[2]) {
            return false;
        }
        phase = Phase::SinkFirst;
        reported = MxUsbRole::Detached;
        deadline = now + kSinkPreferenceNs;
        return true;
    }
    template <typename Bus> static bool mode(Bus& bus, mx_u8_t value) {
        mx_u8_t actual = 0;
        return bus.write(2, value) && bus.read(2, actual) && (actual & 0x3f) == value;
    }
    template <typename Bus> bool begin(Bus& bus, mx_u64_t now) {
        phase = Phase::SinkFirst;
        reported = MxUsbRole::Detached;
        deadline = now + kSinkPreferenceNs;
        return mode(bus, kSinkMode);
    }
    template <typename Bus>
    MxBoardReply observe(Bus& bus, mx_u8_t status, mx_u8_t type, mx_u8_t interrupts, mx_u64_t now,
                         bool device_only) {
        auto out = decode(status, type);
        if (out.status != MX_OK) {
            return out;
        }
        const auto role = static_cast<MxUsbRole>(out.value);
        // A latched detach catches unplug/replug between polls. Mode changes
        // also generate detach, so only rearm after an attachment was reported.
        // A latched detach describes history, not the current cable state.
        // Do not rewrite Sink mode and break an enumerated device when the
        // current snapshot still shows the same externally powered source.
        const bool device_connected = reported == MxUsbRole::Device && (out.flags & MX_TYPEC_VBUS);
        if (device_connected) {
            out.value = static_cast<mx_u32_t>(MxUsbRole::Device);
        }
        if (reported == MxUsbRole::Device && !device_connected) {
            // We already selected Sink for this device connection. Rewriting
            // the mode here would disconnect it before StableRole can debounce
            // a short VBUS loss. Rearm policy without touching the controller.
            reported = MxUsbRole::Detached;
            phase = Phase::SinkFirst;
            deadline = now + kSinkPreferenceNs;
            out.value = 0;
            out.flags &= MX_TYPEC_VBUS;
            return out;
        }
        if (!device_connected && reported != MxUsbRole::Detached &&
            ((interrupts & 2) || role != reported)) {
            if (!begin(bus, now)) {
                out.status = MX_ERR_FAULT;
            }
            out.value = 0;
            out.flags &= MX_TYPEC_VBUS;
            return out;
        }
        if (attached(out, MxUsbRole::Device) && (out.flags & MX_TYPEC_VBUS)) {
            reported = MxUsbRole::Device;
            return out;
        }
        if (!device_only && phase == Phase::HostFallback && attached(out, MxUsbRole::Host)) {
            reported = MxUsbRole::Host;
            return out;
        }
        if (!device_only && phase == Phase::Seeking && attached(out, MxUsbRole::Host)) {
            if (mode(bus, kSinkMode)) {
                phase = Phase::PreferSink;
                deadline = now + kSinkPreferenceNs;
            } else {
                out.status = MX_ERR_FAULT;
            }
        } else if (!device_only && !(out.flags & MX_TYPEC_VBUS) && now >= deadline &&
                   (phase == Phase::SinkFirst || phase == Phase::PreferSink)) {
            const auto next = phase == Phase::SinkFirst ? Phase::Seeking : Phase::HostFallback;
            if (mode(bus, kDualRoleMode)) {
                phase = next;
            } else {
                out.status = MX_ERR_FAULT;
            }
        }
        out.value = 0;
        out.flags &= MX_TYPEC_VBUS;
        return out;
    }
};

// Attach is debounced. An active device stays a device until a successful
// snapshot proves detach or VBUS loss; query failures are not cable removal.
// Host failures still cut source power immediately. VBUS is expected while we supply a host,
// but must be absent before starting a new source lease.
struct StableRole {
    mx_u64_t since = 0, detach_since = 0;
    mx_u32_t candidate = 0, active = 0;

    void reset() {
        candidate = active = 0;
        detach_since = 0;
    }
    MxUsbRole observe(const MxBoardReply& status, bool query_ok, bool device_only,
                      MxUsbRole current, mx_u64_t now) {
        const auto role = static_cast<MxUsbRole>(status.value);
        if (current == MxUsbRole::Device && active) {
            if (!query_ok || status.status != MX_OK || status.reserved) {
                return current;
            }
            // Legacy A-to-C cables retain external VBUS for the connection.
            // A momentary CC/partner change must not restart enumeration.
            if (status.flags & MX_TYPEC_VBUS) {
                detach_since = 0;
                return current;
            }
            if (!detach_since) {
                detach_since = now ? now : 1;
            }
            if (now < detach_since || now - detach_since < kDeviceDetachStableNs) {
                return current;
            }
            reset();
            return MxUsbRole::Detached;
        }
        if (!query_ok || (role != MxUsbRole::Host && role != MxUsbRole::Device) ||
            !attached(status, role) || (device_only && role == MxUsbRole::Host) ||
            (role == MxUsbRole::Device && !(status.flags & MX_TYPEC_VBUS)) ||
            (role == MxUsbRole::Host && current != role && (status.flags & MX_TYPEC_VBUS))) {
            reset();
            return MxUsbRole::Detached;
        }
        const auto signature = (status.flags & kOrientation) | (status.value << 8);
        if (candidate != signature) {
            candidate = signature;
            since = now;
        }
        if (current != MxUsbRole::Detached) {
            if (active == signature && current == role) {
                return current;
            }
            active = 0;
            return MxUsbRole::Detached;
        }
        if (now - since < kAttachStableNs) {
            return MxUsbRole::Detached;
        }
        active = signature;
        return role;
    }
};
static_assert(sizeof(SinkPreference) + sizeof(StableRole) <= 64);

// CC alone cannot distinguish a keyboard from a computer accepting charge.
// A source session must acquire a USB data connection within one second and
// retain that connection's generation. A fast unplug/replug cannot inherit it.
struct SourceSession {
    mx_u64_t deadline = 0;
    mx_u32_t generation = 0;
    bool seen = false;
    void begin(mx_u64_t now) {
        deadline = now + 1000000000ull;
        generation = 0;
        seen = false;
    }
    bool keep(const MxBoardReply& port, bool query_ok, mx_u64_t now) {
        if (!query_ok || port.status != MX_OK || port.reserved ||
            !(port.flags & MX_ROLE_HOST_ACTIVE)) {
            return false;
        }
        if (!(port.flags & MX_ROLE_DATA_CONNECTED)) {
            return !seen && now < deadline;
        }
        if (seen && port.value != generation) {
            return false;
        }
        seen = true;
        generation = port.value;
        return true;
    }
};
static_assert(sizeof(SourceSession) == 16);
} // namespace mochi::usb::typec
