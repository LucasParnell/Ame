#pragma once
#include "midix/abi/types.h"
#include "libk/bytes.h"

namespace mochi::hi6523 {
// Preserve the sink owner's charge-enable bit across an OTG source session.
// Never invent a charge-enable setting when firmware left charging disabled.
struct ChargeHandoff {
    bool active = false, restore = false;
    template <typename Bus> bool suspend(Bus& bus) {
        if (active)
            return true;
        mx_u8_t config = 0;
        if (!bus.read(0x90, config))
            return false;
        restore = (config & 0x20) != 0;
        if (!bus.update(0x90, 0x20, 0))
            return false;
        active = true;
        return true;
    }
    // Caller must successfully disable boost before invoking resume.
    template <typename Bus> bool resume(Bus& bus) {
        if (!active)
            return true;
        if (restore && !bus.update(0x90, 0x20, 0x20))
            return false;
        active = false;
        restore = false;
        return true;
    }
};
enum class SourceFailure : mx_u8_t { None, Read, BoostStopped, HardFault, Clear, Feed };
struct SourceHealth {
    mx_u8_t status = 0, faults = 0;
    SourceFailure failure = SourceFailure::None;
    bool ok() const {
        return failure == SourceFailure::None;
    }
};

// STATUS0..IRQ0 are contiguous. One snapshot avoids separate service round
// trips for the live OTG state and latched faults. IRQ0 is write-one-to-clear.
// IRQ0 is history, not an active-fault register. Once after boost startup,
// acknowledge recovered OCP/UVP/SCP only while OTG_ON is still healthy. The
// vendor logs/clears OCP and checks live OTG_ON for UVP/SCP. Restrict recovery
// here to the first health sample: a recurring fault, stopped output, OVP,
// overtemperature or failed I2C operation always fails closed.
template <typename Bus> SourceHealth maintain_source(Bus& bus, bool startup = false) {
    SourceHealth result;
    const mx_u8_t first = 0x06;
    mx_u8_t snapshot[7]{};
    if (!bus.transaction(libk::as_bytes(&first, 1), libk::as_bytes(snapshot))) {
        result.failure = SourceFailure::Read;
        return result;
    }
    result.status = snapshot[0];
    result.faults = snapshot[6];
    if (!(result.status & 0x40))
        result.failure = SourceFailure::BoostStopped;
    else if ((result.faults & 0x03) || (!startup && (result.faults & 0x1c)))
        result.failure = SourceFailure::HardFault;
    else if ((result.faults & 0x1c) && !bus.write(0x0c, result.faults & 0x1c))
        result.failure = SourceFailure::Clear;
    else if (!bus.write(0xe8, 1))
        result.failure = SourceFailure::Feed;
    return result;
}
} // namespace mochi::hi6523
