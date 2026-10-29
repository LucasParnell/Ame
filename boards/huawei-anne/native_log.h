#pragma once
#include "anko/types.h"

namespace anne {
bool native_log_layout(PhysAddr dtb, PhysAddr* console_base);
// No memory access unless the runtime DTB matches the captured ramoops layout.
bool native_log_init(PhysAddr dtb);
void native_log_putchar(char c);
}
