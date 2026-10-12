// kernel/platform/hw_info.cc — global HwInfo definition.
//
// g_hw is pre-populated from the qemu-virt board fallback constants so the
// kernel has valid hardware addresses even if the DTB is absent or corrupt.
// platform::probe() overwrites these fields from the actual DTB.

#include "anko/hw_info.h"
#include "anko/platform.h"

HwInfo g_hw = [] {
    const HwInfo& fb = platform::get_board_fallback();
    return fb;
}();
