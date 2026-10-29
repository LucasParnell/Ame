#pragma once
// xHCI encodings and bounded producer rings. CPU-only tests use the same code
// as the HCD. Queue memory must be coherent or uniformly non-cacheable.
#include "libk/dma.h"
#include "libk/integer.h"
#include "midix/abi/protocols/usb_transfer.h"

namespace midix::usb::xhci {

constexpr mx_u32_t kCycle = 1u << 0;
constexpr mx_u32_t kToggleCycle = 1u << 1;
constexpr mx_u32_t kInterruptOnShort = 1u << 2;
constexpr mx_u32_t kChain = 1u << 4;
constexpr mx_u32_t kInterruptOnCompletion = 1u << 5;
constexpr mx_u32_t type(mx_u32_t value) { return value << 10; }
struct Trb { mx_u64_t parameter; mx_u32_t status; mx_u32_t control; };
static_assert(sizeof(Trb) == 16);

// Default xHCI speed IDs are not the portable USB speed enumeration. A backend
// with explicit Supported Protocol PSI entries must use its discovered map.
inline bool standard_speed(mx_u8_t id, MxUsbSpeed* out) {
    if (!out) return false;
    switch (id) {
    case 1: *out = MxUsbSpeed::Full; return true;
    case 2: *out = MxUsbSpeed::Low; return true;
    case 3: *out = MxUsbSpeed::High; return true;
    case 4: *out = MxUsbSpeed::Super; return true;
    default: return false;
    }
}

struct ProducerRing {
    Trb* trbs = nullptr;
    mx_u64_t phys = 0;
    mx_u32_t count = 0;
    mx_u32_t index = 0;
    mx_u32_t cycle = 1;
    mx_u32_t outstanding = 0;

    bool init(Trb* storage, mx_u64_t device_address, mx_u32_t descriptors) {
        if (!storage || descriptors < 3 || descriptors > 4096 ||
            (reinterpret_cast<mx_u64_t>(storage) & 63u) || (device_address & 63u)) return false;
        const mx_u64_t bytes = static_cast<mx_u64_t>(descriptors) * sizeof(Trb);
        if (bytes - 1 > ~mx_u64_t{0} - device_address ||
            (device_address >> 16) != ((device_address + bytes - 1) >> 16)) return false;
        trbs = storage; phys = device_address; count = descriptors;
        index = outstanding = 0; cycle = 1;
        for (mx_u32_t i = 0; i < count; ++i) trbs[i] = {};
        write_link(false);
        return true;
    }
    mx_u32_t available() const { return count ? count - 1 - outstanding : 0; }
    mx_u64_t enqueue(Trb trb) {
        if (!available()) return 0;
        const auto at = index;
        trb.control = (trb.control & ~kCycle) | cycle;
        trbs[at].parameter = trb.parameter;
        trbs[at].status = trb.status;
        libk::dma::publish();
        __atomic_store_n(&trbs[at].control, trb.control, __ATOMIC_RELEASE);
        ++outstanding;
        if (++index == count - 1) {
            write_link((trb.control & kChain) != 0);
            index = 0;
            cycle ^= 1u;
        }
        return phys + static_cast<mx_u64_t>(at) * sizeof(Trb);
    }
    // Only hardware retirement permits reuse. A timeout alone is insufficient.
    bool retire(mx_u32_t descriptors) {
        if (!descriptors || descriptors > outstanding) return false;
        outstanding -= descriptors;
        return true;
    }
private:
    void write_link(bool chained) {
        auto& link = trbs[count - 1];
        link.parameter = phys;
        link.status = 0;
        libk::dma::publish();
        __atomic_store_n(&link.control, type(6) | kToggleCycle | cycle | (chained ? kChain : 0), __ATOMIC_RELEASE);
    }
};

inline bool interrupt_interval(MxUsbSpeed speed, mx_u8_t interval, mx_u32_t* out) {
    if (!out || !interval || speed < MxUsbSpeed::Low || speed > MxUsbSpeed::SuperPlus) return false;
    if (speed >= MxUsbSpeed::High) {
        if (interval > 16) return false;
        *out = interval - 1; // USB encodes an exponent plus one at HS/SS.
    } else {
        mx_u32_t exponent = 0, frames = 1;
        while (frames < interval) { frames <<= 1; ++exponent; }
        *out = exponent + 3; // xHCI units are 125 us; FS/LS bInterval is ms.
    }
    return true;
}

// Split normal TDs at 64 KiB boundaries. The caller reserves all ring slots
// before publishing any descriptor; rejected requests never create partial TDs.
inline mx_u32_t normal_trb_count(mx_u64_t address, mx_u32_t length, mx_u16_t packet, bool zlp) {
    if (!packet || (length && length - 1 > ~mx_u64_t{0} - address)) return 0;
    if (length && zlp && length % packet == 0 && length > ~mx_u64_t{0} - address) return 0;
    if (!length) return 1;
    const auto first = 65536u - static_cast<mx_u32_t>(address & 0xffffu);
    mx_u32_t count = 1;
    if (length > first) count += static_cast<mx_u32_t>(libk::div_round_up(length - first, 65536));
    if (zlp && length % packet == 0) ++count;
    return count;
}

inline bool encode_normal_td(Trb* out, mx_u32_t capacity, mx_u64_t address, mx_u32_t length,
                              mx_u16_t packet, MxUsbDirection direction, bool zlp, mx_u32_t* written) {
    const auto needed = normal_trb_count(address, length, packet, zlp);
    if (!out || !written || !needed || needed > capacity || direction > MxUsbDirection::In ||
        (zlp && direction != MxUsbDirection::Out)) return false;
    mx_u32_t remaining = length;
    for (mx_u32_t i = 0; i < needed; ++i) {
        const auto boundary = 65536u - static_cast<mx_u32_t>(address & 0xffffu);
        const auto bytes = remaining < boundary ? remaining : boundary;
        remaining -= bytes;
        auto packets = libk::div_round_up(remaining, packet);
        if (packets > 31) packets = 31;
        out[i] = {address, bytes | (static_cast<mx_u32_t>(packets) << 17), type(1) |
                  (direction == MxUsbDirection::In ? kInterruptOnShort : 0) |
                  (i + 1 == needed ? kInterruptOnCompletion : kChain)};
        if (i + 1 != needed) address += bytes;
    }
    *written = needed;
    return true;
}

} // namespace midix::usb::xhci
