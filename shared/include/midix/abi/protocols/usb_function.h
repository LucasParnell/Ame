#pragma once
#include "midix/abi/protocols/usb_transfer.h"
#include "midix/abi/status.h"
constexpr mx_u32_t MX_FUNCTION_SETUP = 0x55464e01, MX_FUNCTION_SETUP_REPLY = 0x55464e02;
constexpr mx_u32_t MX_FUNCTION_RX = 0x55464e03, MX_FUNCTION_TX = 0x55464e04,
                   MX_FUNCTION_RESET = 0x55464e05;
// RESET revokes the previous function incarnation. CONFIGURED permits new
// byte traffic; clear means reset/disconnect/unconfigured, not a live stream.
// Function generations change on every configuration/lifecycle boundary and
// are distinct from the controller packet generation. Other flag bits are zero.
constexpr mx_u8_t MX_FUNCTION_CONFIGURED = 1u << 0;
constexpr mx_u32_t MX_FUNCTION_INLINE_BYTES = 32;
struct MxFunctionSetup {
    mx_u64_t id, generation;
    MxUsbSetup setup;
    mx_i32_t status;
    mx_u32_t length;
    mx_u8_t data[16];
};
struct MxFunctionChunk {
    mx_u64_t generation;
    mx_u32_t sequence;
    mx_u16_t length;
    mx_u8_t endpoint, flags;
    mx_u8_t data[32];
};
static_assert(sizeof(MxFunctionSetup) == MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxFunctionChunk) == MX_RING_INLINE_PAYLOAD);
