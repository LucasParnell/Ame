#pragma once
#include "midix/abi/types.h"

constexpr mx_u32_t MX_PROTOCOL_TERMINAL = 0x5445524Du; // 'TERM'
constexpr mx_u16_t MX_TERM_VERSION = 1;

enum MxTermOpcode : mx_u32_t {
    MX_TERM_INPUT_BYTES  = 0x54494E01u,
    MX_TERM_OUTPUT_BYTES = 0x544F5501u,
    MX_TERM_RESIZE       = 0x54525301u,
    MX_TERM_CONTROL      = 0x54435401u,
};
