#include "anko/vm.h"
#include "midix/abi/dango_channel_guest.h"
#include <cassert>

static unsigned injections, wakes, notifications;
namespace vm {
void vgic_pending_set(Vcpu&, u32 irq) {
    assert(irq == 48);
    ++injections;
}
void vcpu_wake_irq(Vcpu&) { ++wakes; }
}
namespace ipc {
SysError notify_object(Doorbell* db) { assert(db); ++notifications; return E_OK; }
}

int main() {
    static vm::Vm owner{};
    vm::Vcpu cpu{}, other_cpu{};
    RingHeader h2g{};
    Ring ring{};
    Doorbell db{};
    vm::VmChannel channel{};
    ring.phys_base = reinterpret_cast<PhysAddr>(&h2g);
    channel.channel_id = 7;
    channel.target = &cpu;
    channel.guest_irq = 48;
    channel.host_to_guest = &ring;
    channel.guest_doorbell = &db;
    owner.channels[0] = &channel;
    owner.channel_count = 1;
    auto call = [&](u64 fn, u64 id, u64 arg) {
        cpu.el2.regs[0] = fn; cpu.el2.regs[1] = id; cpu.el2.regs[2] = arg;
        cpu.el2.pc = 0x1234;
        assert(vm::channel_hvc(cpu, owner));
        assert(cpu.el2.pc == 0x1234); // HVC return PC must not be advanced twice
        return static_cast<i64>(cpu.el2.regs[0]);
    };
    cpu.el2.regs[0] = 0xc4000003; // PSCI belongs to the existing handler
    assert(!vm::channel_hvc(cpu, owner));
    assert(call(DANGO_CH_HVC_NOTIFY, 8, 2) < 0 && !notifications);
    assert(call(DANGO_CH_HVC_NOTIFY, 7, 99) < 0 && !notifications);
    channel.target = &other_cpu;
    assert(call(DANGO_CH_HVC_ARM, 7, 0) < 0 && !channel.armed);
    channel.target = &cpu;
    channel.dead = true;
    assert(call(DANGO_CH_HVC_NOTIFY, 7, 2) < 0 && !notifications);
    channel.dead = false;
    owner.channel_count = 0;
    assert(call(DANGO_CH_HVC_NOTIFY, 7, 2) < 0 && !notifications);
    owner.channel_count = 1;

    // Arm before publication: exactly one IRQ, subsequent notifications coalesce.
    assert(call(DANGO_CH_HVC_ARM, 7, 0) == 0 && channel.armed);
    h2g.head.head = 1;
    vm::channel_publish(channel);
    assert(!channel.armed && injections == 1 && wakes == 1);
    vm::channel_publish(channel);
    assert(injections == 1);
    // Publication before ARM must also wake, including counter wraparound.
    h2g.head.head = 0;
    assert(call(DANGO_CH_HVC_ARM, 7, ~0ull) == 0);
    assert(injections == 2 && !channel.armed);
    assert(call(DANGO_CH_HVC_ARM, 7, 0) == 0 && channel.armed);
    assert(call(DANGO_CH_HVC_NOTIFY, 7, 2) == 0);
    assert(notifications == 1 && channel.messages_guest_to_host == 1);
    assert(channel.messages_host_to_guest == 2);
}
