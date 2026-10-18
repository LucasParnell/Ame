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
#include "midix/abi/vm.h"
#include "anko/log.h"
#include "anko/panic.h"
#include "anko/spinlock.h"
#include "anko/smp.h"
#ifdef ANKO_BOARD_HUAWEI_ANNE
#include "boot_diagnostics.h"
#include "battery.h"
#include "debug_io.h"
#include "rpmb.h"
#include "rpmb_diagnostics.h"
#include "emmc.h"
#endif
#include "libk/metric.h"
#include "libk/printf.h"
#include "libk/string.h"

namespace drivers::timer { u64 tick_count(); u64 cpu_tick_count(u32 cpu); }
extern "C" u64 g_kernel_boot_cntpct;
extern "C" u64 g_cpu_mpidrs[MX_HOST_MAX_CPUS];

namespace {

// Single-CPU writers avoid a shared counter line on every cross-core IPC.
struct alignas(64) SyscallMetrics {
    u64 calls, ipc, debug_puts, faults;
};
static SyscallMetrics syscall_metrics[MX_HOST_MAX_CPUS]{};
static SyscallMetrics& local_syscall_metrics() { return syscall_metrics[sched::current_cpu_id()]; }
static void syscall_metric_inc(u64& counter) {
    // SVC/synchronous-fault entry masks IRQs. This CPU is the sole writer;
    // diagnostic readers still use atomic loads, without an A53 LL/SC loop.
    __atomic_store_n(&counter, __atomic_load_n(&counter, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
}
static u64 syscall_metric_total(u64 SyscallMetrics::* member) {
    u64 value = 0;
    for (auto& cpu : syscall_metrics) value += libk::metric_load(cpu.*member);
    return value;
}
constexpr u64 kUserCopyMin = 0x1000'0000ull;
constexpr u64 kUserCopyMax = 0x4000'0000ull;
constexpr u64 kMaxUserElfSize = 16ull * 1024ull * 1024ull;
constexpr usize kMaxArenaPages    = 64;  // pages; also sizes allocs[] VLA
constexpr u32   kMaxStackSlots    = 16;  // threads per process
constexpr u64 kLegacyVcpuInfoSize = 56;  // VcpuInfo ABI before sched-context fields.
constexpr VirtAddr kStackFrameAlign = 16; // AArch64 ABI: SP must be 16-byte aligned

using libk::metric_inc;
using libk::metric_load;

#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
// These RPMB service metadata IDs are defined by the matching Hi6250 kernel;
// its driver does not call them during initialization. Keep them separate from
// the request-address and request-done calls: those calls consume a shared
// buffer and are not safe compatibility tests.
constexpr u64 kRpmbCallCount = anne::rpmb::kCallCountSmc;
constexpr u64 kRpmbUid = anne::rpmb::kUidSmc;
constexpr u64 kRpmbVersion = anne::rpmb::kVersionSmc;
constexpr u64 kRpmbRequestAddressSmc = anne::rpmb::kRequestAddressSmc;
constexpr u64 kRpmbRequestDoneSmc = anne::rpmb::kRequestDoneSmc;

constexpr auto kAnneRpmbConfig = anne::rpmb::kAnneConfig;
static kernel::SpinLock g_el3_lock{};

struct SmcWords {
    u64 x0;
    u64 x1;
    u64 x2;
    u64 x3;
};

SmcWords rpmb_call(u64 function_id, u64 arg0 = 0, u64 arg1 = 0, u64 arg2 = 0,
                   u64 arg3 = 0, u64 arg4 = 0, u64 arg5 = 0, u64 arg6 = 0) {
#if defined(__aarch64__)
    // Fixed register variables are intentional: the SMCCC convention assigns
    // the four result words to x0..x3, while ordinary C++ operands may be
    // allocated to any general-purpose register. x4..x7 are pinned so secure
    // handlers that inspect extended registers (e.g. the RPMB caller-trusted
    // bit checked in w7) see deterministic values.
    register u64 x0 asm("x0") = function_id;
    register u64 x1 asm("x1") = arg0;
    register u64 x2 asm("x2") = arg1;
    register u64 x3 asm("x3") = arg2;
    register u64 x4 asm("x4") = arg3;
    register u64 x5 asm("x5") = arg4;
    register u64 x6 asm("x6") = arg5;
    register u64 x7 asm("x7") = arg6;
    asm volatile("smc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
                   "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                 :
                 : "memory", "cc");
    return {x0, x1, x2, x3};
#else
    (void)function_id; (void)arg0; (void)arg1; (void)arg2;
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    return {~0ull, 0, 0, 0};
#endif
}

#endif
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

inline bool has_rights(const cap::Snapshot* e, u32 rights) {
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

    syscall_metric_inc(local_syscall_metrics().faults);
    sched::record_exit(p, static_cast<long>(esr));
    sched::mark_current_dead(true);
}

// Keep steady-state IPC outside the large cold control/VM dispatch body.
// The caller has already validated process/execution ownership.
static SysResult ipc_dispatch_hot(Process* p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    syscall_metric_inc(local_syscall_metrics().ipc);
    switch (static_cast<Syscall>(nr)) {
    case SYS_NOTIFY: {
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_NOTIFY)) return err(E_PERM);
        const SysError rc = ipc::notify_object(static_cast<Doorbell*>(e->target));
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_WAIT_DOORBELL: {
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_WAIT)) return err(E_PERM);
        const SysError rc = ipc::wait_doorbell_object(e, a1, 0);
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_DOORBELL_READ: {
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_WAIT)) return err(E_PERM);
        return ok(static_cast<long>(__atomic_load_n(&static_cast<Doorbell*>(e->target)->counter, __ATOMIC_ACQUIRE)));
    }

    case SYS_NOTIFY_HANDOFF: {
        cap::Ref e = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_DOORBELL);
        if (!e) return err(E_NOTFOUND);
        if (!has_rights(e, RIGHT_NOTIFY)) return err(E_PERM);
        const SysError rc = ipc::notify_handoff_object(static_cast<Doorbell*>(e->target), a1);
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_REPLY_HANDOFF: {
        const SysError rc = ipc::reply_handoff(a0, p->pid);
        if (rc != E_OK) return err(rc);
        return ok(E_OK);
    }

    case SYS_WAIT_RING:
        return ok(ipc::wait_ring(a0, a1, a2, a3, p->pid));

    case SYS_NOTIFY_WAIT:
        return ok(ipc::exchange_ring(a0, a1, a2, a3, a4, p->pid));

    case SYS_REPLY_WAIT:
        return ok(ipc::exchange_ring(a0, a1, a2, a3, MX_IPC_EXCHANGE_REPLY, p->pid));



    default: return err(E_INVALID);
    }
}

[[gnu::noinline]] static SysResult syscall_dispatch_cold(Process* p, u64 nr, u64 a0, u64 a1,
                                                         u64 a2, u64 a3, u64 a4, u64 a5);

extern "C" SysResult syscall_dispatch(u64 nr, u64 a0, u64 a1, u64 a2,
                                      u64 a3, u64 a4, u64 a5) {
#ifdef ANKO_BOARD_HUAWEI_ANNE
    anne::boot_syscall(nr);
#endif
    (void)a5;
    syscall_metric_inc(local_syscall_metrics().calls);

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
    if (nr == SYS_CLOCK_NS) return ok(static_cast<long>(sched::sched_clock_ns()));
    if (nr == SYS_YIELD) { sched::yield(); return ok(E_OK); }
    if ((nr >= SYS_NOTIFY && nr <= SYS_WAIT_DOORBELL) || nr == SYS_DOORBELL_READ ||
        nr == SYS_NOTIFY_HANDOFF || nr == SYS_REPLY_HANDOFF ||
        (nr >= SYS_WAIT_RING && nr <= SYS_REPLY_WAIT))
        return ipc_dispatch_hot(p, nr, a0, a1, a2, a3, a4);
    return syscall_dispatch_cold(p, nr, a0, a1, a2, a3, a4, a5);
}

