// kernel/syscall/dispatch.cc — SVC dispatch table.
//
// Spec ref: §6 Syscall ABI

#include "anko/syscall.h"
#include "anko/cap.h"
#include "anko/platform_control.h"
#include "anko/sched.h"
#include "anko/ipc.h"
#include "anko/irq.h"
#include "anko/mm.h"
#include "anko/init.h"
#include "anko/platform.h"
#include "anko/platform_clock.h"
#include "anko/vm.h"
#include "anko/el2_boot.h"
#include "midix/abi/diagnostics.h"
#include "anko/log.h"
#include "anko/panic.h"
#ifdef ANKO_BOARD_HUAWEI_ANNE
#include "boot_diagnostics.h"
#endif
#include "libk/metric.h"
#include "libk/printf.h"
#include "libk/string.h"

namespace drivers::timer { u64 tick_count(); u64 cpu_tick_count(u32 cpu); }
extern "C" u64 g_kernel_boot_cntpct;
extern "C" u64 g_cpu_mpidrs[MX_HOST_MAX_CPUS];

namespace {

u64 g_syscall_count = 0;
u64 g_ipc_syscall_count = 0;
u64 g_debug_puts_count = 0;
u64 g_el0_fault_count = 0;
constexpr u64 kUserCopyMin = 0x1000'0000ull;
constexpr u64 kUserCopyMax = 0x4000'0000ull;
constexpr u64 kMaxUserElfSize = 16ull * 1024ull * 1024ull;
constexpr usize kMaxArenaPages    = 64;  // pages; also sizes allocs[] VLA
constexpr u32   kMaxStackSlots    = 16;  // threads per process
constexpr u64 kLegacyVcpuInfoSize = 56;  // VcpuInfo ABI before sched-context fields.
constexpr VirtAddr kStackFrameAlign = 16; // AArch64 ABI: SP must be 16-byte aligned

using libk::metric_inc;
using libk::metric_load;

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

struct UserAccess {
    VmSpace space{};
    bool valid = false;
};

UserAccess user_access_for(Process* p) {
    UserAccess ua{};
    if (p && p->ttbr0) {
        ua.space.pgd_phys = p->ttbr0;
        ua.space.pid = p->pid;
        ua.valid = true;
    }
    return ua;
}

bool user_range_ok(const UserAccess& ua, u64 ptr, u64 len, u32 rights = RIGHT_READ) {
    if (ptr < kUserCopyMin || ptr >= kUserCopyMax) return false;
    if (len == 0) return true;
    u64 end = 0;
    if (__builtin_add_overflow(ptr, len, &end)) return false;
    if (end < ptr) return false;
    if (end > kUserCopyMax) return false;

    const u64 kbase = reinterpret_cast<u64>(&__text_start[0]);
    const u64 kend = reinterpret_cast<u64>(&__kernel_end[0]);
    if (ptr < kend && end > kbase) return false;

    if (!ua.valid) return false;
    return mm::user_range_mapped(ua.space, ptr, len, rights);
}

[[maybe_unused]] bool copy_from_user(const UserAccess& ua, void* dst, u64 user_ptr, u64 len) {
    if (!dst) return false;
    if (len == 0) return true;
    if (!user_range_ok(ua, user_ptr, len, RIGHT_READ)) return false;
    libk::memcpy(dst, reinterpret_cast<const void*>(user_ptr), static_cast<usize>(len));
    return true;
}

[[maybe_unused]] bool copy_to_user(const UserAccess& ua, u64 user_ptr, const void* src, u64 len) {
    if (!src) return false;
    if (len == 0) return true;
    if (!user_range_ok(ua, user_ptr, len, RIGHT_WRITE)) return false;
    libk::memcpy(reinterpret_cast<void*>(user_ptr), src, static_cast<usize>(len));
    return true;
}

bool principal_from_id(u64 raw, PrincipalClass* out) {
    if (!out) return false;
    if (raw == PRINCIPAL_NATIVE_TRUSTED || raw == PRINCIPAL_NATIVE_SANDBOXED) {
        *out = static_cast<PrincipalClass>(raw);
        return true;
    }
    return false;
}

u64 ring_bytes(const Ring& ring) {
    return sizeof(RingHeader) + (static_cast<u64>(1) << ring.order) * sizeof(RingSlot);
}

u64 round_page_size(u64 size) {
    u64 rounded = 0;
    if (__builtin_add_overflow(size, static_cast<u64>(PAGE_SIZE - 1), &rounded)) return 0;
    return rounded & ~(static_cast<u64>(PAGE_SIZE - 1));
}

bool map_object(Process& proc, VirtAddr va, PhysAddr pa, u64 size, u32 rights, bool device) {
    if (va == 0 || (va & (PAGE_SIZE - 1)) != 0) return false;
    if (pa == 0 || (pa & (PAGE_SIZE - 1)) != 0) return false;
    if (size == 0) return false;
    if (va < kUserCopyMin || va >= kUserCopyMax) return false;
    u64 end = 0;
    if (__builtin_add_overflow(va, size, &end)) return false;
    if (end > kUserCopyMax || end <= va) return false;
    const u64 kbase = reinterpret_cast<u64>(&__text_start[0]);
    const u64 kend = reinterpret_cast<u64>(&__kernel_end[0]);
    if (va < kend && end > kbase) return false;
    VmSpace space{};
    space.pgd_phys = proc.ttbr0;
    space.pid = proc.pid;
    return device ? mm::vm_map_device_range(space, va, pa, size, rights)
                  : mm::vm_map_range(space, va, pa, size, rights);
}




} // namespace

