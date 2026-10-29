#pragma once
#include "anko/types.h"

namespace platform {
constexpr u64 counter_frequency([[maybe_unused]] u64 reported) {
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    // Measured on ANE-LX1 and confirmed by the DTB; CNTFRQ reports only 16384.
    return 1920000;
#else
    return reported;
#endif
}
}
