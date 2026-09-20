// kernel/platform/hw_info.cc — global HwInfo definition.
//
// g_hw is pre-populated from the qemu-virt board fallback constants so the
// kernel has valid hardware addresses even if the DTB is absent or corrupt.
// platform::probe() overwrites these fields from the actual DTB.

#include "anko/hw_info.h"
#include "anko/platform.h"

// board_fallback.cc (compiled from boards/<ANKO_BOARD>/board_fallback.cc)
// provides get_board_fallback().
namespace platform { const HwInfo& get_board_fallback(); }

HwInfo g_hw = [] {
    const HwInfo& fb = platform::get_board_fallback();
    return fb;
}();
