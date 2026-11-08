// Exercise the production IRQ routing entry point with real capability tables;
// only the final hardware installation and unrelated VM lookup are substituted.
#include "anko/vm.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {
void* slab_alloc(usize size, usize align) {
    if (align < sizeof(void*)) align = sizeof(void*);
    void* pointer = nullptr;
    return posix_memalign(&pointer, align, size) ? nullptr : pointer;
}
void slab_free(void* pointer, usize, usize) { std::free(pointer); }
}
namespace libk {
void* memset(void* target, int value, usize size) { return std::memset(target, value, size); }
}
[[noreturn]] void panic(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}
namespace vm {
static u32 installs;
void note_error() {}
void metric_inc(VmDebugCounter) {}
cap::Ref require_cap(Process& caller, CapHandle handle, CapType type, u32 rights, SysError* error) {
    auto entry = cap::lookup(*caller.cap_table, handle, type);
    if (!entry || (entry->rights_mask & rights) != rights) {
        *error = entry ? E_PERM : E_NOTFOUND;
        return nullptr;
    }
    *error = E_OK;
    return entry;
}
cap::Borrow<Vcpu> vcpu_from_cap(Process& caller, CapHandle handle, u32 rights, SysError* error) {
    return cap::Borrow<Vcpu>(require_cap(caller, handle, CAP_VCPU, rights, error));
}
SysError device_irq_install(VmDeviceAssignment*, u32) { ++installs; return E_OK; }
}

int main() {
    CapTable table{};
    Process caller{};
    caller.cap_table = &table;
    vm::Vm owner{};
    owner.vm_id = 42;
    vm::Vcpu cpu{};
    cpu.owner_vm_id = owner.vm_id;
    cpu.vcpu_id = 1;
    vm::VmDeviceAssignment assignment{};
    assignment.owner_vm = &owner;
    assignment.state = vm::VM_DEV_ACTIVE;
    assignment.lifetime = object::create(&assignment, nullptr);
    const auto assigned = cap::alloc(table, CAP_VM_DEVICE, &assignment, RIGHT_VM_MAP);
    object::release(assignment.lifetime);
    const auto target = cap::alloc(table, CAP_VCPU, &cpu, RIGHT_VCPU_CONTROL);
    const auto inspected_cpu = cap::alloc(table, CAP_VCPU, &cpu, RIGHT_VCPU_INSPECT);
    IrqLine line{99, 0, 0, 0, 0};
    const auto inspected_irq = cap::alloc(table, CAP_IRQ, &line, RIGHT_READ | RIGHT_DUP);
    const auto controlled_irq = cap::alloc(table, CAP_IRQ, &line, RIGHT_WRITE);
    assert(assigned && target && inspected_cpu && inspected_irq && controlled_irq);

    assert(vm::device_route_irq(caller, assigned, inspected_irq, target, 45, 0) == E_PERM);
    assert(!assignment.irq_count && !vm::installs);
    assert(vm::device_route_irq(caller, assigned, controlled_irq, inspected_cpu, 45, 0) == E_PERM);
    assert(!assignment.irq_count && !vm::installs);
    cpu.owner_vm_id = owner.vm_id + 1;
    assert(vm::device_route_irq(caller, assigned, controlled_irq, target, 45, 0) == E_PERM);
    assert(!assignment.irq_count && !vm::installs);
    cpu.owner_vm_id = owner.vm_id;
    assert(vm::device_route_irq(caller, assigned, controlled_irq, target, 45, 1) == E_INVALID);
    assert(vm::device_route_irq(caller, assigned, controlled_irq, target, 16, 0) == E_INVALID);
    assert(!assignment.irq_count && !vm::installs);
    assert(vm::device_route_irq(caller, assigned, controlled_irq, target, 45, 0) == E_OK);
    assert(assignment.irq_count == 1 && vm::installs == 1);
    assert(assignment.irq[0].host_irq == 99 && assignment.irq[0].target == &cpu);
    cap::close(table);
    assert(!object::live_count());
    std::puts("VM device IRQ authority probe ok");
}
