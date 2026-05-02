// kernel/ipc/doorbell.cc — doorbell: notify and wait_doorbell.
//
// Spec ref: §5.9 Doorbells
//
// V1 single-core correctness rule:
//   wait_doorbell() and notify() share a per-doorbell lock while touching
//   counter, waiter, and thread state. Cross-core wake routes through the
//   target CPU pending-wake queue rather than touching its runqueue directly.

#include "anko/ipc.h"
#include "anko/sched.h"
#include "anko/cap.h"
#include "anko/mm.h"
#include "libk/string.h"

namespace ipc {

static u64 g_sleep_count = 0;
static u64 g_wake_count = 0;
static u64 g_wait_calls = 0;
static u64 g_wait_immediate = 0;
static u64 g_notify_calls = 0;
static u64 g_notify_no_waiter = 0;
static u64 g_notify_woke_thread = 0;
static u64 g_doorbell_local_wakes = 0;
static u64 g_doorbell_remote_wakes = 0;

static inline void metric_inc(u64& counter) {
    __atomic_add_fetch(&counter, 1ull, __ATOMIC_RELAXED);
}

static inline u64 metric_load(const u64& counter) {
    return __atomic_load_n(&counter, __ATOMIC_RELAXED);
}

static inline u64 local_irq_save_disable() {
    u64 daif = 0;
    asm volatile("mrs %0, DAIF" : "=r"(daif) :: "memory");
    asm volatile("msr DAIFSet, #2" ::: "memory"); // mask IRQ only
    return daif;
}

static inline void local_irq_restore(u64 daif) {
    asm volatile("msr DAIF, %0" :: "r"(daif) : "memory");
}

static void lock(Doorbell* db) {
    while (__atomic_exchange_n(&db->lock, 1u, __ATOMIC_ACQUIRE) != 0) {
        asm volatile("yield" ::: "memory");
    }
}

static void unlock(Doorbell* db) {
    __atomic_store_n(&db->lock, 0u, __ATOMIC_RELEASE);
}

static CapEntry* resolve_doorbell_entry(u32 pid, CapHandle handle, u32 need_rights) {
    Process* p = sched::find_process(pid);
    if (!p || !p->cap_table) return nullptr;

    CapEntry* e = cap::lookup(*p->cap_table, handle, CAP_DOORBELL);
    if (!e) return nullptr;
    if ((e->rights_mask & need_rights) != need_rights) return nullptr;
    return e;
}

Doorbell* doorbell_create_object(u32 pid) {
    Process* owner = sched::find_process(pid);
    if (!owner || !owner->cap_table) return nullptr;

    Doorbell* db = static_cast<Doorbell*>(mm::heap_alloc(sizeof(Doorbell), alignof(Doorbell)));
    if (!db) return nullptr;
    libk::memset(db, 0, sizeof(*db));
    db->owner_pid = pid;
    db->rights = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;

    return db;
}

CapHandle doorbell_cap_alloc(u32 target_pid, Doorbell* db, u32 rights) {
    if (!db) return kNullCap;
    Process* target = sched::find_process(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    return cap::alloc(*target->cap_table, CAP_DOORBELL, db, requested);
}

CapHandle doorbell_create(u32 pid) {
    Doorbell* db = doorbell_create_object(pid);
    return doorbell_cap_alloc(pid, db, RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP);
}

CapHandle doorbell_share(u32 owner_pid, CapHandle db_cap, u32 target_pid, u32 rights) {
    CapEntry* src = resolve_doorbell_entry(owner_pid, db_cap, RIGHT_DUP);
    if (!src) return kNullCap;

    Doorbell* db = static_cast<Doorbell*>(src->target);
    if (!db) return kNullCap;

    Process* target = sched::find_process(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    if ((requested & src->rights_mask) != requested) return kNullCap;

    return doorbell_cap_alloc(target_pid, db, requested);
}

SysError notify_object(Doorbell* db) {
    metric_inc(g_notify_calls);

    if (!db) return E_NOTFOUND;

    Thread* waiter = nullptr;
    const u64 irq = local_irq_save_disable();
    lock(db);

    __atomic_add_fetch(&db->counter, 1ull, __ATOMIC_RELEASE);

    waiter = db->waiter;
    db->waiter = nullptr;

    if (waiter) {
        __atomic_store_n(&waiter->waiting_doorbell, nullptr, __ATOMIC_RELEASE);
        metric_inc(g_wake_count);
        metric_inc(g_notify_woke_thread);
        if (waiter->cpu_id == sched::current_cpu_id()) metric_inc(g_doorbell_local_wakes);
        else metric_inc(g_doorbell_remote_wakes);
    } else {
        metric_inc(g_notify_no_waiter);
    }

    unlock(db);
    local_irq_restore(irq);

    if (waiter) sched::wake(waiter);
    return E_OK;
}

SysError notify(CapHandle handle, u32 caller_pid) {
    CapEntry* e = resolve_doorbell_entry(caller_pid, handle, RIGHT_NOTIFY);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    return notify_object(db);
}

SysError wait_doorbell(CapHandle handle, u64 expected, u32 caller_pid) {
    metric_inc(g_wait_calls);

    CapEntry* e = resolve_doorbell_entry(caller_pid, handle, RIGHT_WAIT);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    if (!db) return E_NOTFOUND;

    Thread* cur = sched::current();
    if (!cur) return E_DEAD;

    const u64 irq = local_irq_save_disable();
    lock(db);

    const u64 now = __atomic_load_n(&db->counter, __ATOMIC_ACQUIRE);
    if (now != expected) {
        metric_inc(g_wait_immediate);
        unlock(db);
        local_irq_restore(irq);
        return E_OK;
    }

    if (db->waiter != nullptr) {
        unlock(db);
        local_irq_restore(irq);
        return E_BUSY;
    }

    db->waiter = cur;
    __atomic_store_n(&cur->waiting_doorbell, db, __ATOMIC_RELEASE);
    cur->state = TS_SLEEPING;
    metric_inc(g_sleep_count);
    sched::block(cur);

    unlock(db);
    local_irq_restore(irq);
    return E_OK;
}

u64 doorbell_counter(CapHandle handle, u32 caller_pid) {
    CapEntry* e = resolve_doorbell_entry(caller_pid, handle, RIGHT_WAIT);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    if (!db) return 0;
    return __atomic_load_n(&db->counter, __ATOMIC_ACQUIRE);
}

void doorbell_clear_waiter(Thread* thread) {
    if (!thread) return;

    Doorbell* db = __atomic_load_n(&thread->waiting_doorbell, __ATOMIC_ACQUIRE);
    if (!db) return;

    const u64 irq = local_irq_save_disable();
    lock(db);
    if (db->waiter == thread) db->waiter = nullptr;
    Doorbell* waiting = __atomic_load_n(&thread->waiting_doorbell, __ATOMIC_ACQUIRE);
    if (waiting == db) {
        __atomic_store_n(&thread->waiting_doorbell, nullptr, __ATOMIC_RELEASE);
    }
    unlock(db);
    local_irq_restore(irq);
}

u64 doorbell_sleep_count() { return metric_load(g_sleep_count); }
u64 doorbell_wake_count() { return metric_load(g_wake_count); }
u64 doorbell_wait_calls() { return metric_load(g_wait_calls); }
u64 doorbell_wait_immediate() { return metric_load(g_wait_immediate); }
u64 notify_calls() { return metric_load(g_notify_calls); }
u64 notify_no_waiter() { return metric_load(g_notify_no_waiter); }
u64 notify_woke_thread() { return metric_load(g_notify_woke_thread); }
u64 doorbell_local_wake_count() { return metric_load(g_doorbell_local_wakes); }
u64 doorbell_remote_wake_count() { return metric_load(g_doorbell_remote_wakes); }

} // namespace ipc
