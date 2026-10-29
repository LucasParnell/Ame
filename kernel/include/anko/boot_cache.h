#pragma once
#include "anko/types.h"

namespace mm {

// Temporary identity mapping for the sole boot CPU's DT transform. Only the
// kernel image/stack and immutable DTB become cacheable; peripherals retain
// Device attributes. Destruction publishes dirty kernel data before returning
// to MMU-off boot. Unsupported extents simply use the existing uncached path.
class BootDtbCache {
public:
    explicit BootDtbCache(PhysAddr dtb);
    ~BootDtbCache();
    BootDtbCache(const BootDtbCache&) = delete;
    BootDtbCache& operator=(const BootDtbCache&) = delete;
private:
    bool active_ = false;
};

} // namespace mm
