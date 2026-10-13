#pragma once
#include "midix/abi/ring.h"
#include "midix/abi/status.h"
// One bounded repeated-start transaction. Each signed client row owns an
// address lease; a client cannot select a different chip on the same bus.
constexpr mx_u32_t MX_I2C_TRANSACTION = 0x49324331, MX_I2C_REPLY = 0x49324332;
struct MxI2cTransaction {
    mx_u64_t id;
    mx_u16_t address;
    mx_u8_t write_bytes, read_bytes;
    mx_u32_t reserved;
    mx_u8_t data[16];
};
struct MxI2cReply {
    mx_u64_t id;
    mx_status_t status;
    mx_u32_t bytes;
    mx_u8_t data[16];
};
static_assert(sizeof(MxI2cTransaction) <= MX_RING_INLINE_PAYLOAD);
