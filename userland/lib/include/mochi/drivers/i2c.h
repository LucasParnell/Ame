#pragma once
#include "mochi/driver.h"
#include "midix/abi/protocols/i2c.h"
namespace mochi::i2c {
struct Client {
    SpscEndpoint endpoint;
    mx_u64_t sequence;
    mx_u16_t address;
    bool transaction(libk::const_bytes out, libk::bytes in) {
        if (out.size > 16 || in.size > 16 || (!out.size && !in.size)) {
            return false;
        }
        MxI2cTransaction request{
            ++sequence, address, static_cast<mx_u8_t>(out.size), static_cast<mx_u8_t>(in.size),
            0,          {}};
        if (!libk::copy_n(libk::as_bytes(request.data), out, out.size)) {
            return false;
        }
        const auto deadline = mx_clock_ns() + 100000000ull;
        if (!endpoint.tx().push_bounded(MX_I2C_TRANSACTION, &request, sizeof(request), deadline)) {
            return false;
        }
        MxRingSlot slot{};
        MxI2cReply reply{};
        while (driver::receive(endpoint, slot, deadline)) {
            if (message_get(slot, &reply, MX_I2C_REPLY) && reply.id == request.id) {
                return reply.status == MX_OK && reply.bytes == in.size &&
                       libk::copy_n(in, libk::as_bytes(reply.data), in.size);
            }
        }
        return false;
    }
    bool read(mx_u8_t reg, mx_u8_t& value) {
        return transaction(libk::as_bytes(&reg, 1), libk::as_bytes(&value, 1));
    }
    bool write(mx_u8_t reg, mx_u8_t value) {
        const mx_u8_t bytes[] = {reg, value};
        return transaction(libk::as_bytes(bytes), {});
    }
    bool update(mx_u8_t reg, mx_u8_t mask, mx_u8_t value) {
        mx_u8_t old = 0;
        return !(value & ~mask) && read(reg, old) && write(reg, (old & ~mask) | value);
    }
};
} // namespace mochi::i2c
