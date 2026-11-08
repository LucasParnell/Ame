#pragma once
#include "anko/types.h"
namespace security {
bool random_ready();
bool random_trusted(); // false for the explicitly enabled uptime fallback
bool random_bytes(void*, usize);
u64 random_below(u64 bound);
// Runs before constructors, MMU setup and secondary CPU startup.
bool seed_boot_random(PhysAddr dtb);
}
