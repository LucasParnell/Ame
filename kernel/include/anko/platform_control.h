#pragma once
#include "anko/cap.h"
#include "anko/syscall.h"
namespace platform {
enum class Domain : u32 { UsbPhy, UsbController, I2c, Display, System };
struct Control {
    Domain domain;
    u32 reserved;
    u64 owner;
};
SysError control(Control&, u32 operation, u64 argument, u64 identity);
void fence_owner(u64 identity);
Control* board_control(Domain);
} // namespace platform
