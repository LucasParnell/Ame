#pragma once
#include "anko/types.h"
#include "anko/cap.h"
#include "anko/syscall.h"
struct Process;
namespace vm {
struct Vm;
struct Vcpu;
} // namespace vm
namespace mali {
SysError attach(Process&, CapHandle vm, CapHandle vcpu, CapHandle authority);
SysError performance(u32 mhz); // zero reads the firmware OPP; nonzero requests a bounded OPP
bool owns(const vm::Vm&);
// False pins the entire VM after a reset timeout; it must not be reclaimed.
bool release(vm::Vm&);
bool emulate(vm::Vm&, u64 ipa, u32 size, bool write, u64* value);
void deliver(u32 host_irq);
void guest_eoi(vm::Vcpu&, u32 guest_irq);
} // namespace mali
