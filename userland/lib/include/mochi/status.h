#pragma once
#include "midix/abi/status.h"

namespace mochi {

struct Status {
    mx_status_t code;
    constexpr bool ok() const { return code >= 0; }
    constexpr bool error() const { return code < 0; }
    constexpr explicit operator bool() const { return ok(); }
};

constexpr Status Ok{MX_OK};

} // namespace mochi
