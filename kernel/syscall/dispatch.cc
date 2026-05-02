// kernel/syscall/dispatch.cc — SVC dispatch table.
//
// Spec ref: §6 Syscall ABI

#include "anko/syscall.h"
#include "anko/cap.h"
#include "anko/sched.h"
#include "anko/ipc.h"
#include "anko/mm.h"
#include "anko/init.h"
#include "anko/platform.h"
#include "anko/vm.h"
#include "libk/printf.h"
#include "libk/string.h"

namespace drivers::timer { u64 tick_count(); }

namespace {

u64 g_syscall_count = 0;
u64 g_ipc_syscall_count = 0;
u64 g_debug_puts_count = 0;
u64 g_el0_fault_count = 0;
bool g_supervisor_event_pending = false;
u32 g_supervisor_event_pid = 0;
u32 g_supervisor_event_supervisor = 0;
long g_supervisor_event_status = 0;
constexpr u64 kUserCopyMin = 0x4000'0000ull;
constexpr u64 kUserCopyMax = 0x8000'0000ull;
constexpr u64 kMaxUserElfSize = 16ull * 1024ull * 1024ull;

inline void counter_inc(u64& counter) {
    __atomic_add_fetch(&counter, 1ull, __ATOMIC_RELAXED);
}

inline u64 counter_load(const u64& counter) {
    return __atomic_load_n(&counter, __ATOMIC_RELAXED);
}

extern "C" char __text_start[];
extern "C" char __kernel_end[];

inline SysResult ok(long value = 0, long detail = 0) {
    return {value, detail};
}

inline SysResult err(SysError e, long detail = 0) {
    return {static_cast<long>(e), detail};
}

inline Process* cur_proc() {
    return sched::current_process();
}

inline bool has_rights(const CapEntry* e, u32 rights) {
    return e && ((e->rights_mask & rights) == rights);
}

bool user_range_ok(u64 ptr, u64 len, u32 rights = RIGHT_READ) {
    if (ptr < kUserCopyMin || ptr >= kUserCopyMax) return false;
    if (len == 0) return true;
    u64 end = 0;
    if (__builtin_add_overflow(ptr, len, &end)) return false;
    if (end < ptr) return false;
    if (end > kUserCopyMax) return false;

    const u64 kbase = reinterpret_cast<u64>(&__text_start[0]);
    const u64 kend = reinterpret_cast<u64>(&__kernel_end[0]);
    if (ptr < kend && end > kbase) return false;

    Process* p = cur_proc();
    if (!p || !p->ttbr0) return false;
    VmSpace space{};
    space.pgd_phys = p->ttbr0;
    space.pid = p->pid;
    return mm::user_range_mapped(space, ptr, len, rights);
}

[[maybe_unused]] bool copy_from_user(void* dst, u64 user_ptr, u64 len) {
    if (!dst) return false;
    if (len == 0) return true;
    if (!user_range_ok(user_ptr, len, RIGHT_READ)) return false;
    libk::memcpy(dst, reinterpret_cast<const void*>(user_ptr), static_cast<usize>(len));
    return true;
}

[[maybe_unused]] bool copy_to_user(u64 user_ptr, const void* src, u64 len) {
    if (!src) return false;
    if (len == 0) return true;
    if (!user_range_ok(user_ptr, len, RIGHT_WRITE)) return false;
    libk::memcpy(reinterpret_cast<void*>(user_ptr), src, static_cast<usize>(len));
    return true;
}

bool principal_from_id(u64 raw, Principal* out) {
    if (!out) return false;
    if (raw == PRINCIPAL_NATIVE_TRUSTED || raw == PRINCIPAL_NATIVE_SANDBOXED) {
        *out = static_cast<Principal>(raw);
        return true;
    }
    return false;
}

u64 ring_bytes(const Ring& ring) {
    return sizeof(RingHeader) + (static_cast<u64>(1) << ring.order) * sizeof(RingSlot);
}

bool map_object(Process& proc, VirtAddr va, PhysAddr pa, u64 size, u32 rights, bool device) {
    if (va == 0 || (va & (PAGE_SIZE - 1)) != 0) return false;
    if (pa == 0 || (pa & (PAGE_SIZE - 1)) != 0) return false;
    if (size == 0) return false;
    if (!user_range_ok(va, 1, RIGHT_READ)) {
        // user_range_ok requires the mapping to exist, so only use the cheap
        // syntactic checks for a destination that is about to be mapped.
        if (va < kUserCopyMin || va >= kUserCopyMax) return false;
        u64 end = 0;
        if (__builtin_add_overflow(va, size, &end)) return false;
        if (end > kUserCopyMax || end <= va) return false;
        const u64 kbase = reinterpret_cast<u64>(&__text_start[0]);
        const u64 kend = reinterpret_cast<u64>(&__kernel_end[0]);
        if (va < kend && end > kbase) return false;
    }
    VmSpace space{};
    space.pgd_phys = proc.ttbr0;
    space.pid = proc.pid;
    return device ? mm::vm_map_device_range(space, va, pa, size, rights)
                  : mm::vm_map_range(space, va, pa, size, rights);
}

void record_supervisor_event(Process* proc, long status) {
    if (!proc || proc->supervisor_pid == 0) return;
    if (g_supervisor_event_pending) return;
    g_supervisor_event_pending = true;
    g_supervisor_event_pid = proc->pid;
    g_supervisor_event_supervisor = proc->supervisor_pid;
    g_supervisor_event_status = status;
}

} // namespace

