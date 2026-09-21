#include "anko/vm.h"
#include "midix/abi/dango_channel_guest.h"

namespace vm {
// CallGuard protects channel lifetime and serializes publication against ARM.
static void wake_guest(VmChannel& channel) {
    channel.armed = false;
    vgic_pending_set(*channel.target, channel.guest_irq); // edge, no physical route
    vcpu_wake_irq(*channel.target);
}

void channel_publish(VmChannel& channel) {
    ++channel.messages_host_to_guest;
    if (channel.armed) wake_guest(channel);
}

bool channel_hvc(Vcpu& cpu, Vm& owner) {
    const u64 fn = cpu.el2.regs[0];
    if (fn != DANGO_CH_HVC_NOTIFY && fn != DANGO_CH_HVC_ARM) return false;
    // HVC is only a doorbell, never authority to map pages or select another VM.
    VmChannel* channel = nullptr;
    for (u32 i = 0; i < owner.channel_count; ++i) {
        auto* candidate = owner.channels[i];
        if (candidate && !candidate->dead && candidate->target == &cpu &&
            candidate->channel_id == cpu.el2.regs[1]) { channel = candidate; break; }
    }
    i64 result = E_NOTFOUND;
    if (channel) {
        if (fn == DANGO_CH_HVC_NOTIFY) {
            if (cpu.el2.regs[2] != 2) result = E_INVALID; // RING_PUBLISH
            else {
                ++channel->messages_guest_to_host;
                result = ipc::notify_object(channel->guest_doorbell);
            }
        } else {
            // Arm, then recheck the producer head. CallGuard serializes this
            // with host notification, closing the empty -> sleep race even if
            // publication precedes ARM. The guest never supplies a host pointer.
            channel->armed = true;
            const auto* header = reinterpret_cast<const RingHeader*>(channel->host_to_guest->phys_base);
            if (__atomic_load_n(&header->head.head, __ATOMIC_ACQUIRE) != cpu.el2.regs[2])
                wake_guest(*channel);
            result = E_OK;
        }
    }
    cpu.el2.regs[0] = static_cast<u64>(result);
    // ELR_EL2 already points past HVC. Do not advance it a second time.
    return true;
}

} // namespace vm