extern "C" void syscall_fault(u64 esr, u64 far, void* frame) {
    auto* tf = static_cast<TrapFrame*>(frame);
    Process* p = cur_proc();
    Thread* ct = sched::current();
    const u32 ec = static_cast<u32>((esr >> 26) & 0x3fu);
    const u32 iss = static_cast<u32>(esr & 0x01ff'ffffu);
#ifdef ANKO_BOARD_HUAWEI_ANNE
    anne::boot_user_fault(esr, tf ? tf->elr : 0, far, tf ? tf->sp_el0 : 0,
                          p ? p->pid : 0, ct ? ct->tid : 0);
#endif

    if (ec == 0x3c) {
        // BRK from EL0 is used by supervised demo services as an intentional
        // crash signal. Keep notifying the supervisor, but don't make the
        // expected death look like a kernel-fatal condition on the UART log.
        ANKO_LOGF_WARN("kernel/fault",
                       "EL0 brk pid=%u tid=%u esr=%llx elr=%llx sp=%llx "
                       "x0=%llx x1=%llx x8=%llx x9=%llx x16=%llx",
                       p ? p->pid : 0,
                       ct ? ct->tid : 0,
                       static_cast<unsigned long long>(esr),
                       static_cast<unsigned long long>(tf ? tf->elr : 0),
                       static_cast<unsigned long long>(tf ? tf->sp_el0 : 0),
                       static_cast<unsigned long long>(tf ? tf->x[0] : 0),
                       static_cast<unsigned long long>(tf ? tf->x[1] : 0),
                       static_cast<unsigned long long>(tf ? tf->x[8] : 0),
                       static_cast<unsigned long long>(tf ? tf->x[9] : 0),
                       static_cast<unsigned long long>(tf ? tf->x[16] : 0));
    } else {
        ANKO_LOGF_FATAL("kernel/fault",
                        "EL0 fault pid=%u(%s) tid=%u svpid=%u principal=%u "
                        "sched_class=%u principal_id=%u "
                        "esr=%llx ec=%u iss=%x far=%llx "
                        "elr=%llx sp=%llx x0=%llx x1=%llx x2=%llx x3=%llx "
                        "x4=%llx x5=%llx x6=%llx x7=%llx x8=%llx x9=%llx "
                        "x10=%llx x11=%llx x12=%llx x13=%llx x14=%llx x15=%llx "
                        "x16=%llx x17=%llx x19=%llx x20=%llx x21=%llx x22=%llx",
                        p ? p->pid : 0,
                        (p && p->name[0]) ? p->name : "?",
                        ct ? ct->tid : 0,
                        p ? p->supervisor_pid : 0,
                        p ? static_cast<u32>(p->principal_class) : 0,
                        ct ? static_cast<u32>(ct->sched.sched_class) : 0,
                        ct ? ct->principal_id : 0,
                        static_cast<unsigned long long>(esr),
                        ec,
                        iss,
                        static_cast<unsigned long long>(far),
                        static_cast<unsigned long long>(tf ? tf->elr : 0),
                        static_cast<unsigned long long>(tf ? tf->sp_el0 : 0),
                        static_cast<unsigned long long>(tf ? tf->x[0] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[1] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[2] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[3] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[4] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[5] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[6] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[7] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[8] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[9] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[10] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[11] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[12] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[13] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[14] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[15] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[16] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[17] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[19] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[20] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[21] : 0),
                        static_cast<unsigned long long>(tf ? tf->x[22] : 0));
    }

    metric_inc(g_el0_fault_count);
    sched::record_exit(p, static_cast<long>(esr));
    sched::mark_current_dead(true);
}

extern "C" SysResult syscall_dispatch(u64 nr, u64 a0, u64 a1, u64 a2,
                                      u64 a3, u64 a4, u64 a5) {
#ifdef ANKO_BOARD_HUAWEI_ANNE
    anne::boot_syscall(nr);
#endif
    (void)a5;
    metric_inc(g_syscall_count);

    Process* p = cur_proc();
    if (!p && nr != SYS_DEBUG_PUTS && nr != SYS_DEBUG_COUNTERS) {
        Thread* t = sched::current();
        if (t) {
            t->state = TS_DEAD;
            sched::request_switch(false, SWITCH_EXIT);
        }
        return err(E_DEAD);
    }
    if (p) {
        Thread* t = sched::current();
        ANKO_ASSERT(t && t->state == TS_RUNNING);
    }
    if (p && __atomic_load_n(&p->dying, __ATOMIC_ACQUIRE)) return err(E_DEAD);
    const UserAccess ua = user_access_for(p);

    vm::CallGuard vm_call((nr >= SYS_VM_CREATE && nr <= SYS_VM_MEMORY_WRITE) ||
                          (nr >= SYS_VCPU_BIND_SCHED_CONTEXT && nr <= SYS_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER) ||
                          (nr >= SYS_VM_ASSIGN_DEVICE && nr <= SYS_VM_SHARED_MEMORY_INSPECT));
    switch (static_cast<Syscall>(nr)) {

    case SYS_YIELD:
        sched::yield();
        return ok(E_OK);

    case SYS_EXIT: {
        sched::record_exit(p, static_cast<long>(a0));
        sched::mark_current_dead(true);
        return ok(E_OK);
    }

    case SYS_SPAWN: {
        if (!a0 || a1 == 0 || a1 > kMaxUserElfSize) return err(E_INVALID);
        if (!user_range_ok(ua, a0, a1)) return err(E_FAULT);
        char kname[32]{};
        bool have_name = false;
        if (a5 != 0) {
            if (!copy_from_user(ua, kname, a5, sizeof(kname))) return err(E_FAULT);
            kname[sizeof(kname) - 1] = '\0';
            have_name = true;
        }

        PrincipalClass principal = PRINCIPAL_NATIVE_SANDBOXED;
        if (!principal_from_id(a2, &principal)) return err(E_INVALID);

        cap::Ref supervisor_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a3), CAP_SUPERVISOR);
        if (!supervisor_e) return err(E_NOTFOUND);
        if (!has_rights(supervisor_e, RIGHT_SPAWN)) return err(E_PERM);
        auto* supervisor = static_cast<Process*>(supervisor_e->target);
        if (!supervisor) return err(E_NOTFOUND);
        if (supervisor->dying) return err(E_DEAD);
        if (!cap::reserve_slots(*p->cap_table, 2)) return err(E_OVERFLOW);

        Thread* main_thread = nullptr;
        Process* child = kernel::init::spawn_image(reinterpret_cast<const void*>(a0),
                                                  static_cast<usize>(a1),
                                                  principal,
                                                  supervisor->pid,
                                                  static_cast<u32>(a4),
                                                  &main_thread);
        if (!child) return err(E_NOMEM);
        if (have_name) sched::process_set_name(child, kname);
        CapHandle h = cap::alloc(*p->cap_table, CAP_PROCESS, child,
                                 RIGHT_READ | RIGHT_KILL | RIGHT_DUP | RIGHT_SPAWN);
        if (h == kNullCap) {
            sched::process_destroy(child);
            return err(E_OVERFLOW);
        }
        CapHandle th = kNullCap;
        if (main_thread) {
            th = cap::alloc(*p->cap_table, CAP_THREAD, main_thread,
                            RIGHT_READ | RIGHT_DUP | RIGHT_THREAD_CONTROL);
            if (th == kNullCap) {
                cap::revoke(*p->cap_table, h);
                sched::process_destroy(child);
                return err(E_OVERFLOW);
            }
        }
        return ok(static_cast<long>(h), static_cast<long>(th));
    }

    case SYS_KILL: {
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_PROCESS);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_KILL)) return err(E_PERM);
        auto* target = static_cast<Process*>(e->target);
        if (!target) return err(E_NOTFOUND);
        sched::record_exit(target, -1);
        if (target == p) {
            sched::mark_current_dead(true);
            return ok(E_OK);
        }
        sched::process_destroy(target);
        return ok(E_OK);
    }

    case SYS_MAP_PAGE: {
        cap::Ref e = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a0));
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
            const u64 size = round_page_size(ring_bytes(*ring));
            if (size == 0) return err(E_INVALID);
            u32 arena_idx = 0;
            if (!p->arena_table ||
                !mm::arena_register(*p->arena_table, static_cast<VirtAddr>(a1),
                                    ring->phys_base, size, p->pid,
                                    RIGHT_READ | RIGHT_WRITE, 0, &arena_idx)) {
                return err(E_OVERFLOW);
            }
            if (!map_object(*p, a1, ring->phys_base, size, map_rights, false)) {
                mm::arena_unregister(*p->arena_table, arena_idx);
                return err(E_INVALID);
            }
            mm::arena_retain_mapping(*p->arena_table, a1, e->object);
            return ok(E_OK);
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
            if (!arena->phys_base) return err(E_INVALID);
            bool claimed_now = false;
            if (!mm::arena_claim_va(*p->arena_table, *arena, static_cast<VirtAddr>(a1),
                                    arena->size, &claimed_now)) {
                return err(E_OVERFLOW);
            }
            if (!map_object(*p, a1, arena->phys_base, arena->size, map_rights, false)) {
                if (claimed_now) {
                    mm::arena_release_va(*p->arena_table, *arena, static_cast<VirtAddr>(a1));
                }
                return err(E_INVALID);
            }
            mm::arena_retain_mapping(*p->arena_table, a1, e->object);
            return ok(E_OK);
        }
        return err(E_BADTYPE);
    }

    case SYS_UNMAP_PAGE: {
        if (a0 == 0 || (a0 & (PAGE_SIZE - 1)) != 0) return err(E_INVALID);
        VmSpace space{};
        space.pgd_phys = p->ttbr0;
        space.pid = p->pid;
        mm::vm_unmap(space, a0);
        mm::arena_unmap_page(*p->arena_table, a0);
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
        if (pages == 0 || pages > kMaxArenaPages) return err(E_INVALID);

        const PhysAddr base = mm::alloc_pages_contiguous_for(p->pid, pages);
        if (!base) return err(E_NOMEM);
        auto* arena = static_cast<Arena*>(mm::slab_alloc(sizeof(Arena), alignof(Arena)));
        if (!arena) {
            for (usize j = 0; j < pages; ++j) mm::free_page_for(p->pid, base + j * PAGE_SIZE);
            return err(E_NOMEM);
        }
        *arena = {};
        arena->phys_base = base; arena->size = rounded_size;
        arena->owner_pid = p->pid; arena->owner_generation = p->generation; arena->rights = RIGHT_READ | RIGHT_WRITE;
        // User flags cannot request exposure of physical addresses or ownership.
        arena->flags = kArenaFlagOwnsPages;
        auto* root = object::create(arena, mm::destroy_arena);
        if (!root) { mm::destroy_arena(arena); return err(E_NOMEM); }
        const CapHandle handle = cap::alloc(*p->cap_table, CAP_ARENA, arena,
            RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_REGISTER | RIGHT_DUP);
        object::release(root);
        return handle ? ok(static_cast<long>(handle)) : err(E_OVERFLOW);
    }

    case SYS_ARENA_REGISTER: {
        if (!p->arena_table || !a0 || !a1 || (a0 | a1) & (PAGE_SIZE - 1)) return err(E_INVALID);
        VmSpace space{p->ttbr0, p->pid};
        if (!mm::user_range_mapped(space, a0, a1, RIGHT_READ | RIGHT_WRITE)) return err(E_INVALID);
        auto* arena = static_cast<Arena*>(mm::slab_alloc(sizeof(Arena), alignof(Arena)));
        if (!arena) return err(E_NOMEM);
        *arena = {}; arena->base_va = a0; arena->size = a1;
        arena->owner_pid = p->pid; arena->owner_generation = p->generation; arena->rights = RIGHT_READ | RIGHT_WRITE;
        auto* root = object::create(arena, mm::destroy_arena);
        if (!root) { mm::destroy_arena(arena); return err(E_NOMEM); }
        const CapHandle handle = cap::alloc(*p->cap_table, CAP_ARENA, arena,
            RIGHT_READ | RIGHT_WRITE | RIGHT_REGISTER | RIGHT_DUP);
        object::release(root);
        return handle ? ok(static_cast<long>(handle)) : err(E_OVERFLOW);
    }

    case SYS_CAP_DUP: {
        cap::Ref src = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a0));
        if (!src) return err(E_NOTFOUND);
        if (!has_rights(src, RIGHT_DUP)) return err(E_PERM);
        const u32 requested = static_cast<u32>(a1);
        if (requested == 0) return err(E_INVALID);
        if ((requested & src->rights_mask) != requested) return err(E_PERM);
        CapHandle h = cap::dup(*p->cap_table, static_cast<CapHandle>(a0), requested);
        if (h == kNullCap) return err(E_NOTFOUND);
        return ok(static_cast<long>(h));
    }

    case SYS_CAP_REVOKE: {
        // Revoking the line capability tears the binding down (and disables the
        // SPI) so a driver that loses its IRQ authority cannot leave an enabled
        // stub behind.
        IrqLine* revoked_line = nullptr;
        {
            cap::Ref e = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a0));
            if (e && e->type_tag == CAP_IRQ) {
                revoked_line = static_cast<IrqLine*>(e->target);
            }
        }
        cap::revoke(*p->cap_table, static_cast<CapHandle>(a0));
        if (revoked_line) irq::unbind_line(revoked_line, sched::process_identity(*p));
        return ok(E_OK);
    }

    case SYS_CAP_IDENTIFY: {
        cap::Ref e = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a0));
        if (!e) return err(E_NOTFOUND);
        return ok(static_cast<long>(e->type_tag));
    }

    case SYS_CAP_GRANT: {
        cap::Ref target_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_PROCESS);
        if (!target_e || !has_rights(target_e, RIGHT_SPAWN)) return err(E_PERM);
        auto* target_proc = static_cast<Process*>(target_e->target);
        if (!target_proc || !target_proc->cap_table) return err(E_NOTFOUND);

        cap::Ref src = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a1));
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
        metric_inc(g_ipc_syscall_count);
        CapHandle h = ipc::ring_create(p->pid, static_cast<u32>(a0), static_cast<u32>(a1));
        if (h == kNullCap) return err(E_NOMEM);
        return ok(static_cast<long>(h));
    }

    case SYS_RING_SHARE: {
        metric_inc(g_ipc_syscall_count);
        cap::Ref target_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a1), CAP_PROCESS);
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
        metric_inc(g_ipc_syscall_count);
        CapHandle h = ipc::doorbell_create(p->pid);
        if (h == kNullCap) return err(E_NOMEM);
        return ok(static_cast<long>(h));
    }

    case SYS_DOORBELL_SHARE: {
        metric_inc(g_ipc_syscall_count);
        cap::Ref target_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a1), CAP_PROCESS);
        if (!target_e) return err(E_BADTYPE);
        if (!has_rights(target_e, RIGHT_SPAWN)) return err(E_PERM);
        auto* target_proc = static_cast<Process*>(target_e->target);
        if (!target_proc) return err(E_NOTFOUND);
        CapHandle h = ipc::doorbell_share(p->pid, static_cast<CapHandle>(a0), target_proc->pid,
                                          static_cast<u32>(a2));
        if (h == kNullCap) return err(E_PERM);
        return ok(static_cast<long>(h));
    }

    case SYS_NOTIFY: {
        metric_inc(g_ipc_syscall_count);
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_NOTIFY)) return err(E_PERM);
        const SysError rc = ipc::notify(static_cast<CapHandle>(a0), p->pid);
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_PLATFORM_CONTROL: {
        auto entry = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_PLATFORM);
        if (!entry || !has_rights(entry, RIGHT_WRITE)) return err(E_PERM);
        if (a1 > 0xffffffffu) return err(E_INVALID);
        return ok(platform::control(*static_cast<platform::Control*>(entry->target),
            static_cast<u32>(a1), a2, sched::process_identity(*p)));
    }

    case SYS_WAIT_DOORBELL_UNTIL: {
        return ok(ipc::wait_doorbell_until(static_cast<CapHandle>(a0), a1, a2, p->pid));
    }

    case SYS_WAIT_DOORBELL: {
        metric_inc(g_ipc_syscall_count);
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_WAIT)) return err(E_PERM);
        const SysError rc = ipc::wait_doorbell(static_cast<CapHandle>(a0), a1, p->pid);
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_DOORBELL_READ: {
        metric_inc(g_ipc_syscall_count);
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_WAIT)) return err(E_PERM);
        return ok(static_cast<long>(ipc::doorbell_counter(static_cast<CapHandle>(a0), p->pid)));
    }

    case SYS_SUPERVISOR_POLL: {
        cap::Ref supervisor_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_SUPERVISOR);
        if (!supervisor_e) return err(E_NOTFOUND);
        if (!has_rights(supervisor_e, RIGHT_READ)) return err(E_PERM);

        auto* supervisor = static_cast<Process*>(supervisor_e->target);
        if (!supervisor) return err(E_NOTFOUND);
        if (supervisor->dying) return err(E_DEAD);

        u64 identity = 0; long status = 0;
        if (!sched::poll_exit(*supervisor, &identity, &status)) return err(E_NOTFOUND);
        return ok(static_cast<long>(identity), status);
    }

    case SYS_THREAD_CREATE: {
        Process* target_proc = nullptr;
        if (static_cast<CapHandle>(a0) == kNullCap) {
            // proc_cap==0 means "create a helper thread in my own process".
            target_proc = p;
        } else {
            cap::Ref target_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_PROCESS);
            if (!target_e || !has_rights(target_e, RIGHT_SPAWN)) return err(E_PERM);
            target_proc = static_cast<Process*>(target_e->target);
        }
        if (!target_proc || !target_proc->cap_table || !target_proc->ttbr0) return err(E_NOTFOUND);
        if (a1 == 0) return err(E_INVALID);

        VmSpace target_space{};
        target_space.pgd_phys = target_proc->ttbr0;
        target_space.pid = target_proc->pid;
        if (!mm::user_range_mapped(target_space, static_cast<VirtAddr>(a1), 4, RIGHT_EXEC)) {
            return err(E_FAULT);
        }
        if (!cap::reserve_slots(*p->cap_table, 1)) return err(E_OVERFLOW);

        u32 slot = target_proc->next_stack_slot;
        if (slot == 0) slot = 1;
        if (slot > kMaxStackSlots) return err(E_OVERFLOW);
        const PhysAddr stack = mm::alloc_page_for(target_proc->pid);
        if (!stack) return err(E_NOMEM);
        libk::memset(reinterpret_cast<void*>(stack), 0, PAGE_SIZE);

        VmSpace space{};
        space.pgd_phys = target_proc->ttbr0;
        space.pid = target_proc->pid;
        const VirtAddr stack_base = USER_STACK_TOP - (static_cast<VirtAddr>(slot + 1) * PAGE_SIZE);
        if (!mm::vm_map_owned(space, stack_base, stack, RIGHT_READ | RIGHT_WRITE)) {
            mm::free_page_for(target_proc->pid, stack);
            return err(E_INVALID);
        }
        const VirtAddr sp_top = stack_base + PAGE_SIZE - kStackFrameAlign;

        Thread* t = sched::thread_create(target_proc->pid,
                                         static_cast<VirtAddr>(a1),
                                         sp_top,
                                         PRIO_NORMAL,
                                         a2,
                                         static_cast<u32>(a3));
        if (!t) {
            mm::vm_unmap_owned_range(space, stack_base, PAGE_SIZE);
            return err(E_NOMEM);
        }
        CapHandle th = cap::alloc(*p->cap_table, CAP_THREAD, t,
                                  RIGHT_READ | RIGHT_DUP | RIGHT_THREAD_CONTROL);
        if (th == kNullCap) {
            sched::thread_destroy(t);
            mm::vm_unmap_owned_range(space, stack_base, PAGE_SIZE);
            return err(E_OVERFLOW);
        }
        target_proc->next_stack_slot = slot + 1;
        if (!(static_cast<u32>(a3) & THREAD_SUSPENDED)) sched::enqueue(t);
        return ok(static_cast<long>(th));
    }

    case SYS_THREAD_SET_AFFINITY: {
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_THREAD);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_THREAD_CONTROL)) return err(E_PERM);
        auto* t = static_cast<Thread*>(e->target);
        if (!t) return err(E_NOTFOUND);
        if (t->state == TS_DEAD) return err(E_DEAD);
        return sched::set_affinity(t, a1) ? ok(E_OK) : err(E_INVALID);
    }

    case SYS_THREAD_RESUME: {
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_THREAD);
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
        return copy_to_user(ua, a1, &info, sizeof(info)) ? ok(E_OK) : err(E_FAULT);
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
        return copy_to_user(ua, a1, &info, sizeof(info)) ? ok(E_OK) : err(E_FAULT);
    }

    case SYS_VM_MEMORY_WRITE: {
        if (a3 != 0 && !user_range_ok(ua, a2, a3, RIGHT_READ)) return err(E_FAULT);
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
        /*
         * VcpuInfo grew when sched-context metadata was added.  Keep the
         * syscall ABI append-only: older managers may pass the original
         * shorter struct and should receive the prefix they understand, while
         * newer managers receive the full extended view.
         */
        if (!a1 || a2 < kLegacyVcpuInfoSize) return err(E_INVALID);
        vm::VcpuInfo info{};
        const SysError rc = vm::inspect_vcpu(*p, static_cast<CapHandle>(a0), &info);
        if (rc != E_OK) return err(rc);
        const u64 copy_len = (a2 < sizeof(info)) ? a2 : sizeof(info);
        return copy_to_user(ua, a1, &info, copy_len) ? ok(E_OK) : err(E_FAULT);
    }

    case SYS_VCPU_RUN: {
        if (!a1 || !a2) return err(E_INVALID);
        vm::VcpuRunBudget budget{};
        if (!copy_from_user(ua, &budget, a1, sizeof(budget))) return err(E_FAULT);
        vm::VcpuRunResult result{};
        const SysError rc = vm::run_vcpu(*p, static_cast<CapHandle>(a0), budget, &result);
        if (!copy_to_user(ua, a2, &result, sizeof(result))) return err(E_FAULT);
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_SCHED_CONTEXT_CREATE: {
        if (!a0 || a1 < sizeof(SchedContext)) return err(E_INVALID);
        SchedContext ctx{};
        if (!copy_from_user(ua, &ctx, a0, sizeof(ctx))) return err(E_FAULT);
        SysError rc = E_OK;
        CapHandle h = sched::sched_context_create(*p, ctx, &rc);
        if (h == kNullCap) return err(rc);
        return ok(static_cast<long>(h));
    }

    case SYS_SCHED_CONTEXT_BIND_THREAD: {
        const SysError rc = sched::sched_context_bind_thread(*p, static_cast<CapHandle>(a0),
                                                            static_cast<CapHandle>(a1));
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VCPU_BIND_SCHED_CONTEXT: {
        const SysError rc = vm::vcpu_bind_sched_context(*p, static_cast<CapHandle>(a0),
                                                        static_cast<CapHandle>(a1));
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VCPU_SET_CORE_MASK: {
        const SysError rc = vm::vcpu_set_affinity(*p, static_cast<CapHandle>(a0), a1,
                                                  static_cast<u32>(a2));
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VCPU_KICK: {
        const SysError rc = vm::vcpu_kick(*p, static_cast<CapHandle>(a0), static_cast<u32>(a1));
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VCPU_INJECT_IRQ: {
        const SysError rc = vm::vcpu_inject_irq(*p, static_cast<CapHandle>(a0), static_cast<u32>(a1));
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER: {
        const SysError rc = vm::vcpu_block_until_irq_or_timer(*p, static_cast<CapHandle>(a0));
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

    case SYS_VM_CHANNEL_MAP: {
        if (a4 > 0xffffffffull) return err(E_INVALID);
        const auto rc = vm::map_channel(*p, static_cast<CapHandle>(a0),
                                       static_cast<CapHandle>(a1), a2, a3, static_cast<u32>(a4));
        return rc == E_OK ? ok(E_OK) : err(rc);
    }
    case SYS_VM_CHANNEL_NOTIFY: {
        const auto rc = vm::notify_channel(*p, static_cast<CapHandle>(a0));
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VM_CHANNEL_STAGE: {
        // a0 = channel_cap, a1 = guest IPA, a2 = size (both page-aligned).
        if (a1 == 0 || a2 == 0) return err(E_INVALID);
        const auto rc = vm::stage_channel(*p, static_cast<CapHandle>(a0), a1, a2);
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VM_SHARED_MEMORY_ATTACH: {
        // a0 = vm_cap, a1 = arena_cap, a2 = guest_pa, a3 = size, a4 = rights, a5 = flags
        const u32 rights = static_cast<u32>(a4);
        if (a4 != rights) return err(E_INVALID);
        const u32 flags = static_cast<u32>(a5);
        if (a5 != flags) return err(E_INVALID);
        SysError rc = E_OK;
        const CapHandle h = vm::attach_shared_memory(*p, static_cast<CapHandle>(a0),
                                                     static_cast<CapHandle>(a1), a2, a3, rights,
                                                     flags, &rc);
        if (h == kNullCap) return err(rc);
        return ok(static_cast<long>(h));
    }

    case SYS_VM_SHARED_MEMORY_REGISTER_DMA: {
        // a0 = shared_cap, a1 = device permissions (DmaPerm bitmask)
        const u32 perms = static_cast<u32>(a1);
        if (a1 != perms) return err(E_INVALID);
        const SysError rc = vm::register_shared_memory_dma(*p, static_cast<CapHandle>(a0), perms);
        return rc == E_OK ? ok(E_OK) : err(rc);
    }

    case SYS_VM_SHARED_MEMORY_INSPECT: {
        if (!a1 || a2 < sizeof(vm::SharedArenaInfo)) return err(E_INVALID);
        vm::SharedArenaInfo info{};
        const SysError rc = vm::inspect_shared_memory(*p, static_cast<CapHandle>(a0), &info);
        if (rc != E_OK) return err(rc);
        return copy_to_user(ua, a1, &info, sizeof(info)) ? ok(E_OK) : err(E_FAULT);
    }

    case SYS_VM_CHANNEL_INSPECT: {
        if (!a1 || a2 < sizeof(vm::VmChannelInfo)) return err(E_INVALID);
        vm::VmChannelInfo info{};
        const SysError rc = vm::inspect_channel(*p, static_cast<CapHandle>(a0), &info);
        if (rc != E_OK) return err(rc);
        return copy_to_user(ua, a1, &info, sizeof(info)) ? ok(E_OK) : err(E_FAULT);
    }

    case SYS_DEBUG_PUTS: {
        const u64 len = a1;
        if (!a0 || len > 4096) return err(E_INVALID);
        if (!user_range_ok(ua, a0, len)) return err(E_FAULT);
        const char* s = reinterpret_cast<const char*>(a0);
        metric_inc(g_debug_puts_count);
#ifdef ANKO_BOARD_HUAWEI_ANNE
        anne::boot_log_write(s, len);
#else
        for (u64 i = 0; i < len; ++i) libk::putchar(s[i]);
#endif
        return ok(E_OK);
    }

    case SYS_CLOCK_NS: return ok(static_cast<long>(sched::sched_clock_ns()));

    // Phase D: PCI device assignment into a VM (§6, §7, §8.11/§8.12).
    case SYS_VM_ASSIGN_DEVICE: {
        // a0 = vm_cap, a1 = host function cap, a2 = flags
        cap::Ref fn_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a1), CAP_PCI_FUNCTION);
        if (!fn_e) return err(E_NOTFOUND);
        if ((fn_e->rights_mask & RIGHT_WRITE) == 0) return err(E_PERM);
        auto* host_fn = static_cast<pci::HostFunction*>(fn_e->target);
        if (!host_fn) return err(E_NOTFOUND);
        CapHandle out = kNullCap;
        const SysError rc = vm::device_secure_attach(*p, static_cast<CapHandle>(a0), *host_fn, a2, &out);
        if (rc != E_OK) return err(rc);
        return ok(static_cast<long>(out));
    }

    case SYS_VM_ASSIGN_MAP_MMIO: {
        // a0 = assign, a1 = bar_index, a2 = guest_ipa, a3 = size, a4 = rights, a5 = bar_offset
        const u32 bar_index = static_cast<u32>(a1);
        if (a1 != bar_index || bar_index >= pci::kMaxBars || a5 > 0xffffffffull) return err(E_INVALID);
        const u32 rights = static_cast<u32>(a4);
        if (a4 != rights) return err(E_INVALID);
        const SysError rc = vm::device_map_bar(*p, static_cast<CapHandle>(a0), bar_index,
                                               static_cast<u32>(a5), a3, a2, rights);
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_VM_ASSIGN_ROUTE_IRQ: {
        // a0 = assign, a1 = irq_cap, a2 = vcpu_cap, a3 = guest_irq, a4 = flags
        const u32 guest_irq = static_cast<u32>(a3);
        if (a3 != guest_irq || guest_irq >= vm::kVgicMaxIrqs) return err(E_INVALID);
        const u32 flags = static_cast<u32>(a4);
        if (a4 != flags || flags != 0) return err(E_INVALID);
        const SysError rc = vm::device_route_irq(*p, static_cast<CapHandle>(a0),
                                                 static_cast<CapHandle>(a1),
                                                 static_cast<CapHandle>(a2), guest_irq, flags);
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_VM_ASSIGN_EXPOSE_ECAM:
        return err(vm::device_expose_ecam(*p, static_cast<CapHandle>(a0), a1, a2));

    case SYS_VM_ASSIGN_DESTROY:
        return err(vm::device_destroy(*p, static_cast<CapHandle>(a0)));

    case SYS_DEBUG_READ: {
        // Fixed-size records and bounded log chunks keep stack use and time
        // with IRQs masked small. Never copy directly into a user buffer while
        // holding the retained-log lock.
        if (!a2 || !a3 || a3 > MX_DIAG_READ_MAX) return err(E_INVALID);
        switch (a0) {
        case MX_DIAG_SYSTEM: {
            if (a1 || a3 < sizeof(MxDiagSystem)) return err(E_INVALID);
            MxDiagSystem info{};
            info.version = MX_DIAG_VERSION;
#ifdef ANKO_BOARD_HUAWEI_ANNE
            info.board = MX_DIAG_BOARD_ANNE;
            anne::boot_log_info(&info.boot_log_size, &info.boot_log_capacity);
#elif defined(ANKO_BOARD_SWITCH_T210)
            info.board = MX_DIAG_BOARD_SWITCH;
#else
            info.board = MX_DIAG_BOARD_QEMU;
#endif
            info.cpu_limit = MX_HOST_MAX_CPUS;
            info.sampled_cpu = sched::current_cpu_id();
            info.online_mask = sched::online_cpu_mask();
            asm volatile("mrs %0, CurrentEL" : "=r"(info.current_el));
            info.current_el >>= 2;
            asm volatile("mrs %0, MIDR_EL1" : "=r"(info.midr));
            asm volatile("mrs %0, MPIDR_EL1" : "=r"(info.mpidr));
            asm volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(info.pfr0));
            asm volatile("mrs %0, ID_AA64MMFR0_EL1" : "=r"(info.mmfr0));
            asm volatile("mrs %0, SCTLR_EL1" : "=r"(info.sctlr));
            asm volatile("mrs %0, TCR_EL1" : "=r"(info.tcr));
            asm volatile("mrs %0, MAIR_EL1" : "=r"(info.mair));
            asm volatile("mrs %0, CTR_EL0" : "=r"(info.ctr));
            asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(info.counter_reported_hz));
            info.counter_effective_hz = platform::counter_frequency(info.counter_reported_hz);
            return copy_to_user(ua, a2, &info, sizeof(info)) ? ok(sizeof(info)) : err(E_FAULT);
        }
        case MX_DIAG_CPU: {
            if (a1 >= MX_HOST_MAX_CPUS || a3 < sizeof(MxDiagCpu)) return err(E_INVALID);
            const u32 cpu = static_cast<u32>(a1);
            MxDiagCpu info{};
            info.version = MX_DIAG_VERSION;
            info.cpu = cpu;
            info.online = (sched::online_cpu_mask() >> cpu) & 1;
            info.mpidr = g_cpu_mpidrs[cpu];
            // Read the release-published ready flag first so a concurrent
            // secondary entry cannot report ready before its entry record.
            info.monitor_ready = vm::monitor_ready(cpu);
            info.boot_entry_el = vm::entry_level(cpu);
            info.context_switches = sched::cpu_context_switch_count(cpu);
            info.timer_preempts = sched::cpu_timer_preempt_count(cpu);
            info.timer_ticks = drivers::timer::cpu_tick_count(cpu);
            info.ipis_handled = sched::cpu_ipi_handled_count(cpu);
            return copy_to_user(ua, a2, &info, sizeof(info)) ? ok(sizeof(info)) : err(E_FAULT);
        }
        case MX_DIAG_BOOT_LOG: {
            if (!user_range_ok(ua, a2, a3, RIGHT_WRITE)) return err(E_FAULT);
#ifdef ANKO_BOARD_HUAWEI_ANNE
            char bytes[MX_DIAG_READ_MAX];
            const usize count = anne::boot_log_read(a1, bytes, a3);
            return copy_to_user(ua, a2, bytes, count) ? ok(count) : err(E_FAULT);
#else
            return err(E_NOTIMPL);
#endif
        }
        default: return err(E_INVALID);
        }
    }

    case SYS_DEBUG_COUNTERS:
        if (a0 >= vm::VM_DEBUG_CREATE_CALLS && a0 <= vm::VM_DEBUG_VM_DEVICE_MAX) {
            return ok(static_cast<long>(vm::debug_counter(a0)));
        }
        switch (a0) {
        case 200: return ok(static_cast<long>(object::live_count()));
        case 201: return ok(static_cast<long>(mm::allocated_page_count()));
        // Phase L shared-arena counters (attached / number registered SharedGpu).
        case 202: return ok(static_cast<long>(vm::shared_arena_stats().attached));
        case 203: return ok(static_cast<long>(vm::shared_arena_stats().dma_registered));
        case DBG_SYSCALL_COUNT:         return ok(static_cast<long>(metric_load(g_syscall_count)));
        case DBG_IPC_SYSCALL_COUNT:     return ok(static_cast<long>(metric_load(g_ipc_syscall_count)));
        case DBG_CONTEXT_SWITCHES:      return ok(static_cast<long>(sched::context_switch_count()));
        case DBG_DEBUG_PUTS:            return ok(static_cast<long>(metric_load(g_debug_puts_count)));
        case DBG_EL0_FAULTS:            return ok(static_cast<long>(metric_load(g_el0_fault_count)));
        case DBG_DOORBELL_SLEEPS:       return ok(static_cast<long>(ipc::doorbell_sleep_count()));
        case DBG_DOORBELL_WAKES:        return ok(static_cast<long>(ipc::doorbell_wake_count()));
        case DBG_TIMER_TICKS:           return ok(static_cast<long>(drivers::timer::tick_count()));
        case DBG_DOORBELL_WAIT_CALLS:   return ok(static_cast<long>(ipc::doorbell_wait_calls()));
        case DBG_DOORBELL_WAIT_IMMED:   return ok(static_cast<long>(ipc::doorbell_wait_immediate()));
        case DBG_NOTIFY_CALLS:          return ok(static_cast<long>(ipc::notify_calls()));
        case DBG_NOTIFY_NO_WAITER:      return ok(static_cast<long>(ipc::notify_no_waiter()));
        case DBG_NOTIFY_WOKE:           return ok(static_cast<long>(ipc::notify_woke_thread()));
        case DBG_YIELD_COUNT:           return ok(static_cast<long>(sched::yield_count()));
        case DBG_BLOCK_SWITCHES:        return ok(static_cast<long>(sched::block_switch_count()));
        case DBG_TIMER_PREEMPTS:        return ok(static_cast<long>(sched::timer_preempt_count()));
        case DBG_EXIT_SWITCHES:         return ok(static_cast<long>(sched::exit_switch_count()));
        case DBG_ONLINE_CPUS:           return ok(static_cast<long>(sched::online_cpu_count()));
        case DBG_CPU0_CTX_SWITCHES:     return ok(static_cast<long>(sched::cpu_context_switch_count(0)));
        case DBG_CPU1_CTX_SWITCHES:     return ok(static_cast<long>(sched::cpu_context_switch_count(1)));
        case DBG_CPU0_TIMER_PREEMPTS:   return ok(static_cast<long>(sched::cpu_timer_preempt_count(0)));
        case DBG_CPU1_TIMER_PREEMPTS:   return ok(static_cast<long>(sched::cpu_timer_preempt_count(1)));
        case DBG_LOCAL_WAKES:           return ok(static_cast<long>(sched::local_wake_count()));
        case DBG_REMOTE_WAKES:          return ok(static_cast<long>(sched::remote_wake_count()));
        case DBG_IPI_SENT:              return ok(static_cast<long>(sched::ipi_sent_count()));
        case DBG_IPI_HANDLED:           return ok(static_cast<long>(sched::ipi_handled_count()));
        case DBG_PENDING_WAKE_DRAINS:   return ok(static_cast<long>(sched::pending_wake_drain_count()));
        case DBG_DOORBELL_LOCAL_WAKES:  return ok(static_cast<long>(ipc::doorbell_local_wake_count()));
        case DBG_DOORBELL_REMOTE_WAKES: return ok(static_cast<long>(ipc::doorbell_remote_wake_count()));
        case DBG_FAIRNESS_LAG:          return ok(static_cast<long>(sched::fairness_max_lag()));
        case DBG_PRINCIPAL_TRUSTED_RT:  return ok(static_cast<long>(sched::principal_class_runtime(PRINCIPAL_NATIVE_TRUSTED)));
        case DBG_PRINCIPAL_SANDBOX_RT:  return ok(static_cast<long>(sched::principal_class_runtime(PRINCIPAL_NATIVE_SANDBOXED)));
        case DBG_WAKE_DOORBELL:         return ok(static_cast<long>(sched::wake_doorbell_count()));
        case DBG_WAKE_REMOTE:           return ok(static_cast<long>(sched::wake_remote_count()));
        case DBG_WAKE_RESUME:           return ok(static_cast<long>(sched::wake_resume_count()));
        case DBG_IRQ_DELIVERED:         return ok(static_cast<long>(irq::delivered_count()));
        case DBG_IRQ_BOUND:             return ok(static_cast<long>(irq::binding_count()));
        case DBG_IRQ_SELFTEST:          return ok(static_cast<long>(irq::selftest_status()));
        case DBG_BOOT_CS: {
            // Centiseconds from kernel entry to now (the msh boot banner).
            u64 now = 0, reported = 0;
            asm volatile("mrs %0, CNTPCT_EL0" : "=r"(now) :: "memory");
            asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(reported));
            const u64 freq = platform::counter_frequency(reported);
            if (!freq || now <= g_kernel_boot_cntpct) return ok(0);
            const u64 delta = now - g_kernel_boot_cntpct;
            return ok(static_cast<long>((delta * 100ull) / freq));
        }
        default: return err(E_INVALID);
        }

    case SYS_NOTIFY_HANDOFF: {
        metric_inc(g_ipc_syscall_count);
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_NOTIFY)) return err(E_PERM);
        const SysError rc = ipc::notify_handoff(static_cast<CapHandle>(a0), a1, p->pid);
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_REPLY_HANDOFF: {
        metric_inc(g_ipc_syscall_count);
        const SysError rc = ipc::reply_handoff(a0, p->pid);
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_MAP_MMIO: {
        // a0 = mmio_cap, a1 = va, a2 = rights
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_MMIO);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_MAP)) return err(E_PERM);
        auto* range = static_cast<MmioRange*>(e->target);
        if (!range) return err(E_NOTFOUND);
        const u32 requested = static_cast<u32>(a2);
        if (a2 != requested || (requested & ~(RIGHT_READ | RIGHT_WRITE)) ||
            !(requested & RIGHT_READ) || !has_rights(e, requested)) return err(E_PERM);
        const bool device = (range->attr == MX_MMIO_DEVICE_nGnRnE ||
                             range->attr == MX_MMIO_DEVICE_nGnRE);
        if (!map_object(*p, static_cast<VirtAddr>(a1), range->phys_base,
                        range->size, static_cast<u32>(a2), device))
            return err(E_FAULT);
        return ok(E_OK);
    }

    case SYS_IRQ_BIND_DOORBELL: {
        // a0 = irq_cap, a1 = doorbell_cap, a2 = flags
        cap::Ref irq_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_IRQ);
        if (!irq_e) return err(E_NOTFOUND);
        if (!has_rights(irq_e, RIGHT_WRITE)) return err(E_PERM);
        cap::Ref db_e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a1), CAP_DOORBELL);
        if (!db_e) return err(E_NOTFOUND);
        if (!has_rights(db_e, RIGHT_NOTIFY)) return err(E_PERM);
        auto* line = static_cast<IrqLine*>(irq_e->target);
        if (!line) return err(E_NOTFOUND);
        const u32 flags = static_cast<u32>(a2);
        if (flags != a2) return err(E_INVALID);
        const SysError rc = irq::bind(line, static_cast<Doorbell*>(db_e->target),
                                      sched::process_identity(*p), flags);
        if (rc != E_OK) return err(rc);
        line->doorbell_cap = a1;
        return ok(E_OK);
    }

    case SYS_IRQ_ACK: {
        // a0 = irq_cap. Edge lines were EOI'd at IRQ; level lines are paused on
        // delivery and re-armed here once the driver has serviced the source.
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_IRQ);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_WRITE)) return err(E_PERM);
        auto* line = static_cast<IrqLine*>(e->target);
        if (!line) return err(E_NOTFOUND);
        const SysError rc = irq::ack(line, sched::process_identity(*p));
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_CAP_INFO: {
        if (!a1 || a2 < sizeof(MxCapInfo)) return err(E_INVALID);
        cap::Ref e = cap::lookup_any(*p->cap_table, static_cast<CapHandle>(a0));
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_READ)) return err(E_PERM);

        MxCapInfo info{};
        info.type = e->type_tag;
        info.rights = e->rights_mask;
        switch (static_cast<CapType>(e->type_tag)) {
        case CAP_PROCESS: {
            auto* process = static_cast<Process*>(e->target);
            info.base = sched::process_identity(*process);
            info.flags = __atomic_load_n(&process->dying, __ATOMIC_ACQUIRE);
            break;
        }
        case CAP_MMIO: {
            auto* range = static_cast<MmioRange*>(e->target);
            if (!range) return err(E_NOTFOUND);
            info.base = range->phys_base;
            info.size = range->size;
            info.attr = range->attr;
            info.flags = range->flags;
            info.aux0 = range->device_hint;
            break;
        }
        case CAP_ARENA: {
            auto* arena = static_cast<Arena*>(e->target);
            if (!arena) return err(E_NOTFOUND);
            info.base = (arena->flags & kArenaFlagExposePhys)
                            ? arena->phys_base
                            : arena->base_va;
            info.size = arena->size;
            info.attr = mm::range_is_noncached(arena->phys_base, arena->size)
                            ? MX_ARENA_MEMORY_NONCACHEABLE : MX_ARENA_MEMORY_WRITE_BACK;
            info.flags = arena->flags & ~(kArenaFlagExposePhys | kArenaFlagOwnsPages);
            info.aux0 = arena->owner_pid;
            break;
        }
        case CAP_IRQ: {
            auto* line = static_cast<IrqLine*>(e->target);
            if (!line) return err(E_NOTFOUND);
            info.base = line->irq_id;
            info.size = 1;
            info.attr = line->trigger;
            info.flags = line->flags;
            info.aux0 = line->device_hint;
            break;
        }
        case CAP_PAGE: {
            auto* mem = static_cast<MemoryObject*>(e->target);
            if (!mem) return err(E_NOTFOUND);
            info.base = mem->phys_base;
            info.size = mem->size;
            break;
        }
        case CAP_VM_SHARED: {
            // Geometry and DMA class of an attached shared region. `attr` carries
            // the VmDmaClass (0 = None), so userspace can see the region has not
            // been granted device access until it registers it.
            auto* shared = static_cast<vm::SharedArena*>(e->target);
            if (!shared) return err(E_NOTFOUND);
            info.base = shared->guest_pa_base;
            info.size = shared->size;
            info.attr = static_cast<u32>(shared->dma_class);
            info.flags = shared->flags;
            info.aux0 = shared->shared_id;
            break;
        }
        case CAP_UART:
            info.base = reinterpret_cast<PhysAddr>(e->target);
            info.size = PAGE_SIZE;
            break;
        default:
            break;
        }
        return copy_to_user(ua, a1, &info, sizeof(info)) ? ok(E_OK) : err(E_FAULT);
    }

    default:
        return err(E_INVALID);
    }
}