[[gnu::noinline]] static SysResult syscall_dispatch_cold(Process* p, u64 nr, u64 a0, u64 a1,
                                                         u64 a2, u64 a3, u64 a4, u64 a5) {
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
        if (a4 & ~(MX_SPAWN_SUSPENDED | MX_SPAWN_KILL_WITH_SUPERVISOR)) return err(E_INVALID);
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

    case SYS_SUPERVISOR_DELEGATE: {
        auto authority = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_SUPERVISOR);
        auto target = cap::lookup(*p->cap_table, static_cast<CapHandle>(a1), CAP_PROCESS);
        if (!authority || !target || !has_rights(authority, RIGHT_SPAWN) ||
            !has_rights(target, RIGHT_SPAWN)) return err(E_PERM);
        auto* parent = static_cast<Process*>(authority->target);
        auto* child = static_cast<Process*>(target->target);
        if (!parent || !child || parent->dying || child->dying) return err(E_DEAD);
        if (child->supervisor_identity != sched::process_identity(*parent)) return err(E_PERM);
        const auto h = cap::alloc(*p->cap_table, CAP_SUPERVISOR, child,
                                  RIGHT_READ | RIGHT_SPAWN | RIGHT_DUP);
        return h ? ok(static_cast<long>(h)) : err(E_OVERFLOW);
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
        syscall_metric_inc(local_syscall_metrics().ipc);
        CapHandle h = ipc::ring_create(p->pid, static_cast<u32>(a0), static_cast<u32>(a1));
        if (h == kNullCap) return err(E_NOMEM);
        return ok(static_cast<long>(h));
    }

    case SYS_RING_SHARE: {
        syscall_metric_inc(local_syscall_metrics().ipc);
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
        syscall_metric_inc(local_syscall_metrics().ipc);
        if (a0 && p->principal_class != PRINCIPAL_NATIVE_TRUSTED && p->principal_class != PRINCIPAL_KERNEL)
            return err(E_PERM);
        CapHandle h = ipc::doorbell_create(p->pid, static_cast<u32>(a0));
        if (h == kNullCap) return err(E_NOMEM);
        return ok(static_cast<long>(h));
    }

    case SYS_DOORBELL_SHARE: {
        syscall_metric_inc(local_syscall_metrics().ipc);
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




    case SYS_SUPERVISOR_BIND_DOORBELL: {
        auto authority = cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_SUPERVISOR);
        if (!authority || !has_rights(authority, RIGHT_READ | RIGHT_SPAWN)) return err(E_PERM);
        // A delegated supervisor may bind its own exit stream, never replace
        // another process's wake destination through a borrowed capability.
        if (authority->target != p) return err(E_PERM);
        auto doorbell = a1 ? cap::lookup(*p->cap_table, static_cast<CapHandle>(a1), CAP_DOORBELL)
                           : cap::Ref{};
        if (a1 && (!doorbell || !has_rights(doorbell, RIGHT_NOTIFY))) return err(E_PERM);
        return ok(sched::bind_exit_doorbell(*p,
            doorbell ? static_cast<Doorbell*>(doorbell->target) : nullptr));
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
        if (!a0 || a1 < sizeof(MxSchedContext)) return err(E_INVALID);
        MxSchedContext request{};
        if (!copy_from_user(ua, &request, a0, sizeof(request))) return err(E_FAULT);
        if (request.band >= NUM_BANDS) return err(E_INVALID);
        // Translate policy explicitly: the kernel's aligned atomic budget
        // window has a different layout from the stable userspace request.
        SchedContext ctx{};
        ctx.principal_id = request.principal_id;
        ctx.owner_thread_or_vcpu = request.owner_thread_or_vcpu;
        ctx.band = static_cast<SchedBand>(request.band);
        ctx.weight = request.weight;
        ctx.budget_ns = request.budget_ns;
        ctx.period_ns = request.period_ns;
        ctx.deadline_ns = request.deadline_ns;
        ctx.core_mask = request.core_mask;
        ctx.home_cpu = request.home_cpu;
        ctx.flags = request.flags;
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
        syscall_metric_inc(local_syscall_metrics().debug_puts);
#ifdef ANKO_BOARD_HUAWEI_ANNE
        anne::boot_log_write(s, len);
#else
        for (u64 i = 0; i < len; ++i) libk::putchar(s[i]);
#endif
        return ok(E_OK);
    }

    case SYS_CLOCK_NS: return ok(static_cast<long>(sched::sched_clock_ns()));

    // Per-principal IPC wake policy: (op, arg). A process can only address its
    // own principal, and the kernel clamps the budget, so a stale or hostile
    // profile can never turn a wait into an unbounded spin.
    case SYS_IPC_POLICY: {
        Thread* cur = sched::current();
        if (!cur) return err(E_DEAD);
        const u32 principal_id = cur->principal_id;
        switch (a0) {
        case 0: return ok(static_cast<long>(sched::ipc_poll_budget(principal_id)));
        case 1: return ok(static_cast<long>(sched::ipc_poll_budget_set(
                    principal_id, static_cast<u32>(a1))));
        case 2: return ok(static_cast<long>(sched::ipc_park_count(principal_id)),
                          static_cast<long>(sched::ipc_wake_count(principal_id)));
        // Read-only aggregate for a direct child: the supervisor already owns
        // the child's lifetime, so exposing its two counters grants no new
        // authority and no way to write anything.
        case 3: {
            const u32 target_pid = static_cast<u32>(a1);
            if (!p || !target_pid) return err(E_INVALID);
            object::Ref<Process> child = sched::find_process(target_pid);
            if (!child) return err(E_NOTFOUND);
            if (child->supervisor_pid != p->pid) return err(E_PERM);
            return ok(static_cast<long>(sched::ipc_park_count(child->principal_id)),
                      static_cast<long>(sched::ipc_wake_count(child->principal_id)));
        }
        default: return err(E_INVALID);
        }
    }

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
        case MX_DIAG_HEALTH: {
            if (a1 || a3 < sizeof(MxDiagHealth)) return err(E_INVALID);
            MxDiagHealth info{};
            info.version = MX_DIAG_VERSION;
            info.sampled_ns = sched::sched_clock_ns();
            info.online_mask = sched::online_cpu_mask();
            info.busy_ns = sched::busy_runtime_ns();
            info.total_bytes = mm::total_page_count() * PAGE_SIZE;
            info.used_bytes = mm::allocated_page_count() * PAGE_SIZE;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
            const libk::Mmio pmic{reinterpret_cast<volatile u8*>(anne::kDebugPmicBase), PAGE_SIZE};
            if (anne::battery::voltage_mv(pmic, info.battery_mv))
                info.flags |= MX_HEALTH_BATTERY_VOLTAGE;
#endif
            return copy_to_user(ua, a2, &info, sizeof(info)) ? ok(sizeof(info)) : err(E_FAULT);
        }
        case MX_DIAG_RUNTIME: {
            if (!a1 || a1 > MX_DIAG_PROCESS_LIMIT || a3 < sizeof(MxDiagRuntimeBatch))
                return err(E_INVALID);
            MxDiagRuntimeBatch info{};
            if (!sched::process_runtime_snapshot(static_cast<u32>(a1), &info)) return err(E_INVALID);
            return copy_to_user(ua, a2, &info, sizeof(info)) ? ok(sizeof(info)) : err(E_FAULT);
        }
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
        case MX_DIAG_EL3: {
            if (a1 > MX_DIAG_EL3_MAX_INDEX) return err(E_INVALID);
            // Apply board availability to every operation, including the raw
            // operations below. Compiled-out native bodies must not return a
            // successful record containing caller-supplied result fields.
#if !defined(ANKO_BOARD_HUAWEI_ANNE) || defined(ANKO_ANNE_DIAG_TEST)
            if (a1 != 0) return err(E_NOTIMPL);
#endif
            if (a1 == 29 || a1 == 30) {
                if (a3 < sizeof(MxDiagEl3Rpmb)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE)) return err(E_FAULT);
                MxDiagEl3Rpmb info{};
                if (!copy_from_user(ua, &info, a2, sizeof(info))) return err(E_FAULT);
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                const u32 action = info.action;
                info = {};
                const anne::rpmb_diag::Storage storage{anne::emmc::prepare,
                    anne::emmc::service, anne::emmc::provision_key,
                    anne::emmc::write_pending, anne::emmc::key_info, anne::emmc::delay_ms};
                anne::rpmb_diag::execute(action, info,
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress),
                    [](u64 fid, u64 x1, u64 x2, u64 x3) { return rpmb_call(fid, x1, x2, x3).x0; }, &storage);
