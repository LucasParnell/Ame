// kernel/drivers/gic/irq_table.cc — IRQ → doorbell binding table and delivery.
//
// Spec ref: §5.10 IRQ and Timer, §18 Masking-at-Entry
//
// The table is a fixed array of kIrqSlots slots indexed by `irq_id & (kIrqSlots-1)`.
// Every entry point masks the untrusted id before touching the array and then
// rejects aliases (sentinel/PPI/SGI/out-of-range), so the speculative path can
// never leave the array.
//
// Locking: a single spinlock (IRQs masked) guards the table. bind/ack/unbind and
// delivery all take it; delivery holds it across ipc::notify_object so the bound
// doorbell and the table's reference on it cannot be torn down mid-notify. The
// critical section is short (a slot compare plus a counter/fast wake).

#include "anko/irq.h"
#include "anko/mali.h"
#include "anko/ipc.h"
#include "anko/object.h"
#include "anko/mm.h"
#include "anko/spinlock.h"

namespace irq {

namespace {

constexpr u32 kFree = 0xFFFF'FFFFu;
constexpr u32 kTriggerLevel = 0;

struct Slot {
    Doorbell* doorbell;     // bound doorbell, or null
    object::Record* record; // the table's reference on `doorbell`
    u64 owner;              // binding process identity
    u32 irq_id;             // exact id — guards against masked aliasing
    u32 trigger;            // 0 = level, 1 = edge (mirrors IrqLine::trigger)
    u32 active;             // 0 = free, 1 = bound, 2 = level, awaiting ack
    u32 flags;
    u64 deliveries;
};

Slot g_slots[kIrqSlots]{};
kernel::SpinLock g_lock{};
u64 g_delivered = 0;
u64 g_bindings = 0;
u64 g_selftest = 0;
DeliverHook g_deliver_hook = nullptr;

void zero_bytes(void* p, usize n) {
    auto* b = static_cast<u8*>(p);
    for (usize i = 0; i < n; ++i) {
        b[i] = 0;
    }
}

void destroy_test_doorbell(void* target) {
    mm::slab_free(target, sizeof(Doorbell), alignof(Doorbell));
}

// Release a slot's shared body. Caller holds g_lock; the doorbell reference is
// returned so it can be dropped after the lock is released.
object::Record* take_slot_locked(Slot& s) {
    drivers::gic::spi_disable(s.irq_id);
    object::Record* rec = s.record;
    s.record = nullptr;
    s.owner = 0;
    s.irq_id = kFree;
    s.trigger = 0;
    s.flags = 0;
    s.active = 0;
    s.doorbell = nullptr;
    __atomic_sub_fetch(&g_bindings, 1, __ATOMIC_RELAXED);
    return rec;
}

} // namespace

SysError bind(IrqLine* line, Doorbell* doorbell, u64 owner, u32 flags) {
    if (!line || !doorbell) {
        return E_INVALID;
    }
    if (flags != 0) {
        return E_INVALID; // reserved
    }
    const u32 irq_id = line->irq_id;
    if (!irq_bindable(irq_id)) {
        return E_INVALID;
    }
    const u32 idx = irq_index(irq_id); // mask before any table access

    kernel::LockGuard guard(g_lock);
    Slot& s = g_slots[idx];
    if (s.active) {
        // Idempotent for the same binder; a different one loses the line.
        if (s.doorbell == doorbell && s.owner == owner && s.irq_id == irq_id) {
            return E_OK;
        }
        return E_BUSY;
    }
    object::Record* rec = object::acquire(doorbell);
    if (!rec) {
        return E_NOMEM;
    }

    s.record = rec;
    s.owner = owner;
    s.irq_id = irq_id;
    s.trigger = line->trigger;
    s.flags = flags;
    s.deliveries = 0;
    s.doorbell = doorbell;

    drivers::gic::spi_configure(irq_id, line->trigger, /*target_cpu=*/0);
    drivers::gic::spi_enable(irq_id);
    s.active = 1;
    __atomic_add_fetch(&g_bindings, 1, __ATOMIC_RELAXED);
    return E_OK;
}

SysError ack(IrqLine* line, u64 owner) {
    if (!line) {
        return E_INVALID;
    }
    const u32 irq_id = line->irq_id;
    if (!irq_bindable(irq_id)) {
        return E_INVALID;
    }
    const u32 idx = irq_index(irq_id);

    kernel::LockGuard guard(g_lock);
    Slot& s = g_slots[idx];
    if (!s.active || s.irq_id != irq_id || !s.doorbell) {
        return E_OK; // nothing bound
    }
    if (s.owner != owner) {
        return E_PERM;
    }
    if (s.trigger == kTriggerLevel) {
        // Clear the latched pending bit and re-enable the line; a still-asserted
        // source re-pends and is delivered again.
        drivers::gic::spi_clear_pending(irq_id);
        drivers::gic::spi_enable(irq_id);
    }
    s.active = 1;
    return E_OK;
}

void unbind_line(IrqLine* line, u64 owner) {
    if (!line) {
        return;
    }
    const u32 irq_id = line->irq_id;
    if (!irq_bindable(irq_id)) {
        return;
    }
    const u32 idx = irq_index(irq_id);

    object::Record* rec = nullptr;
    {
        kernel::LockGuard guard(g_lock);
        Slot& s = g_slots[idx];
        if (!s.active || s.irq_id != irq_id) {
            return;
        }
        if (owner != 0 && s.owner != owner) {
            return; // not the binder
        }
        rec = take_slot_locked(s);
    }
    if (rec) {
        object::release(rec);
    }
}

void unbind_owner(u64 owner) {
    if (owner == 0) {
        return;
    }
    for (u32 idx = 0; idx < kIrqSlots; ++idx) {
        object::Record* rec = nullptr;
        {
            kernel::LockGuard guard(g_lock);
            Slot& s = g_slots[idx];
            if (!s.active || s.owner != owner) {
                continue;
            }
            rec = take_slot_locked(s);
        }
        if (rec) {
            object::release(rec);
        }
    }
}

void deliver(u32 irq_id) {
    if (!irq_bindable(irq_id)) {
        return; // timer/IPI/PPIs/SGIs are never redirected
    }

    // Second-chance hook: the VM device-assignment layer routes assigned-device
    // host lines into the guest vGIC. It is independent of the doorbell table,
    // so it runs even when no driver holds a binding on this line.
#ifndef ANKO_HOST_TEST
    mali::deliver(irq_id);
#endif
    if (g_deliver_hook) {
        g_deliver_hook(irq_id);
    }

    const u32 idx = irq_index(irq_id);

    kernel::LockGuard guard(g_lock);
    Slot& s = g_slots[idx];
    if (!s.active || s.irq_id != irq_id || !s.doorbell) {
        return;
    }
    if (s.trigger == kTriggerLevel) {
        // Pause the line until the driver acks so it cannot storm the CPU.
        drivers::gic::spi_disable(irq_id);
        s.active = 2;
    }
    __atomic_add_fetch(&s.deliveries, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_delivered, 1, __ATOMIC_RELAXED);
    (void)ipc::notify_object(s.doorbell);
}

u64 delivered_count() {
    return __atomic_load_n(&g_delivered, __ATOMIC_RELAXED);
}
u64 binding_count() {
    return __atomic_load_n(&g_bindings, __ATOMIC_RELAXED);
}
u64 selftest_status() {
    return __atomic_load_n(&g_selftest, __ATOMIC_RELAXED);
}

void set_deliver_hook(DeliverHook hook) {
    __atomic_store_n(&g_deliver_hook, hook, __ATOMIC_RELEASE);
}

u64 run_selftest() {
    constexpr u32 kTestIrq = 100; // spare SPI, wired to nothing on either board
    u64 status = 2;
    if (irq_bindable(kTestIrq)) {
        auto* db = static_cast<Doorbell*>(mm::slab_alloc(sizeof(Doorbell), alignof(Doorbell)));
        if (db) {
            zero_bytes(db, sizeof(*db));
            object::Record* rec = object::create(db, &destroy_test_doorbell);
            if (rec) {
                IrqLine line{};
                line.irq_id = kTestIrq;
                line.trigger = 1; // edge: no acks needed for the check
                if (bind(&line, db, /*owner=*/0, 0) == E_OK) {
                    const u64 before = delivered_count();
                    drivers::gic::spi_set_pending(kTestIrq);
                    const u32 id = drivers::gic::iar_id();
                    if (id == kTestIrq) {
                        deliver(id); // the exact path the IRQ vector uses
                        drivers::gic::eoi(id);
                        const u64 delivered = delivered_count();
                        const u64 wakes = __atomic_load_n(&db->counter, __ATOMIC_RELAXED);
                        if (delivered == before + 1 && wakes == 1) {
                            status = 1;
                        }
                    }
                    drivers::gic::spi_clear_pending(kTestIrq);
                    unbind_line(&line, 0);
                }
                object::release(rec);
            } else {
                mm::slab_free(db, sizeof(Doorbell), alignof(Doorbell));
            }
        }
    }
    __atomic_store_n(&g_selftest, status, __ATOMIC_RELEASE);
    return status;
}

} // namespace irq
