// kernel/vm/vm_device_irq.cc — host IRQ → guest vGIC routing (Phase D, §7).
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §7.1 (requirement), §7.2 (route
// object), §7.3 (MSI/MSI-X primary path), §7.4 (level/edge compatibility).
//
// Steady-state route:
//
//   QEMU / physical GPU  ->  physical GIC / host IRQ domain
//                        ->  Anko assignment route (this file)
//                        ->  vGIC pending bit for the appliance vCPU
//                        ->  Linux GPU driver ISR
//
// A route binds host IRQ line -> VM -> vCPU target -> guest IRQ number ->
// trigger semantics. Delivery is a pending bit in the target vCPU's VgicState,
// which el2.S exposes through HCR_EL2.VI; a raw host MSI target is never
// programmed. Level routes suppress repeated host delivery after injecting the
// virtual IRQ and re-arm it on the guest's EOI (§7.4).

#include "anko/vm.h"

#include "anko/irq.h"
#include "anko/log.h"
#include "anko/spinlock.h"
#include "libk/string.h"

namespace vm {

namespace {

constexpr u32 kRouteSlots = kMaxVmDevices * kMaxAssignIrqs;

struct RouteSlot {
    u32 host_irq;
    u32 route_index;
    VmDeviceAssignment* owner;
    bool used;
    u8 pad[3];
};

RouteSlot g_routes[kRouteSlots];
kernel::SpinLock g_route_lock;
u64 g_route_count;
bool g_hook_installed;

RouteSlot* find_by_host_locked(u32 host_irq, u32 skip_index) {
    for (u32 i = 0; i < kRouteSlots; ++i) {
        if (!g_routes[i].used || g_routes[i].host_irq != host_irq) {
            continue;
        }
        if (i == skip_index) {
            continue;
        }
        return &g_routes[i];
    }
    return nullptr;
}

} // namespace

void device_irq_init() {
    if (g_hook_installed) {
        return;
    }
    // The IRQ vector consults this hook for every bindable SPI, so an assigned
    // device's host line reaches its route even with no driver doorbell bound.
    irq::set_deliver_hook(&device_irq_deliver);
    g_hook_installed = true;
    ANKO_LOG_INFO("kernel/vm", "device IRQ routes initialised");
}

bool device_irq_vector_used(const Vcpu& vcpu, u32 guest_irq) {
    kernel::LockGuard guard(g_route_lock);
    for (const auto& slot : g_routes) {
        if (!slot.used) continue;
        const auto& route = slot.owner->irq[slot.route_index];
        if (route.target == &vcpu && route.guest_irq == guest_irq) return true;
    }
    return false;
}

SysError device_irq_install(VmDeviceAssignment* a, u32 route_index) {
    if (!a || route_index >= a->irq_count) {
        return E_INVALID;
    }
    VmAssignIrq& route = a->irq[route_index];
    if (route.installed) {
        return E_OK; // idempotent
    }
    if (!irq::irq_bindable(route.host_irq)) {
        // A host line that is not a programmable SPI cannot be routed; this is
        // a hard rejection, not a silent fallback.  Log the id so a mis-derived
        // INTx (wrong host interrupt-map parse) is immediately visible.
        ANKO_LOGF_ERROR("kernel/vm", "assign: host irq %u is not a bindable SPI",
                        route.host_irq);
        return E_INVALID;
    }
    // A channel owns its edge-triggered vector exclusively. Sharing it with
    // a physical level route would corrupt EOI/re-arm semantics.
    if (auto* owner = owner_for_vcpu(route.target)) {
        for (u32 i = 0; i < owner->channel_count; ++i) {
            const auto* ch = owner->channels[i];
            if (ch && ch->target == route.target && ch->guest_irq == route.guest_irq) return E_BUSY;
        }
    }

    kernel::LockGuard guard(g_route_lock);
    if (find_by_host_locked(route.host_irq, kRouteSlots) != nullptr) {
        return E_BUSY; // the line already belongs to another active route
    }
    for (u32 i = 0; i < kRouteSlots; ++i) {
        if (g_routes[i].used) {
            continue;
        }
        g_routes[i].used = true;
        g_routes[i].host_irq = route.host_irq;
        g_routes[i].route_index = route_index;
        g_routes[i].owner = a;
        route.installed = true;
        route.suppressed = false;
        __atomic_add_fetch(&g_route_count, 1, __ATOMIC_RELAXED);
        break;
    }
    if (!route.installed) {
        return E_OVERFLOW;
    }
    // Program and enable the host line. A level line is delivered and then
    // masked per delivery; an edge line is level-agnostic here.
    drivers::gic::spi_configure(route.host_irq, route.trigger, /*target_cpu=*/0);
    drivers::gic::spi_enable(route.host_irq);
    return E_OK;
}

void device_irq_uninstall(VmDeviceAssignment* a, u32 route_index) {
    if (!a || route_index >= a->irq_count) {
        return;
    }
    VmAssignIrq& route = a->irq[route_index];
    if (!route.installed) {
        return;
    }
    {
        kernel::LockGuard guard(g_route_lock);
        for (u32 i = 0; i < kRouteSlots; ++i) {
            if (!g_routes[i].used) {
                continue;
            }
            if (g_routes[i].owner == a && g_routes[i].route_index == route_index) {
                g_routes[i].used = false;
                g_routes[i].owner = nullptr;
                g_routes[i].host_irq = 0;
                if (__atomic_load_n(&g_route_count, __ATOMIC_RELAXED) != 0) {
                    __atomic_sub_fetch(&g_route_count, 1, __ATOMIC_RELAXED);
                }
                break;
            }
        }
    }
    // Mask the host line before the function can be detached or its memory
    // reused (§7.3). The route is unreachable from the vector from here on.
    drivers::gic::spi_disable(route.host_irq);
    route.installed = false;
    route.suppressed = false;
}

void device_irq_inject(Vcpu& vcpu, u32 guest_irq, u32 trigger) {
    vgic_pending_set(vcpu, guest_irq);
    (void)trigger;
    metric_inc(VM_DEBUG_VM_DEVICE_IRQ_INJECTED);
    vcpu_wake_irq(vcpu);
}

void device_irq_deliver(u32 host_irq) {
    // Keep the route and target alive until injection completes. Teardown takes
    // this same lock before removing a slot and releasing retained objects.
    kernel::LockGuard guard(g_route_lock);
    RouteSlot* slot = find_by_host_locked(host_irq, kRouteSlots);
    if (!slot || !slot->owner || slot->route_index >= slot->owner->irq_count) return;
    VmDeviceAssignment* owner = slot->owner;
    const u32 route_index = slot->route_index;
    VmAssignIrq& route = owner->irq[route_index];
    if (!route.target) {
        return;
    }
    if (route.trigger == 0) {
        // Level: suppress-after-inject. The line is masked on delivery and the
        // guest's EOI re-arms it, so a still-asserted source cannot storm.
        if (route.suppressed) {
            return;
        }
        route.suppressed = true;
        drivers::gic::spi_disable(host_irq);
    }
    __atomic_add_fetch(&route.deliveries, 1, __ATOMIC_RELAXED);
    device_irq_inject(*route.target, route.guest_irq, route.trigger);
}

void device_irq_on_guest_eoi(Vcpu& vcpu, u32 guest_irq) {
    kernel::LockGuard guard(g_route_lock);
    for (u32 i = 0; i < kRouteSlots; ++i) {
        if (!g_routes[i].used) {
            continue;
        }
        VmDeviceAssignment* a = g_routes[i].owner;
        if (!a || g_routes[i].route_index >= a->irq_count) {
            continue;
        }
        VmAssignIrq& route = a->irq[g_routes[i].route_index];
        if (route.target != &vcpu || route.guest_irq != guest_irq) {
            continue;
        }
        if (route.trigger == 0 && route.suppressed) {
            route.suppressed = false;
            // Re-enable delivery now the guest has serviced the source.
            drivers::gic::spi_enable(route.host_irq);
        }
    }
}

u64 device_irq_route_count() {
    return __atomic_load_n(&g_route_count, __ATOMIC_RELAXED);
}

} // namespace vm