#endif
                return copy_to_user(ua, a2, &info, sizeof(info)) ? ok(sizeof(info)) : err(E_FAULT);
            }
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
            bool allow_rpmb_producer = false;
            if (a1 == 25) {
                if (a3 < sizeof(MxDiagEl3Svc) ||
                    !user_range_ok(ua, a2, sizeof(MxDiagEl3Svc), RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Svc requested{};
                if (!copy_from_user(ua, &requested, a2, sizeof(requested))) return err(E_FAULT);
                // Keep arbitrary EL3 research calls blocked while the RPMB
                // window is owned. The known HISEE producer is the exception:
                // it must run concurrently so its SGI 5 reaches the service.
                allow_rpmb_producer = anne::rpmb_diag::producer_allowed(requested.fid) &&
                                       requested.x1 == 0x13;
            }
            anne::rpmb_diag::ResearchAccess research;
            if (!allow_rpmb_producer && !research) return err(E_BUSY);
#endif
            // Raw ops 25..28 deliberately skip the cross-CPU transaction lock: they
            // exist so a window or secure call stuck on another CPU can still be
            // observed (26) and driven (27/28) from the rest of the system, and so the
            // sacrificial caller (25) never leaves other CPUs blocked on the lock.
            if (a1 == 25) {
                if (a3 < sizeof(MxDiagEl3Svc)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Svc sv{};
                if (!copy_from_user(ua, &sv, a2, sizeof(sv))) return err(E_FAULT);
                sv.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                store64(0x6900, 0x454C335000000000ull | 0xD1ull);
                store64(0x6908, sv.fid);
                store64(0x6910, sv.x1);
                store64(0x6918, sv.x2);
                asm volatile("dsb sy" ::: "memory");
                const SmcWords r =
                    rpmb_call(sv.fid, sv.x1, sv.x2, sv.x3, sv.x4, sv.x5, sv.x6, sv.x7);
                sv.r0 = r.x0;
                sv.r1 = r.x1;
                sv.r2 = r.x2;
                sv.r3 = r.x3;
                store64(0x6900, 0x454C335000000000ull | 0xD2ull);
                store64(0x6908, sv.r0);
                store64(0x6910, sv.r1);
                store64(0x6918, sv.r2);
                asm volatile("dsb sy" ::: "memory");
                sv.crumb_stage = 0xD0ull;
#endif
                return copy_to_user(ua, a2, &sv, sizeof(sv)) ? ok(sizeof(sv)) : err(E_FAULT);
            }
            if (a1 == 26) {
                if (a3 < sizeof(MxDiagEl3Win)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Win wn{};
                if (!copy_from_user(ua, &wn, a2, sizeof(wn))) return err(E_FAULT);
                wn.version = MX_DIAG_VERSION;
                libk::memset(wn.words, 0, sizeof(wn.words));
                wn.aborted = 0;
                wn.crumb_stage = 0;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                wn.crumb_stage = 0xD3ull;
                if (wn.off > anne::kRpmbRequestSize - sizeof(wn.words)) {
                    wn.aborted = 1;
                } else {
                    const volatile u8* const base =
                        reinterpret_cast<const volatile u8*>(anne::kRpmbRequestAddress);
                    for (unsigned w = 0; w < 8; ++w) {
                        u64 value = 0;
                        for (unsigned i = 0; i < 8; ++i)
                            value |= static_cast<u64>(base[wn.off + w * 8 + i]) << (8 * i);
                        wn.words[w] = value;
                    }
                }
#endif
                return copy_to_user(ua, a2, &wn, sizeof(wn)) ? ok(sizeof(wn)) : err(E_FAULT);
            }
            if (a1 == 27) {
                if (a3 < sizeof(MxDiagEl3Wput)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Wput wp{};
                if (!copy_from_user(ua, &wp, a2, sizeof(wp))) return err(E_FAULT);
                wp.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                wp.aborted = 0;
                wp.readback = 0;
                wp.crumb_stage = 0xD4ull;
                if (wp.off > anne::kRpmbRequestSize - sizeof(wp.val)) {
                    wp.aborted = 1;
                } else {
                    volatile u8* const tgt =
                        reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress + wp.off);
                    for (unsigned i = 0; i < 8; ++i)
                        tgt[i] = static_cast<u8>(wp.val >> (8 * i));
                    asm volatile("dsb sy" ::: "memory");
                    u64 rb = 0;
                    for (unsigned i = 0; i < 8; ++i)
                        rb |= static_cast<u64>(tgt[i]) << (8 * i);
                    wp.readback = rb;
                }
#endif
                return copy_to_user(ua, a2, &wp, sizeof(wp)) ? ok(sizeof(wp)) : err(E_FAULT);
            }
            if (a1 == 28) {
                if (a3 < sizeof(MxDiagEl3Wmemset)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Wmemset wm{};
                if (!copy_from_user(ua, &wm, a2, sizeof(wm))) return err(E_FAULT);
                wm.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                wm.aborted = 0;
                wm.crumb_stage = 0xD5ull;
                if (!anne::rpmb::contains(0, anne::kRpmbRequestSize, wm.off, wm.len)) {
                    wm.aborted = 1;
                } else {
                    volatile u8* const base =
                        reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress + wm.off);
                    const u64 val = wm.val;
                    u64 i = 0;
                    for (; i + 8 <= wm.len; i += 8) {
                        for (unsigned b = 0; b < 8; ++b)
                            base[i + b] = static_cast<u8>(val >> (8 * b));
                    }
                    for (; i < wm.len; ++i)
                        base[i] = static_cast<u8>(val);
                    asm volatile("dsb sy" ::: "memory");
                }
#endif
                return copy_to_user(ua, a2, &wm, sizeof(wm)) ? ok(sizeof(wm)) : err(E_FAULT);
            }
            if (a1 == 29) {
                // Send a wake SGI to a CPU. Used to test whether a core held
                // in EL3 is sitting in an interrupt-enabled wait.
                if (a3 < sizeof(u64)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, sizeof(u64), RIGHT_READ))
                    return err(E_FAULT);
                u64 cpu = 0;
                if (!copy_from_user(ua, &cpu, a2, sizeof(cpu))) return err(E_FAULT);
                if (cpu >= MX_HOST_MAX_CPUS) return err(E_INVALID);
                smp::send_wake_ipi(static_cast<u32>(cpu));
                return ok(0);
            }
            if (a1 > 24) return err(E_INVALID);
            // Validate the destination before touching shared memory or firmware.
            if (!user_range_ok(ua, a2, a3, RIGHT_WRITE)) return err(E_FAULT);
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
            // Registration and window access form one transaction across CPUs.
            kernel::LockGuard el3_guard(g_el3_lock);
#endif
            if (a1 == 2) {
                if (a3 < sizeof(MxDiagEl3Recon)) return err(E_INVALID);
                MxDiagEl3Recon recon{};
                recon.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                // Raw window snapshots retained for research compatibility.
                // Linux does not establish callback/status semantics for these
                // offsets. Nothing is written and no SMC is issued here.
                recon.window_base = 0x34a29000ull;
                recon.window_size = 0x8000ull;
                const volatile u8* const win =
                    reinterpret_cast<const volatile u8*>(anne::kRpmbRequestAddress);
                auto load64 = [&](u64 off) -> u64 {
                    u64 value = 0;
                    const volatile u8* p = win + off;
                    for (unsigned i = 0; i < 8; ++i)
                        value |= static_cast<u64>(p[i]) << (8 * i);
                    return value;
                };
                recon.base_6000 = load64(0x6000);
                recon.slot_6c30 = load64(0x6C30);
                recon.slot_6c38 = load64(0x6C38);
                recon.slot_6c40 = load64(0x6C40);
                recon.slot_6c48 = load64(0x6C48);
                recon.status_6c42 = load64(0x6C42) & 0xFFFFull;
                recon.bl31_lo = 0x35e00000ull;
                recon.bl31_hi = 0x35ec0000ull;
                recon.crumb[0] = load64(0x6900);
                recon.crumb[1] = load64(0x6908);
                recon.crumb[2] = load64(0x6910);
                recon.crumb[3] = load64(0x6918);
                {
                    u64 v = 0;
                    asm volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(v));
                    recon.isar0 = v;
                }
                for (u64 off = 0; off + 8 <= recon.window_size; off += 8) {
                    const u64 value = load64(off);
                    if (value >= recon.bl31_lo && value < recon.bl31_hi) {
                        if (recon.ptr_count < 8) {
                            recon.ptr_slots[recon.ptr_count][0] = off;
                            recon.ptr_slots[recon.ptr_count][1] = value;
                        }
                        ++recon.ptr_count;
                    }
                }
#endif
                return copy_to_user(ua, a2, &recon, sizeof(recon)) ? ok(sizeof(recon))
                                                                    : err(E_FAULT);
            }
            if (a1 == 3) {
                // Linux's rpmb_work_routine completes an actual MMC/UFS request
                // and passes its result in x1. There is no Linux callback field
                // at +0x6c38, so clearing it cannot guard a standalone DONE call.
                // No storage backend/pending-request lifecycle exists here yet.
                return err(E_NOTIMPL);
            }
            if (a1 == 4) {
                if (a3 < sizeof(MxDiagEl3Bl31)) return err(E_INVALID);
                MxDiagEl3Bl31 probe{};
                probe.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                // Breadcrumbs first: if any secure read faults, the watchdog
                // resets and these words survive in the request window for
                // post-recovery inspection via el3-recon.
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                auto crumb = [&](u64 stage, u64 v0, u64 v1, u64 v2) {
                    store64(0x6900, 0x454C335000000000ull | stage);
                    store64(0x6908, v0);
                    store64(0x6910, v1);
                    store64(0x6918, v2);
                    asm volatile("dsb sy" ::: "memory");
                };
                crumb(1, 0, 0, 0);
                probe.tick_before = drivers::timer::tick_count();
                probe.control_addr = 0x34a29000ull;
                probe.control_value =
                    *reinterpret_cast<const volatile u32*>(0x34a29000ull);
                crumb(2, probe.control_value, 0, 0);
                probe.bl31_addr = 0x35e00000ull;
                crumb(3, 0x35e00000ull, 0, 0);
                probe.bl31_words[0] =
                    *reinterpret_cast<const volatile u32*>(0x35e00000ull);
                probe.bl31_words[1] =
                    *reinterpret_cast<const volatile u32*>(0x35e00004ull);
                probe.bl31_words[2] =
                    *reinterpret_cast<const volatile u32*>(0x35e00008ull);
                probe.bl31_words[3] =
                    *reinterpret_cast<const volatile u32*>(0x35e0000cull);
                probe.bl31_read_ok = 1;
                crumb(4, probe.bl31_words[0], probe.bl31_words[1], probe.bl31_words[2]);
                probe.teeos_addr = 0x36208000ull;
                crumb(5, 0x36208000ull, 0, 0);
                probe.teeos_words[0] =
                    *reinterpret_cast<const volatile u32*>(0x36208000ull);
                probe.teeos_words[1] =
                    *reinterpret_cast<const volatile u32*>(0x36208004ull);
                probe.teeos_read_ok = 1;
                crumb(6, probe.teeos_words[0], probe.teeos_words[1], 0);
                probe.tick_after = drivers::timer::tick_count();
                probe.crumb_stage = 6;
#endif
                return copy_to_user(ua, a2, &probe, sizeof(probe)) ? ok(sizeof(probe))
                                                                   : err(E_FAULT);
            }
            if (a1 == 5) {
                if (a3 < sizeof(MxDiagEl3Gv)) return err(E_INVALID);
                MxDiagEl3Gv gv{};
                gv.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                auto crumb = [&](u64 stage, u64 v0) {
                    store64(0x6900, 0x454C335000000000ull | stage);
                    store64(0x6908, v0);
                    asm volatile("dsb sy" ::: "memory");
                };
                gv.tick_before = drivers::timer::tick_count();
                crumb(1, 0);
                gv.replay_x1 = 0xFFFC6100ull;
                gv.replay_ret = rpmb_call(0xc500aa01ull, 0xFFFC6100ull, 4, 0x55bbcce0ull).x0;
                crumb(2, gv.replay_ret);
                gv.oracle_expected = 0x5A420000ull;
                gv.oracle_ret = rpmb_call(0xc500aa01ull, 0x34a2fc40ull, 4, 0x55bbcce0ull).x0;
                gv.oracle_match =
                    ((gv.oracle_ret & 0xFFFFFFFFull) == gv.oracle_expected) ? 1ull : 0ull;
                crumb(3, gv.oracle_ret);
                if (gv.oracle_match) {
                    // The prize: first word of the running BL31, read at EL3.
                    gv.bl31_ret = rpmb_call(0xc500aa01ull, 0x35e00000ull, 4, 0x55bbcce0ull).x0;
                    crumb(4, gv.bl31_ret);
                    gv.crumb_stage = 4;
                } else {
                    gv.aborted = 1;
                    gv.crumb_stage = 9;
                }
                gv.tick_after = drivers::timer::tick_count();
#endif
                return copy_to_user(ua, a2, &gv, sizeof(gv)) ? ok(sizeof(gv)) : err(E_FAULT);
            }
            if (a1 == 6) {
                if (a3 < sizeof(MxDiagEl3Aa)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Aa aa{};
                if (!copy_from_user(ua, &aa, a2, sizeof(aa))) return err(E_FAULT);
                aa.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                if (aa.idx > 31) {
                    aa.aborted = 1;
                } else {
                    volatile u8* const request =
                        reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                    auto store64 = [&](u64 off, u64 value) {
                        volatile u8* p = request + off;
                        for (unsigned i = 0; i < 8; ++i)
                            p[i] = static_cast<u8>(value >> (8 * i));
                    };
                    store64(0x6900, 0x454C335000000000ull | (0xA0ull + aa.idx));
                    store64(0x6910, aa.x1);
                    store64(0x6918, aa.x2);
                    asm volatile("dsb sy" ::: "memory");
                    aa.cookie = 0x55bbcce0ull | (aa.idx & 0x1Full);
                    const SmcWords r =
                        rpmb_call(0xc500aa01ull, aa.x1, aa.x2, aa.cookie);
                    aa.r0 = r.x0;
                    aa.r1 = r.x1;
                    aa.r2 = r.x2;
                    aa.r3 = r.x3;
                    store64(0x6908, aa.r0);
                    store64(0x6900, 0x454C335000000000ull | (0xB0ull + aa.idx));
                    asm volatile("dsb sy" ::: "memory");
                    aa.crumb_stage = 0xC0ull + aa.idx;
                }
#endif
                return copy_to_user(ua, a2, &aa, sizeof(aa)) ? ok(sizeof(aa))
                                                             : err(E_FAULT);
            }
            if (a1 == 7) {
                if (a3 < sizeof(MxDiagEl3Share)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Share sh{};
                if (!copy_from_user(ua, &sh, a2, sizeof(sh))) return err(E_FAULT);
                sh.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                if (sh.offset >= 0x8000ull || sh.len == 0 || sh.len > 64) {
                    sh.aborted = 1;
                } else {
                    const volatile u8* base =
                        reinterpret_cast<const volatile u8*>(0x34a21000ull);
                    if (sh.offset + sh.len > 0x8000ull) sh.len = 0x8000ull - sh.offset;
                    for (u64 i = 0; i < sh.len; ++i)
                        sh.data[i] = base[sh.offset + i];
                }
#endif
                return copy_to_user(ua, a2, &sh, sizeof(sh)) ? ok(sizeof(sh))
                                                             : err(E_FAULT);
            }
            if (a1 == 8) {
                if (a3 < sizeof(MxDiagEl3Ra)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Ra ra{};
                if (!copy_from_user(ua, &ra, a2, sizeof(ra))) return err(E_FAULT);
                ra.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                if (ra.target < 0x34a21000ull || ra.target > 0x34a31000ull - 64ull) {
                    ra.aborted = 1;
                } else {
                    volatile u8* const request =
                        reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                    const volatile u8* src =
                        reinterpret_cast<const volatile u8*>(&kAnneRpmbConfig);
                    volatile u8* tgt = reinterpret_cast<volatile u8*>(ra.target);
                    for (u64 i = 0; i < sizeof(kAnneRpmbConfig); ++i) tgt[i] = src[i];
                    asm volatile("dsb sy" ::: "memory");
                    const SmcWords r1 =
                        rpmb_call(kRpmbRequestAddressSmc, ra.target, 0, 0);
                    ra.attempt_r0 = r1.x0;
                    ra.attempt_r1 = r1.x1;
                    ra.attempt_r2 = r1.x2;
                    ra.attempt_r3 = r1.x3;
                    for (u64 i = 0; i < sizeof(kAnneRpmbConfig); ++i) request[i] = src[i];
                    asm volatile("dsb sy" ::: "memory");
                    const SmcWords r2 = rpmb_call(kRpmbRequestAddressSmc, 0x34a29000ull, 0, 0);
                    ra.restore_r0 = r2.x0;
                    ra.restore_r1 = r2.x1;
                    ra.restore_r2 = r2.x2;
                    ra.restore_r3 = r2.x3;
                }
#endif
                return copy_to_user(ua, a2, &ra, sizeof(ra)) ? ok(sizeof(ra))
                                                             : err(E_FAULT);
            }
            if (a1 == 9) {
                if (a3 < sizeof(MxDiagEl3Nn)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Nn nn{};
                if (!copy_from_user(ua, &nn, a2, sizeof(nn))) return err(E_FAULT);
                nn.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                {
                    const SmcWords r = rpmb_call(0xc5009900ull, nn.x1, nn.x2, nn.x3);
                    nn.r0 = r.x0;
                    nn.r1 = r.x1;
                    nn.r2 = r.x2;
                    nn.r3 = r.x3;
                }
#endif
                return copy_to_user(ua, a2, &nn, sizeof(nn)) ? ok(sizeof(nn))
                                                             : err(E_FAULT);
            }
            if (a1 == 10) {
                if (a3 < sizeof(MxDiagEl3Teeos)) return err(E_INVALID);
                MxDiagEl3Teeos tt{};
                tt.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                store64(0x6900, 0x454C335000000000ull | 0xD1ull);
                asm volatile("dsb sy" ::: "memory");
                tt.addr = 0x36208000ull;
                tt.words[0] = *reinterpret_cast<const volatile u32*>(0x36208000ull);
                tt.words[1] = *reinterpret_cast<const volatile u32*>(0x36208004ull);
                tt.ok = 1;
                store64(0x6900, 0x454C335000000000ull | 0xD2ull);
                store64(0x6908, tt.words[0]);
                asm volatile("dsb sy" ::: "memory");
                tt.crumb_stage = 0xD2;
#endif
                return copy_to_user(ua, a2, &tt, sizeof(tt)) ? ok(sizeof(tt))
                                                             : err(E_FAULT);
            }
            if (a1 == 11) {
                if (a3 < sizeof(MxDiagEl3Cc)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Cc cc{};
                if (!copy_from_user(ua, &cc, a2, sizeof(cc))) return err(E_FAULT);
                cc.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                store64(0x6900, 0x454C335000000000ull | 0xC1ull);
                store64(0x6908, cc.x1);
                asm volatile("dsb sy" ::: "memory");
                const SmcWords r = rpmb_call(0xc500cc00ull, cc.x1, 0, 0);
                cc.r0 = r.x0; cc.r1 = r.x1; cc.r2 = r.x2; cc.r3 = r.x3;
                store64(0x6900, 0x454C335000000000ull | 0xC2ull);
                store64(0x6908, cc.r0);
                asm volatile("dsb sy" ::: "memory");
                cc.crumb_stage = 0xC2;
#endif
                return copy_to_user(ua, a2, &cc, sizeof(cc)) ? ok(sizeof(cc))
                                                             : err(E_FAULT);
            }
            if (a1 == 12) {
                if (a3 < sizeof(MxDiagEl3Cc2)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Cc2 cc{};
                if (!copy_from_user(ua, &cc, a2, sizeof(cc))) return err(E_FAULT);
                cc.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                store64(0x6900, 0x454C335000000000ull | 0xC1ull);
                store64(0x6908, cc.a);
                asm volatile("dsb sy" ::: "memory");
                const SmcWords ra = rpmb_call(0xc500cc00ull, cc.a, 0, 0);
                cc.ra0 = ra.x0; cc.ra1 = ra.x1; cc.ra2 = ra.x2; cc.ra3 = ra.x3;
                store64(0x6900, 0x454C335000000000ull | 0xC2ull);
                store64(0x6908, cc.ra0);
                asm volatile("dsb sy" ::: "memory");
                store64(0x6900, 0x454C335000000000ull | 0xC3ull);
                store64(0x6908, cc.b);
                asm volatile("dsb sy" ::: "memory");
                const SmcWords rb = rpmb_call(0xc500cc00ull, cc.b, 0, 0);
                cc.rb0 = rb.x0; cc.rb1 = rb.x1; cc.rb2 = rb.x2; cc.rb3 = rb.x3;
                store64(0x6900, 0x454C335000000000ull | 0xC4ull);
                store64(0x6908, cc.rb0);
                asm volatile("dsb sy" ::: "memory");
                cc.crumb_stage = 0xC4;
#endif
                return copy_to_user(ua, a2, &cc, sizeof(cc)) ? ok(sizeof(cc))
                                                             : err(E_FAULT);
            }
            if (a1 == 13) {
                if (a3 < sizeof(MxDiagEl3AaSweep)) return err(E_INVALID);
                MxDiagEl3AaSweep sw{};
                sw.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                for (u64 i = 0; i < 32; ++i) {
                    store64(0x6900, 0x454C335000000000ull | 0xE0ull);
                    store64(0x6908, i);
                    asm volatile("dsb sy" ::: "memory");
                    sw.rows[i] = rpmb_call(0xc500aa01ull, 0, 0, 0x55bbcce0ull | i).x0;
                }
                store64(0x6900, 0x454C335000000000ull | 0xE1ull);
                asm volatile("dsb sy" ::: "memory");
                sw.rows[32] = rpmb_call(0xc500aa01ull, 0xabcde0ull, 2, 0x55bbcce7ull).x0;
                sw.rows[33] = rpmb_call(0xc500aa01ull, 0x34a21100ull, 0, 0x55bbcceAull).x0;
                sw.rows[34] = rpmb_call(0xc500aa01ull, 0x34a21100ull, 0x1000, 0x55bbcceEull).x0;
                sw.rows[35] = rpmb_call(0xc500aa01ull, 0x34a21100ull, 0x1000, 0x55bbcceFull).x0;
                store64(0x6900, 0x454C335000000000ull | 0xE2ull);
                store64(0x6908, 36);
                asm volatile("dsb sy" ::: "memory");
                sw.crumb_stage = 0xE2;
#endif
                return copy_to_user(ua, a2, &sw, sizeof(sw)) ? ok(sizeof(sw))
                                                              : err(E_FAULT);
            }
            if (a1 == 14) {
                if (a3 < sizeof(MxDiagEl3Socp)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Socp sc{};
                if (!copy_from_user(ua, &sc, a2, sizeof(sc))) return err(E_FAULT);
                sc.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                volatile u8* const dmss =
                    reinterpret_cast<volatile u8*>(0x34a21100ull);
                for (u64 i = 0; i < 4; ++i) {
                    const volatile u8* src =
                        reinterpret_cast<const volatile u8*>(&sc.d[i]);
                    for (unsigned j = 0; j < 8; ++j)
                        dmss[i * 8 + j] = src[j];
                }
                asm volatile("dsb sy" ::: "memory");
                store64(0x6900, 0x454C335000000000ull | 0xF1ull);
                asm volatile("dsb sy" ::: "memory");
                if (sc.mode == 2 || sc.mode == 3) {
                    const SmcWords r8r = rpmb_call(0xc500aa01ull, 0x34a21100ull, sc.p1, 0x55bbcce8ull);
                    sc.r8[0] = r8r.x0; sc.r8[1] = r8r.x1; sc.r8[2] = r8r.x2; sc.r8[3] = r8r.x3;
                    store64(0x6900, 0x454C335000000000ull | 0xF2ull);
                    store64(0x6908, sc.r8[0]);
                    asm volatile("dsb sy" ::: "memory");
                    store64(0x6900, 0x454C335000000000ull | 0xF5ull);
                    asm volatile("dsb sy" ::: "memory");
                }
                if (sc.mode == 3 || sc.mode == 4) {
                    const SmcWords rAr = rpmb_call(0xc500aa01ull, 0x34a21100ull, sc.p2, 0x55bbcceAull);
                    sc.rA[0] = rAr.x0; sc.rA[1] = rAr.x1; sc.rA[2] = rAr.x2; sc.rA[3] = rAr.x3;
                    store64(0x6900, 0x454C335000000000ull | 0xF3ull);
                    store64(0x6908, sc.rA[0]);
                    asm volatile("dsb sy" ::: "memory");
                }
                for (u64 i = 0; i < 16; ++i) {
                    u64 v = 0;
                    for (unsigned j = 0; j < 8; ++j)
                        v |= static_cast<u64>(dmss[i * 8 + j]) << (8 * j);
                    sc.readback[i] = v;
                }
                store64(0x6900, 0x454C335000000000ull | 0xF4ull);
                asm volatile("dsb sy" ::: "memory");
                sc.crumb_stage = 0xF4;
#endif
                return copy_to_user(ua, a2, &sc, sizeof(sc)) ? ok(sizeof(sc))
                                                              : err(E_FAULT);
            }
            if (a1 == 15) {
                if (a3 < sizeof(MxDiagEl3Put)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Put pt{};
                if (!copy_from_user(ua, &pt, a2, sizeof(pt))) return err(E_FAULT);
                pt.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                pt.aborted = 0;
                pt.readback = 0;
                pt.crumb_stage = 0;
                if (!anne::rpmb::contains(0, anne::kRpmbSharedSize, pt.off, sizeof(pt.val))) {
                    pt.aborted = 1;
                } else {
                    volatile u8* const request =
                        reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                    auto store64 = [&](u64 off, u64 value) {
                        volatile u8* p = request + off;
                        for (unsigned i = 0; i < 8; ++i)
                            p[i] = static_cast<u8>(value >> (8 * i));
                    };
                    store64(0x6900, 0x454C335000000000ull | 0x1F1ull);
                    asm volatile("dsb sy" ::: "memory");
                    volatile u8* const tgt =
                        reinterpret_cast<volatile u8*>(0x34a21000ull + pt.off);
                    for (unsigned i = 0; i < 8; ++i)
                        tgt[i] = static_cast<u8>(pt.val >> (8 * i));
                    asm volatile("dsb sy" ::: "memory");
                    store64(0x6900, 0x454C335000000000ull | 0x1F2ull);
                    asm volatile("dsb sy" ::: "memory");
                    u64 rb = 0;
                    for (unsigned i = 0; i < 8; ++i)
                        rb |= static_cast<u64>(tgt[i]) << (8 * i);
                    pt.readback = rb;
                    pt.crumb_stage = 0x1F2;
                }
#endif
                return copy_to_user(ua, a2, &pt, sizeof(pt)) ? ok(sizeof(pt))
                                                              : err(E_FAULT);
            }
            if (a1 == 16) {
                if (a3 < sizeof(MxDiagEl3Poke)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Poke pk{};
                if (!copy_from_user(ua, &pk, a2, sizeof(pk))) return err(E_FAULT);
                pk.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                auto store16 = [&](u64 off, u16 value) {
                    volatile u8* p = request + off;
                    p[0] = static_cast<u8>(value);
                    p[1] = static_cast<u8>(value >> 8);
                };
                auto load64 = [&](u64 off) -> u64 {
                    u64 value = 0;
                    const volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        value |= static_cast<u64>(p[i]) << (8 * i);
                    return value;
                };
                const volatile u8* src =
                    reinterpret_cast<const volatile u8*>(&kAnneRpmbConfig);
                for (u64 i = 0; i < sizeof(kAnneRpmbConfig); ++i) request[i] = src[i];
                asm volatile("dsb sy" ::: "memory");
                pk.registered =
                    rpmb_call(kRpmbRequestAddressSmc, 0x34a29000ull, 0, 0).x0;
                if (pk.registered == 0) {
                    pk.slot_pre = load64(0x6C38);
                    store64(0x6C38, pk.cb);
                    store16(0x6C42, 0x5A42);
                    asm volatile("dsb sy" ::: "memory");
                    pk.slot_written = load64(0x6C38);
                    pk.status_set = load64(0x6C42) & 0xFFFFull;
                    if (pk.slot_written == pk.cb && pk.status_set == 0x5A42ull) {
                        store64(0x6900, 0x454C335000000000ull | 0xD1ull);
                        store64(0x6908, pk.cb);
                        store64(0x6910, pk.slot_written);
                        store64(0x6918, pk.status_set);
                        asm volatile("dsb sy" ::: "memory");
                        pk.tick_before = drivers::timer::tick_count();
                        const SmcWords r = rpmb_call(kRpmbRequestDoneSmc, 0, 0, 0);
                        pk.tick_after = drivers::timer::tick_count();
                        pk.call_x0 = r.x0;
                        pk.call_x1 = r.x1;
                        pk.call_x2 = r.x2;
                        pk.call_x3 = r.x3;
                        store64(0x6900, 0x454C335000000000ull | 0xD2ull);
                        store64(0x6908, pk.call_x0);
                        store64(0x6910, pk.call_x1);
                        store64(0x6918, pk.call_x2);
                    } else {
                        pk.aborted = 2;
                    }
                    asm volatile("dsb sy" ::: "memory");
                    pk.slot_post = load64(0x6C38);
                    pk.status_post = load64(0x6C42) & 0xFFFFull;
                    pk.crumb_stage = 0xD0ull;
                } else {
                    pk.aborted = 1;
                }
#endif
                return copy_to_user(ua, a2, &pk, sizeof(pk)) ? ok(sizeof(pk))
                                                             : err(E_FAULT);
            }
            if (a1 == 17) {
                if (a3 < sizeof(MxDiagEl3Svc)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Svc sv{};
                if (!copy_from_user(ua, &sv, a2, sizeof(sv))) return err(E_FAULT);
                sv.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                store64(0x6900, 0x454C335000000000ull | 0xE1ull);
                store64(0x6908, sv.fid);
                store64(0x6910, sv.x1);
                store64(0x6918, sv.x2);
                asm volatile("dsb sy" ::: "memory");
                const SmcWords r =
                    rpmb_call(sv.fid, sv.x1, sv.x2, sv.x3, sv.x4, sv.x5, sv.x6, sv.x7);
                sv.r0 = r.x0;
                sv.r1 = r.x1;
                sv.r2 = r.x2;
                sv.r3 = r.x3;
                store64(0x6900, 0x454C335000000000ull | 0xE2ull);
                store64(0x6908, sv.r0);
                store64(0x6910, sv.r1);
                store64(0x6918, sv.r2);
                asm volatile("dsb sy" ::: "memory");
                sv.crumb_stage = 0xE0ull;
#endif
                return copy_to_user(ua, a2, &sv, sizeof(sv)) ? ok(sizeof(sv))
                                                             : err(E_FAULT);
            }
            if (a1 == 18) {
                if (a3 < sizeof(MxDiagEl3Win)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Win wn{};
                if (!copy_from_user(ua, &wn, a2, sizeof(wn))) return err(E_FAULT);
                wn.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                wn.aborted = 0;
                libk::memset(wn.words, 0, sizeof(wn.words));
                wn.crumb_stage = 0;
                if (wn.off > anne::kRpmbRequestSize - sizeof(wn.words)) {
                    wn.aborted = 1;
                } else {
                    const volatile u8* base =
                        reinterpret_cast<const volatile u8*>(0x34a29000ull);
                    for (u64 i = 0; i < 64; ++i)
                        wn.words[i / 8] |= static_cast<u64>(base[wn.off + i]) << (8 * (i % 8));
                }
#endif
                return copy_to_user(ua, a2, &wn, sizeof(wn)) ? ok(sizeof(wn))
                                                              : err(E_FAULT);
            }
            if (a1 == 19) {
                if (a3 < sizeof(MxDiagEl3Wput)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Wput wp{};
                if (!copy_from_user(ua, &wp, a2, sizeof(wp))) return err(E_FAULT);
                wp.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                wp.aborted = 0;
                wp.readback = 0;
                wp.crumb_stage = 0;
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                auto store64 = [&](u64 off, u64 value) {
                    volatile u8* p = request + off;
                    for (unsigned i = 0; i < 8; ++i)
                        p[i] = static_cast<u8>(value >> (8 * i));
                };
                if (wp.off > 0x8000ull - 8ull) {
                    wp.aborted = 1;
                } else {
                    store64(0x6900, 0x454C335000000000ull | 0x1F7ull);
                    asm volatile("dsb sy" ::: "memory");
                    volatile u8* const tgt =
                        reinterpret_cast<volatile u8*>(0x34a29000ull + wp.off);
                    for (unsigned i = 0; i < 8; ++i)
                        tgt[i] = static_cast<u8>(wp.val >> (8 * i));
                    asm volatile("dsb sy" ::: "memory");
                    u64 rb = 0;
                    for (unsigned i = 0; i < 8; ++i)
                        rb |= static_cast<u64>(tgt[i]) << (8 * i);
                    wp.readback = rb;
                    store64(0x6900, 0x454C335000000000ull | 0x1F8ull);
                    wp.crumb_stage = 0x1F8;
                }
                asm volatile("dsb sy" ::: "memory");
#endif
                return copy_to_user(ua, a2, &wp, sizeof(wp)) ? ok(sizeof(wp))
                                                              : err(E_FAULT);
            }
            if (a1 == 20) {
                if (a3 < sizeof(MxDiagEl3Scan)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Scan sc{};
                if (!copy_from_user(ua, &sc, a2, sizeof(sc))) return err(E_FAULT);
                sc.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                sc.aborted = 0;
                sc.count = 0;
                libk::memset(sc.hits, 0, sizeof(sc.hits));
                sc.crumb_stage = 0;
                if ((sc.base & 7ull) != 0 || sc.len == 0 || sc.len > anne::kRpmbRequestSize ||
                    !anne::rpmb::contains(anne::kRpmbSharedBase, anne::kRpmbSharedSize,
                                          sc.base, sc.len)) {
                    sc.aborted = 1;
                } else {
                    const volatile u8* p0 =
                        reinterpret_cast<const volatile u8*>(sc.base);
                    for (u64 i = 0; i + 8 <= sc.len && sc.count < 20ull; i += 8) {
                        u64 v = 0;
                        for (unsigned j = 0; j < 8; ++j)
                            v |= static_cast<u64>(p0[i + j]) << (8 * j);
                        if (v >= sc.lo && v <= sc.hi) {
                            sc.hits[sc.count][0] = i;
                            sc.hits[sc.count][1] = v;
                            sc.count++;
                        }
                    }
                    sc.crumb_stage = 0x2E0ull;
                }
#endif
                return copy_to_user(ua, a2, &sc, sizeof(sc)) ? ok(sizeof(sc))
                                                             : err(E_FAULT);
            }
            if (a1 == 21) {
                if (a3 < sizeof(MxDiagEl3Scan2)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Scan2 sc{};
                if (!copy_from_user(ua, &sc, a2, sizeof(sc))) return err(E_FAULT);
                sc.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                sc.aborted = 0;
                sc.count = 0;
                libk::memset(sc.hits, 0, sizeof(sc.hits));
                sc.crumb_stage = 0;
                bool bad = (sc.nranges == 0 || sc.nranges > 8);
                for (u64 r = 0; r < sc.nranges && !bad; ++r) {
                    const u64 b = sc.ranges[r][0], l = sc.ranges[r][1];
                    if ((b & 7ull) != 0 || l == 0 || l > 0x200000ull) { bad = true; break; }
                    const bool ok = anne::rpmb::contains(0x400000ull, 0x2600000ull, b, l) ||
                        anne::rpmb::contains(anne::kRpmbSharedBase, anne::kRpmbSharedSize, b, l);
                    if (!ok) { bad = true; break; }
                }
                if (bad) {
                    sc.aborted = 1;
                } else {
                    for (u64 r = 0; r < sc.nranges && sc.count < 32ull; ++r) {
                        const u64 b = sc.ranges[r][0], l = sc.ranges[r][1];
                        const volatile u8* p0 = reinterpret_cast<const volatile u8*>(b);
                        for (u64 i = 0; i + 8 <= l && sc.count < 32ull; i += 8) {
                            u64 v = 0;
                            for (unsigned j = 0; j < 8; ++j)
                                v |= static_cast<u64>(p0[i + j]) << (8 * j);
                            if (v >= sc.lo && v <= sc.hi) {
                                sc.hits[sc.count][0] = r;
                                sc.hits[sc.count][1] = i;
                                sc.hits[sc.count][2] = v;
                                sc.count++;
                            }
                        }
                    }
                    sc.crumb_stage = 0x2F0ull;
                }
#endif
                return copy_to_user(ua, a2, &sc, sizeof(sc)) ? ok(sizeof(sc))
                                                             : err(E_FAULT);
            }
            if (a1 == 22) {
                if (a3 < sizeof(MxDiagEl3Rprobe)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Rprobe rp{};
                if (!copy_from_user(ua, &rp, a2, sizeof(rp))) return err(E_FAULT);
                rp.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                u64 v = 0, esr = 0, far = 0, elr = 0, faults = 0;
                rp.ok = anne::probe_read(rp.addr, &v, &esr, &far, &elr, &faults) ? 1u : 0u;
                rp.value = v;
                rp.esr = esr;
                rp.far = far;
                rp.elr = elr;
                rp.faults = faults;
#endif
                return copy_to_user(ua, a2, &rp, sizeof(rp)) ? ok(sizeof(rp)) : err(E_FAULT);
            }
            if (a1 == 23) {
                if (a3 < sizeof(MxDiagEl3Rscan)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Rscan rs{};
                if (!copy_from_user(ua, &rs, a2, sizeof(rs))) return err(E_FAULT);
                rs.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                rs.aborted = 0;
                rs.readable = 0;
                rs.faults = 0;
                rs.seg_count = 0;
                rs.first_count = 0;
                rs.esr_last = 0;
                if ((rs.base & 7ull) != 0 || rs.length == 0 ||
                    (rs.length & 7ull) != 0 || rs.length > MX_DIAG_EL3_RSCAN_MAX) {
                    rs.aborted = 1;
                } else {
                    for (u64 i = 0; i < rs.length; i += 8) {
                        u64 v = 0, esr = 0, faults = 0;
                        if (anne::probe_read(rs.base + i, &v, &esr, nullptr, nullptr, &faults)) {
                            if (rs.seg_count != 0 &&
                                rs.segments[rs.seg_count - 1][1] == rs.base + i) {
                                rs.segments[rs.seg_count - 1][1] = rs.base + i + 8;
                            } else if (rs.seg_count < 8) {
                                rs.segments[rs.seg_count][0] = rs.base + i;
                                rs.segments[rs.seg_count][1] = rs.base + i + 8;
                                ++rs.seg_count;
                            }
                            if (rs.first_count < 8 && v != 0) {
                                rs.first_values[rs.first_count][0] = rs.base + i;
                                rs.first_values[rs.first_count][1] = v;
                                ++rs.first_count;
                            }
                            ++rs.readable;
                        } else {
                            ++rs.faults;
                            rs.esr_last = esr;
                        }
                    }
                }
#endif
                return copy_to_user(ua, a2, &rs, sizeof(rs)) ? ok(sizeof(rs)) : err(E_FAULT);
            }
            if (a1 == 24) {
                if (a3 < sizeof(MxDiagEl3Rmap)) return err(E_INVALID);
                if (!user_range_ok(ua, a2, a3, RIGHT_READ | RIGHT_WRITE))
                    return err(E_FAULT);
                MxDiagEl3Rmap rm{};
                if (!copy_from_user(ua, &rm, a2, sizeof(rm))) return err(E_FAULT);
                rm.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
                rm.aborted = 0;
                rm.mapped_global = 0;
                rm.mapped_current = 0;
                rm.ok = 0;
                rm.value = 0;
                rm.esr = 0;
                rm.far = 0;
                rm.faults = 0;
                u64 size = rm.size ? rm.size : 0x1000ull;
                if ((rm.addr & 0xfffull) != 0 || size > 0x20000ull ||
                    (size & 0xfffull) != 0) {
                    rm.aborted = 1;
                } else {
                    rm.size = size;
                    if (mm::map_kernel_device_mmio(rm.addr, size)) rm.mapped_global = 1;
                    Process* p = cur_proc();
                    if (p && p->ttbr0) {
                        VmSpace sp{};
                        sp.pgd_phys = p->ttbr0;
                        sp.pid = p->pid;
                        if (mm::vm_map_device_range(sp, rm.addr, rm.addr, size, RIGHT_READ))
                            rm.mapped_current = 1;
                    }
                    u64 v = 0, esr = 0, far = 0, faults = 0;
                    rm.ok = anne::probe_read(rm.addr, &v, &esr, &far, nullptr, &faults) ? 1u : 0u;
                    rm.value = v;
                    rm.esr = esr;
                    rm.far = far;
                    rm.faults = faults;
                }
#endif
                return copy_to_user(ua, a2, &rm, sizeof(rm)) ? ok(sizeof(rm)) : err(E_FAULT);
            }
            if (a3 < sizeof(MxDiagEl3)) return err(E_INVALID);
            MxDiagEl3 info{};
            info.version = MX_DIAG_VERSION;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
            info.available = 1;
            // Hi6250 EMUI-8 global_ddr_map.h and the matching RPMB DT node:
            // BL31 shared memory starts at 0x34a21000 and the RPMB request
            // window is the 0x8000..0xffff subrange of that area.
            info.shared_base = anne::kRpmbSharedBase;
            info.request_address = anne::kRpmbRequestAddress;
            info.request_size = anne::kRpmbRequestSize;
            info.bl31_base = 0x35e00000ull;
            info.bl31_size = 0xc0000ull;
            // Linux's RPMB path has no callback address.  The exploit-slide
            // offset is Kirin970-specific and is intentionally not reported
            // as a Hi6250 property.
            info.callback_offset_from_request = 0;
            info.callback_address = 0;
            const SmcWords call_count = rpmb_call(kRpmbCallCount);
            const SmcWords uid = rpmb_call(kRpmbUid);
            const SmcWords version = rpmb_call(kRpmbVersion);
            info.call_count[0] = call_count.x0;
            info.call_count[1] = call_count.x1;
            info.call_count[2] = call_count.x2;
            info.call_count[3] = call_count.x3;
            info.uid[0] = uid.x0;
            info.uid[1] = uid.x1;
            info.uid[2] = uid.x2;
            info.uid[3] = uid.x3;
            info.service_version[0] = version.x0;
            info.service_version[1] = version.x1;
            info.service_version[2] = version.x2;
            info.service_version[3] = version.x3;
            if (a1 == 1) {
                // Match hisi_rpmb_device_init(): Linux copies this config into
                // the ioremap_wc() window before REQUEST_ADDR.  The entire
                // window is an identity Normal-NC mapping in Ame, so a DSB is
                // sufficient; no cached alias or explicit clean is allowed.
                volatile u8* const request =
                    reinterpret_cast<volatile u8*>(anne::kRpmbRequestAddress);
                const volatile u8* src = reinterpret_cast<const volatile u8*>(&kAnneRpmbConfig);
                for (u64 i = 0; i < sizeof(kAnneRpmbConfig); ++i) request[i] = src[i];
                asm volatile("dsb sy" ::: "memory");
                info.request_address_result =
                    rpmb_call(kRpmbRequestAddressSmc, info.request_address,
                              anne::rpmb::kBootDeviceEmmc, 0).x0;
            }
#endif
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
        case DBG_SYSCALL_COUNT:         return ok(static_cast<long>(syscall_metric_total(&SyscallMetrics::calls)));
        case DBG_IPC_SYSCALL_COUNT:     return ok(static_cast<long>(syscall_metric_total(&SyscallMetrics::ipc)));
        case DBG_CONTEXT_SWITCHES:      return ok(static_cast<long>(sched::context_switch_count()));
        case DBG_DEBUG_PUTS:            return ok(static_cast<long>(syscall_metric_total(&SyscallMetrics::debug_puts)));
        case DBG_EL0_FAULTS:            return ok(static_cast<long>(syscall_metric_total(&SyscallMetrics::faults)));
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
            const u64 refs = object::reference_count(e->object);
            const u64 retained = refs ? refs - 1 : 0;
            info.flags = (info.flags & ~MX_ARENA_REFERENCE_MASK) |
                static_cast<u32>((retained > 65535 ? 65535 : retained) << MX_ARENA_REFERENCE_SHIFT);
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