extern "C" void syscall_fault(u64 esr, u64 far, void* frame) {
    (void)far;
    (void)frame;
    counter_inc(g_el0_fault_count);
    record_supervisor_event(cur_proc(), static_cast<long>(esr));
    sched::mark_current_dead();
}

extern "C" SysResult syscall_dispatch(u64 nr, u64 a0, u64 a1, u64 a2,
                                      u64 a3, u64 a4, u64 a5) {
    (void)a5;
    counter_inc(g_syscall_count);

    Process* p = cur_proc();
    if (!p && nr != SYS_DEBUG_PUTS && nr != SYS_DEBUG_COUNTERS) {
        Thread* t = sched::current();
        if (t) {
            t->state = TS_DEAD;
            sched::request_switch(false, SWITCH_EXIT);
        }
        return err(E_DEAD);
    }

    switch (static_cast<Syscall>(nr)) {

    case SYS_YIELD:
        sched::yield();
        return ok(E_OK);

    case SYS_EXIT: {
        record_supervisor_event(p, static_cast<long>(a0));
        sched::mark_current_dead();
        return ok(E_OK);
    }

    case SYS_SPAWN: {
        if (!a0 || a1 == 0 || a1 > kMaxUserElfSize) return err(E_INVALID);
        if (!user_range_ok(a0, a1)) return err(E_FAULT);

        Principal principal = PRINCIPAL_NATIVE_SANDBOXED;
        if (!principal_from_id(a2, &principal)) return err(E_INVALID);

        CapEntry* supervisor_e =
            cap::lookup(*p->cap_table, static_cast<CapHandle>(a3), CAP_SUPERVISOR);
        if (!supervisor_e) return err(E_NOTFOUND);
        if (!has_rights(supervisor_e, RIGHT_SPAWN)) return err(E_PERM);
        auto* supervisor = static_cast<Process*>(supervisor_e->target);
        if (!supervisor) return err(E_NOTFOUND);

        Thread* main_thread = nullptr;
        Process* child = kernel::init::spawn_image(reinterpret_cast<const void*>(a0),
                                                  static_cast<usize>(a1),
                                                  principal,
                                                  supervisor->pid,
                                                  static_cast<u32>(a4),
                                                  &main_thread);
        if (!child) return err(E_NOMEM);
        CapHandle h = cap::alloc(*p->cap_table, CAP_PROCESS, child,
                                 RIGHT_READ | RIGHT_KILL | RIGHT_DUP | RIGHT_SPAWN);
        if (h == kNullCap) return err(E_OVERFLOW);
        CapHandle th = kNullCap;
        if (main_thread) {
            th = cap::alloc(*p->cap_table, CAP_THREAD, main_thread,
                            RIGHT_READ | RIGHT_DUP | RIGHT_THREAD_CONTROL);
            if (th == kNullCap) return err(E_OVERFLOW);
        }
        return ok(static_cast<long>(h), static_cast<long>(th));
    }

    case SYS_KILL: {
        CapEntry* e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_PROCESS);
        if (!has_rights(e, RIGHT_KILL)) return err(E_PERM);
        auto* target = static_cast<Process*>(e->target);
        if (!target) return err(E_NOTFOUND);
        record_supervisor_event(target, -1);
        sched::process_destroy(target);
        cap::revoke(*p->cap_table, static_cast<CapHandle>(a0));
        return ok(E_OK);
    }

    case SYS_MAP_PAGE: {
        CapEntry* e = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a0));
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_MAP)) return err(E_PERM);
        if (a1 == 0 || (a1 & (PAGE_SIZE - 1)) != 0) return err(E_INVALID);

        const u32 map_rights = static_cast<u32>(a2 ? a2 : (e->rights_mask & (RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC)));
        if (map_rights == 0) return err(E_INVALID);
        if ((map_rights & ~(RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC)) != 0) return err(E_INVALID);
        if (!has_rights(e, map_rights)) return err(E_PERM);
        if ((map_rights & RIGHT_WRITE) && (map_rights & RIGHT_EXEC)) return err(E_PERM);

        if (e->type_tag == CAP_SHARED_RING) {
            auto* ring = static_cast<Ring*>(e->target);
            if (!ring) return err(E_NOTFOUND);
            return map_object(*p, a1, ring->phys_base, ring_bytes(*ring), map_rights, false)
                ? ok(E_OK)
                : err(E_INVALID);
        }
        if (e->type_tag == CAP_PAGE) {
            auto* mem = static_cast<MemoryObject*>(e->target);
            if (!mem) return err(E_NOTFOUND);
            return map_object(*p, a1, mem->phys_base, mem->size, map_rights, false)
                ? ok(E_OK)
                : err(E_INVALID);
        }
        if (e->type_tag == CAP_UART) {
            const PhysAddr pa = reinterpret_cast<PhysAddr>(e->target);
            return map_object(*p, a1, pa, PAGE_SIZE, map_rights, true)
                ? ok(E_OK)
                : err(E_INVALID);
        }
        if (e->type_tag == CAP_ARENA) {
            auto* arena = static_cast<Arena*>(e->target);
            if (!arena) return err(E_NOTFOUND);
            return map_object(*p, a1, arena->base_va, arena->size, map_rights, false)
                ? ok(E_OK)
                : err(E_INVALID);
        }
        return err(E_BADTYPE);
    }

    case SYS_UNMAP_PAGE: {
        if (a0 == 0 || (a0 & (PAGE_SIZE - 1)) != 0) return err(E_INVALID);
        VmSpace space{};
        space.pgd_phys = p->ttbr0;
        space.pid = p->pid;
        mm::vm_unmap(space, a0);
        return ok(E_OK);
    }

    case SYS_ARENA_CREATE: {
        if (!p->arena_table) return err(E_NOMEM);
        const u64 size = a0;
        if (size == 0) return err(E_INVALID);
        u64 rounded_size = 0;
        if (__builtin_add_overflow(size, static_cast<u64>(PAGE_SIZE - 1), &rounded_size)) {
            return err(E_INVALID);
        }
        rounded_size &= ~(static_cast<u64>(PAGE_SIZE - 1));

        const usize pages = static_cast<usize>(rounded_size / PAGE_SIZE);
        if (pages == 0 || pages > 64) return err(E_INVALID);

        PhysAddr allocs[64]{};
        PhysAddr base = 0;
        PhysAddr expect = 0;
        for (usize i = 0; i < pages; ++i) {
            PhysAddr pa = mm::alloc_page();
            if (!pa || (i != 0 && pa != expect)) {
                if (pa) mm::free_page(pa);
                for (usize j = 0; j < i; ++j) mm::free_page(allocs[j]);
                return err(E_NOMEM);
            }
            allocs[i] = pa;
            if (i == 0) base = pa;
            expect = pa + PAGE_SIZE;
        }

        u32 idx = 0;
        if (!mm::arena_register(*p->arena_table, static_cast<VirtAddr>(base),
                                rounded_size, p->pid,
                                RIGHT_READ | RIGHT_WRITE, static_cast<u32>(a1), &idx)) {
            for (usize j = 0; j < pages; ++j) mm::free_page(allocs[j]);
            return err(E_OVERFLOW);
        }

        Arena* arena = &p->arena_table->entries[idx];
        CapHandle cap_h = cap::alloc(*p->cap_table, CAP_ARENA, arena,
                                     RIGHT_READ | RIGHT_WRITE | RIGHT_REGISTER | RIGHT_DUP);
        if (cap_h == kNullCap) {
            for (usize j = 0; j < pages; ++j) mm::free_page(allocs[j]);
            libk::memset(arena, 0, sizeof(*arena));
            if (p->arena_table->count == idx + 1) p->arena_table->count = idx;
            return err(E_OVERFLOW);
        }
        return ok(static_cast<long>(cap_h));
    }

    case SYS_ARENA_REGISTER: {
        if (!p->arena_table) return err(E_NOMEM);
        u32 idx = 0;
        if (!mm::arena_register(*p->arena_table, static_cast<VirtAddr>(a0), a1, p->pid,
                                RIGHT_READ | RIGHT_WRITE, static_cast<u32>(a2), &idx)) {
            return err(E_OVERFLOW);
        }
        Arena* arena = &p->arena_table->entries[idx];
        CapHandle cap_h = cap::alloc(*p->cap_table, CAP_ARENA, arena,
                                     RIGHT_READ | RIGHT_WRITE | RIGHT_REGISTER | RIGHT_DUP);
        if (cap_h == kNullCap) {
            libk::memset(arena, 0, sizeof(*arena));
            if (p->arena_table->count == idx + 1) p->arena_table->count = idx;
            return err(E_OVERFLOW);
        }
        return ok(static_cast<long>(cap_h));
    }

    case SYS_CAP_DUP: {
        CapEntry* src = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a0));
        if (!src) return err(E_NOTFOUND);
        if (!has_rights(src, RIGHT_DUP)) return err(E_PERM);
        const u32 requested = static_cast<u32>(a1);
        if (requested == 0) return err(E_INVALID);
        if ((requested & src->rights_mask) != requested) return err(E_PERM);
        CapHandle h = cap::dup(*p->cap_table, static_cast<CapHandle>(a0), requested);
        if (h == kNullCap) return err(E_NOTFOUND);
        return ok(static_cast<long>(h));
    }

    case SYS_CAP_REVOKE:
        cap::revoke(*p->cap_table, static_cast<CapHandle>(a0));
        return ok(E_OK);

    case SYS_CAP_IDENTIFY: {
        CapEntry* e = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a0));
        if (!e) return err(E_NOTFOUND);
        return ok(static_cast<long>(e->type_tag));
    }

    case SYS_CAP_GRANT: {
        CapEntry* target_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_PROCESS);
        if (!target_e || !has_rights(target_e, RIGHT_SPAWN)) return err(E_PERM);
        auto* target_proc = static_cast<Process*>(target_e->target);
        if (!target_proc || !target_proc->cap_table) return err(E_NOTFOUND);

        CapEntry* src = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a1));
        if (!src) return err(E_NOTFOUND);
        if (!has_rights(src, RIGHT_DUP)) return err(E_PERM);
        const u32 requested = static_cast<u32>(a2);
        if (requested == 0) return err(E_INVALID);
        if ((requested & src->rights_mask) != requested) return err(E_PERM);
        const u32 new_rights = requested;

        CapHandle h = cap::alloc(*target_proc->cap_table,
                                 static_cast<CapType>(src->type_tag),
                                 src->target,
                                 new_rights);
        if (h == kNullCap) return err(E_OVERFLOW);
        return ok(static_cast<long>(h));
    }

    case SYS_RING_CREATE: {
        counter_inc(g_ipc_syscall_count);
        CapHandle h = ipc::ring_create(p->pid, static_cast<u32>(a0), static_cast<u32>(a1));
        if (h == kNullCap) return err(E_NOMEM);
        return ok(static_cast<long>(h));
    }

    case SYS_RING_SHARE: {
        counter_inc(g_ipc_syscall_count);
        CapEntry* target_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a1), CAP_PROCESS);
        if (!target_e) return err(E_BADTYPE);
        if (!has_rights(target_e, RIGHT_SPAWN)) return err(E_PERM);
        auto* target_proc = static_cast<Process*>(target_e->target);
        if (!target_proc) return err(E_NOTFOUND);
        CapHandle h = ipc::ring_share(p->pid, static_cast<CapHandle>(a0), target_proc->pid,
                                      static_cast<u32>(a2));
        if (h == kNullCap) return err(E_PERM);
        return ok(static_cast<long>(h));
    }

    case SYS_DOORBELL_CREATE: {
        counter_inc(g_ipc_syscall_count);
        CapHandle h = ipc::doorbell_create(p->pid);
        if (h == kNullCap) return err(E_NOMEM);
        return ok(static_cast<long>(h));
    }

    case SYS_DOORBELL_SHARE: {
        counter_inc(g_ipc_syscall_count);
        CapEntry* target_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a1), CAP_PROCESS);
        if (!target_e) return err(E_BADTYPE);
        if (!has_rights(target_e, RIGHT_SPAWN)) return err(E_PERM);
        auto* target_proc = static_cast<Process*>(target_e->target);
        if (!target_proc) return err(E_NOTFOUND);
        CapHandle h = ipc::doorbell_share(p->pid, static_cast<CapHandle>(a0), target_proc->pid,
                                          static_cast<u32>(a2));
        if (h == kNullCap) return err(E_PERM);
        return ok(static_cast<long>(h));
    }

    case SYS_NOTIFY:
        counter_inc(g_ipc_syscall_count);
        {
            CapEntry* e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
            if (!e) return err(E_NOTFOUND);
            if (!has_rights(e, RIGHT_NOTIFY)) return err(E_PERM);
        }
        {
            const SysError rc = ipc::notify(static_cast<CapHandle>(a0), p->pid);
            if (rc != E_OK) return err(rc);
            return ok(E_OK);
        }

    case SYS_WAIT_DOORBELL:
        counter_inc(g_ipc_syscall_count);
        {
            CapEntry* e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
            if (!e) return err(E_NOTFOUND);
            if (!has_rights(e, RIGHT_WAIT)) return err(E_PERM);
        }
        {
            const SysError rc = ipc::wait_doorbell(static_cast<CapHandle>(a0), a1, p->pid);
            if (rc != E_OK) return err(rc);
            return ok(E_OK);
        }

    case SYS_DOORBELL_READ:
        counter_inc(g_ipc_syscall_count);
        {
            CapEntry* e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
            if (!e) return err(E_NOTFOUND);
            if (!has_rights(e, RIGHT_WAIT)) return err(E_PERM);
        }
        return ok(static_cast<long>(ipc::doorbell_counter(static_cast<CapHandle>(a0), p->pid)));

    case SYS_SUPERVISOR_POLL: {
        CapEntry* supervisor_e =
            cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_SUPERVISOR);
        if (!supervisor_e) return err(E_NOTFOUND);
        if (!has_rights(supervisor_e, RIGHT_READ)) return err(E_PERM);
        auto* supervisor = static_cast<Process*>(supervisor_e->target);
        if (!supervisor) return err(E_NOTFOUND);
        if (!g_supervisor_event_pending ||
            g_supervisor_event_supervisor != supervisor->pid) {
            return err(E_NOTFOUND);
        }
        const u32 dead_pid = g_supervisor_event_pid;
        const long status = g_supervisor_event_status;
        g_supervisor_event_pending = false;
        g_supervisor_event_pid = 0;
        g_supervisor_event_supervisor = 0;
        g_supervisor_event_status = 0;
        return ok(static_cast<long>(dead_pid), status);
    }

    case SYS_THREAD_CREATE: {
        CapEntry* target_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_PROCESS);
        if (!target_e || !has_rights(target_e, RIGHT_SPAWN)) return err(E_PERM);
        auto* target_proc = static_cast<Process*>(target_e->target);
        if (!target_proc || !target_proc->cap_table || !target_proc->ttbr0) return err(E_NOTFOUND);
        if (a1 == 0) return err(E_INVALID);

        u32 slot = target_proc->next_stack_slot;
        if (slot == 0) slot = 1;
        if (slot > 16) return err(E_OVERFLOW);
        const PhysAddr stack = mm::alloc_page();
        if (!stack) return err(E_NOMEM);
        libk::memset(reinterpret_cast<void*>(stack), 0, PAGE_SIZE);

        VmSpace space{};
        space.pgd_phys = target_proc->ttbr0;
        space.pid = target_proc->pid;
        const VirtAddr stack_base = USER_STACK_TOP - (static_cast<VirtAddr>(slot + 1) * PAGE_SIZE);
        if (!mm::vm_map(space, stack_base, stack, RIGHT_READ | RIGHT_WRITE)) {
            mm::free_page(stack);
            return err(E_INVALID);
        }
        target_proc->next_stack_slot = slot + 1;
        const VirtAddr sp_top = stack_base + PAGE_SIZE - 16;

        Thread* t = sched::thread_create(target_proc->pid,
                                         static_cast<VirtAddr>(a1),
                                         sp_top,
                                         PRIO_NORMAL,
                                         a2,
                                         static_cast<u32>(a3));
        if (!t) return err(E_NOMEM);
        CapHandle th = cap::alloc(*p->cap_table, CAP_THREAD, t,
                                  RIGHT_READ | RIGHT_DUP | RIGHT_THREAD_CONTROL);
        if (th == kNullCap) return err(E_OVERFLOW);
        if (!(static_cast<u32>(a3) & THREAD_SUSPENDED)) sched::enqueue(t);
        return ok(static_cast<long>(th));
    }

    case SYS_THREAD_SET_AFFINITY: {
        CapEntry* e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_THREAD);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_THREAD_CONTROL)) return err(E_PERM);
        auto* t = static_cast<Thread*>(e->target);
        if (!t) return err(E_NOTFOUND);
        if (t->state == TS_DEAD) return err(E_DEAD);
        return sched::set_affinity(t, a1) ? ok(E_OK) : err(E_INVALID);
    }

    case SYS_THREAD_RESUME: {
        CapEntry* e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_THREAD);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_THREAD_CONTROL)) return err(E_PERM);
        auto* t = static_cast<Thread*>(e->target);
        const SysError rc = sched::resume_thread(t);
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_THREAD_GET_CPU:
        return ok(static_cast<long>(sched::current_cpu_id()));

    case SYS_THREAD_EXIT:
        sched::mark_current_dead();
        return ok(E_OK);

    case SYS_VM_CREATE: {
        SysError rc = E_OK;
        CapHandle h = vm::create_vm(*p, static_cast<CapHandle>(a0), a1, &rc);
        if (h == kNullCap) return err(rc);
        return ok(static_cast<long>(h));
    }

    case SYS_VM_DESTROY: {
        const SysError rc = vm::destroy_vm(*p, static_cast<CapHandle>(a0));
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VM_INSPECT: {
        if (!a1 || a2 < sizeof(vm::VmInfo)) return err(E_INVALID);
        vm::VmInfo info{};
        const SysError rc = vm::inspect_vm(*p, static_cast<CapHandle>(a0), &info);
        if (rc != E_OK) return err(rc);
        return copy_to_user(a1, &info, sizeof(info)) ? ok(E_OK) : err(E_FAULT);
    }

    case SYS_VM_MEMORY_CREATE: {
        SysError rc = E_OK;
        CapHandle h = vm::create_memory(*p, static_cast<CapHandle>(a0), a1, a2, &rc);
        if (h == kNullCap) return err(rc);
        return ok(static_cast<long>(h));
    }

    case SYS_VM_MEMORY_MAP: {
        const SysError rc = vm::map_memory(*p, static_cast<CapHandle>(a0),
                                           static_cast<CapHandle>(a1),
                                           a2, a3, static_cast<u32>(a4));
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VM_MEMORY_UNMAP: {
        const SysError rc = vm::unmap_memory(*p, static_cast<CapHandle>(a0), a1, a2);
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VM_MEMORY_INSPECT: {
        if (!a1 || a2 < sizeof(vm::VmMemoryInfo)) return err(E_INVALID);
        vm::VmMemoryInfo info{};
        const SysError rc = vm::inspect_memory(*p, static_cast<CapHandle>(a0), &info);
        if (rc != E_OK) return err(rc);
        return copy_to_user(a1, &info, sizeof(info)) ? ok(E_OK) : err(E_FAULT);
    }

    case SYS_VM_MEMORY_WRITE: {
        if (a3 != 0 && !user_range_ok(a2, a3, RIGHT_READ)) return err(E_FAULT);
        const SysError rc = vm::write_memory(*p,
                                             static_cast<CapHandle>(a0),
                                             a1,
                                             reinterpret_cast<const void*>(a2),
                                             a3);
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VCPU_CREATE: {
        SysError rc = E_OK;
        CapHandle h = vm::create_vcpu(*p, static_cast<CapHandle>(a0), a1, &rc);
        if (h == kNullCap) return err(rc);
        return ok(static_cast<long>(h));
    }

    case SYS_VCPU_SET_ENTRY: {
        const SysError rc = vm::set_vcpu_entry(*p, static_cast<CapHandle>(a0), a1, a2, a3);
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VCPU_INSPECT: {
        if (!a1 || a2 < sizeof(vm::VcpuInfo)) return err(E_INVALID);
        vm::VcpuInfo info{};
        const SysError rc = vm::inspect_vcpu(*p, static_cast<CapHandle>(a0), &info);
        if (rc != E_OK) return err(rc);
        return copy_to_user(a1, &info, sizeof(info)) ? ok(E_OK) : err(E_FAULT);
    }

    case SYS_VCPU_RUN: {
        const SysError rc = vm::run_vcpu(*p, static_cast<CapHandle>(a0), a1);
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VM_CHANNEL_CREATE: {
        SysError rc = E_OK;
        CapHandle h = vm::create_channel(*p, static_cast<CapHandle>(a0),
                                         static_cast<u32>(a1), static_cast<u32>(a2), &rc);
        if (h == kNullCap) return err(rc);
        return ok(static_cast<long>(h));
    }

    case SYS_VM_CHANNEL_ENDPOINT: {
        SysError rc = E_OK;
        CapHandle h = vm::channel_endpoint(*p, static_cast<CapHandle>(a0),
                                           static_cast<u32>(a1), static_cast<u32>(a2), &rc);
        if (h == kNullCap) return err(rc);
        return ok(static_cast<long>(h));
    }

    case SYS_VM_CHANNEL_INSPECT: {
        if (!a1 || a2 < sizeof(vm::VmChannelInfo)) return err(E_INVALID);
        vm::VmChannelInfo info{};
        const SysError rc = vm::inspect_channel(*p, static_cast<CapHandle>(a0), &info);
        if (rc != E_OK) return err(rc);
        return copy_to_user(a1, &info, sizeof(info)) ? ok(E_OK) : err(E_FAULT);
    }

    case SYS_DEBUG_PUTS: {
        const u64 len = a1;
        if (!a0 || len > 4096) return err(E_INVALID);
        if (!user_range_ok(a0, len)) return err(E_FAULT);
        const char* s = reinterpret_cast<const char*>(a0);
        counter_inc(g_debug_puts_count);
        for (u64 i = 0; i < len; ++i) libk::putchar(s[i]);
        return ok(E_OK);
    }

    case SYS_DEBUG_COUNTERS:
        if (a0 >= vm::VM_DEBUG_CREATE_CALLS && a0 <= vm::VM_DEBUG_VCPU_UNSUPPORTED_EXITS) {
            return ok(static_cast<long>(vm::debug_counter(a0)));
        }
        switch (a0) {
        case 0: return ok(static_cast<long>(counter_load(g_syscall_count)));
        case 1: return ok(static_cast<long>(counter_load(g_ipc_syscall_count)));
        case 2: return ok(static_cast<long>(sched::context_switch_count()));
        case 3: return ok(static_cast<long>(counter_load(g_debug_puts_count)));
        case 4: return ok(static_cast<long>(counter_load(g_el0_fault_count)));
        case 5: return ok(static_cast<long>(ipc::doorbell_sleep_count()));
        case 6: return ok(static_cast<long>(ipc::doorbell_wake_count()));
        case 7: return ok(static_cast<long>(drivers::timer::tick_count()));
        case 8: return ok(static_cast<long>(ipc::doorbell_wait_calls()));
        case 9: return ok(static_cast<long>(ipc::doorbell_wait_immediate()));
        case 10: return ok(static_cast<long>(ipc::notify_calls()));
        case 11: return ok(static_cast<long>(ipc::notify_no_waiter()));
        case 12: return ok(static_cast<long>(ipc::notify_woke_thread()));
        case 13: return ok(static_cast<long>(sched::yield_count()));
        case 14: return ok(static_cast<long>(sched::block_switch_count()));
        case 15: return ok(static_cast<long>(sched::timer_preempt_count()));
        case 16: return ok(static_cast<long>(sched::exit_switch_count()));
        case 17: return ok(static_cast<long>(sched::online_cpu_count()));
        case 18: return ok(static_cast<long>(sched::cpu_context_switch_count(0)));
        case 19: return ok(static_cast<long>(sched::cpu_context_switch_count(1)));
        case 20: return ok(static_cast<long>(sched::cpu_timer_preempt_count(0)));
        case 21: return ok(static_cast<long>(sched::cpu_timer_preempt_count(1)));
        case 22: return ok(static_cast<long>(sched::local_wake_count()));
        case 23: return ok(static_cast<long>(sched::remote_wake_count()));
        case 24: return ok(static_cast<long>(sched::ipi_sent_count()));
        case 25: return ok(static_cast<long>(sched::ipi_handled_count()));
        case 26: return ok(static_cast<long>(sched::pending_wake_drain_count()));
        case 27: return ok(static_cast<long>(ipc::doorbell_local_wake_count()));
        case 28: return ok(static_cast<long>(ipc::doorbell_remote_wake_count()));
        case 29: return ok(static_cast<long>(sched::fairness_max_lag()));
        case 30: return ok(static_cast<long>(sched::principal_runtime(1)));
        case 31: return ok(static_cast<long>(sched::principal_runtime(2)));
        default: return err(E_INVALID);
        }

    default:
        return err(E_INVALID);
    }
}
