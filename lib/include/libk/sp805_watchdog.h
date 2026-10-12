#pragma once
#include <cstdint>

namespace libk::sp805 {
using u32 = std::uint32_t;
constexpr u32 kLoad = 0, kValue = 4, kControl = 8, kIntClear = 12;
constexpr u32 kRawStatus = 16, kLock = 0xc00, kUnlock = 0x1acce551;
constexpr u32 kPeripheralId = 0x00141805;
enum class Status : u32 { Off, Active, Unsupported, Expired };
struct Probe {
    u32 id, control, load, value, raw_status;
    Status status;
};

template<class Bus> Probe probe(Bus& bus) {
    Probe result{};
    for (u32 byte = 0; byte < 4; ++byte) result.id |= (bus.read(0xfe0 + byte * 4) & 0xff) << (byte * 8);
    result.control = bus.read(kControl);
    result.load = bus.read(kLoad);
    result.value = bus.read(kValue);
    result.raw_status = bus.read(kRawStatus);
    if ((result.id & 0x00ffffff) != kPeripheralId || (result.control & ~3u) || !result.load)
        result.status = Status::Unsupported;
    else if (!(result.control & 1)) result.status = Status::Off;
    else if (result.raw_status & 1) result.status = Status::Expired;
    else result.status = Status::Active;
    return result;
}

// Match the vendor driver's ping: reload the existing timeout, preserve control,
// then relock and flush posted writes. Do not clear an already expired watchdog.
template<class Bus> bool keepalive(Bus& bus, const Probe& owner) {
    if (owner.status != Status::Active || bus.read(kControl) != owner.control ||
        bus.read(kLoad) != owner.load || (bus.read(kRawStatus) & 1)) return false;
    bus.write(kLock, kUnlock);
    bus.write(kLoad, owner.load);
    bus.write(kLock, 1);
    (void)bus.read(kLock);
    return true;
}
}
