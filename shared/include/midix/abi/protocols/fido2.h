#pragma once
#include "midix/abi/types.h"

// Private gadget -> init lifecycle channel. It cannot name arbitrary services.
constexpr mx_u32_t MX_FIDO2_DEMAND = 0x46494401;
struct MxFido2Demand {
    mx_u64_t generation;
    mx_u32_t active;
    mx_u32_t reserved;
};

// A per-device, host-generated seed delivered by a private boot capability.
// It is never placed in the userland bundle or exposed over USB.
constexpr mx_u32_t MX_FIDO2_SEED_VERSION = 1;
struct MxFido2Seed {
    mx_u32_t version;
    mx_u32_t provisioned;
    mx_u8_t bytes[32];
};

// Dedicated authenticator -> tty channel; USB bytes never approve a request.
constexpr mx_u32_t MX_FIDO2_PROMPT = 0x46494402;
enum MxFido2PromptState : mx_u8_t {
    MX_FIDO2_WAITING = 1, MX_FIDO2_APPROVED = 2, MX_FIDO2_CANCELLED = 3,
    MX_FIDO2_EXPIRED = 4, MX_FIDO2_REJECTED = 5, MX_FIDO2_SELECTED = 6,
};
constexpr mx_u8_t MX_FIDO2_SELECT = 3;
constexpr mx_u64_t MX_FIDO2_PRESENCE_NS = 30'000'000'000ull;
struct MxFido2Prompt {
    mx_u64_t generation, request;
    mx_u16_t offset, length, total;
    mx_u8_t operation, state;
    mx_u8_t rp[24];
};
static_assert(sizeof(MxFido2Prompt) == 48);
