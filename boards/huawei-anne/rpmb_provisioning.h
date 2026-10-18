#pragma once
#include "rpmb_diagnostics.h"

namespace anne::rpmb_diag {
struct ProvisionResult {
    u32 result = MX_RPMB_PROVISION_FAILED;
    u64 stage = MX_RPMB_PROVISION_PREFLIGHT;
    u64 smc_result{}, transport{}, response_result{};
    rpmb::KeyInfo key{};
    bool pending{};
};
// Caller owns the registered shared window exclusively and enables IRQs.
// FF05 prepares a key frame. Provisioning itself does not call FF06 or invent
// a key/HMAC. The real card's one-time PROGRAM_KEY command is issued at most once.
ProvisionResult provision(volatile u8* request, const Storage& storage, Smc call);
}
