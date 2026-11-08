#pragma once
#include "anko/cap.h"
#include "anko/syscall.h"
struct Arena;
namespace platform {
enum class Domain : u32 { UsbPhy, UsbController, I2c, Display, System, Emmc, Gpu, Charger, Health };
struct Control {
    Domain domain;
    u32 reserved;
    u64 owner;
};
SysError control(Control &, u32 operation, u64 argument, u64 identity);
void fence_owner(u64 identity);
Control *board_control(Domain);
// A retained capability lookup validates type, rights, generation and target.
inline bool can_observe_health(CapTable &table, CapHandle handle) {
    auto entry = cap::lookup(table, handle, CAP_PLATFORM);
    return entry && (entry->rights_mask & RIGHT_READ) &&
           entry->target == board_control(Domain::Health);
}
SysError display_bind(Control &, Arena &, object::Record *, u32 slot, u64 identity);
} // namespace platform
