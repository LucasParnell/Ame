#pragma once
#include "anko/power.h"
namespace anne::dvfs {
bool init(PhysAddr dtb);
SysError request(u32 cluster, u32 khz, u32 index, u64 now);
void statistics(u32 cluster, u32& transitions, u32& failures);
} // namespace anne::dvfs
