// kernel/init/boot_caps.cc — assemble the initial capability set for init.
//
// Spec ref: §8 V0 Boot Flow (step 9), §7.1 Init

#include "anko/cap.h"
#include "anko/sched.h"
#include "anko/mm.h"
#include "anko/platform.h"
#include "anko/init.h"
#include "anko/vm.h"
#include "anko/ipc.h"
#include "anko/log.h"
#include "libk/printf.h"
#include "libk/string.h"

namespace kernel::init {

// DTB physical address — provided by arch/arm64/entry.S.
extern "C" PhysAddr g_dtb_phys;

namespace {

constexpr u32 kKernelLogRingOrder = 6;

} // namespace

void issue_boot_caps(Process& init_proc,
                     PhysAddr manifest_addr,
                     PhysAddr initrd_phys,
                     usize    initrd_size) {
    if (!init_proc.cap_table) return;

    auto* manifest = static_cast<MemoryObject*>(mm::heap_alloc(sizeof(MemoryObject), alignof(MemoryObject)));
    auto* initrd = static_cast<MemoryObject*>(mm::heap_alloc(sizeof(MemoryObject), alignof(MemoryObject)));
    if (!manifest || !initrd) return;

    const PhysAddr manifest_page = mm::alloc_page();
    if (!manifest_page) return;
    libk::memset(reinterpret_cast<void*>(manifest_page), 0, PAGE_SIZE);
    if (manifest_addr) {
        libk::memcpy(reinterpret_cast<void*>(manifest_page),
                     reinterpret_cast<const void*>(manifest_addr),
                     PAGE_SIZE);
    }
    manifest->phys_base = manifest_page;
    manifest->size = PAGE_SIZE;

    initrd->phys_base = initrd_phys;
    initrd->size = initrd_size;

    // 1) manifest cap
    cap::alloc(*init_proc.cap_table, CAP_PAGE, manifest, RIGHT_READ | RIGHT_MAP);

    // 2) initrd cap (read-only in V0)
    cap::alloc(*init_proc.cap_table, CAP_PAGE, initrd, RIGHT_READ | RIGHT_MAP | RIGHT_DUP);

    // 3) root supervisor cap
    cap::alloc(*init_proc.cap_table, CAP_SUPERVISOR, &init_proc,
               RIGHT_READ | RIGHT_KILL | RIGHT_SPAWN | RIGHT_REGISTER | RIGHT_DUP);

    // 4) nameservice placeholder cap
    cap::alloc(*init_proc.cap_table, CAP_NAMESERVICE, &init_proc,
               RIGHT_REGISTER | RIGHT_READ | RIGHT_DUP);

    // 5) UART cap
    cap::alloc(*init_proc.cap_table, CAP_UART,
               reinterpret_cast<void*>(platform::kUartBase),
               RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP);

    // 6) VM root authority. Init policy grants reduced VM authority to Dango.
    cap::alloc(*init_proc.cap_table, CAP_VM_ROOT, vm::root_object(),
               RIGHT_VM_CREATE | RIGHT_VM_INSPECT | RIGHT_DUP);

    // 7) Global log doorbell followed by one kernel log ring per supported CPU.
    Doorbell* log_db = ipc::doorbell_create_object(init_proc.pid);
    if (!log_db ||
        ipc::doorbell_cap_alloc(init_proc.pid, log_db,
                                RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP) == kNullCap) {
        ANKO_LOG_WARN("kernel/init", "log doorbell setup failed");
        return;
    }

    Ring* kernel_log_rings[kMaxCpus]{};
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu) {
        Ring* ring = ipc::ring_create_object(init_proc.pid, kKernelLogRingOrder, 0);
        kernel_log_rings[cpu] = ring;
        if (!ring ||
            ipc::ring_cap_alloc(init_proc.pid, ring,
                                RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP) == kNullCap) {
            ANKO_LOGF_WARN("kernel/init", "kernel log ring%u setup failed", cpu);
            kernel_log_rings[cpu] = nullptr;
        }
    }
    anko::log::attach_console(kernel_log_rings, log_db, kMaxCpus);
}

Process* spawn_image(const void* elf_data, usize elf_size, PrincipalClass principal,
                     u32 supervisor_pid, u32 flags, Thread** out_main_thread) {
    if (!elf_data || elf_size == 0) return nullptr;

    Process* p = sched::process_create(principal, supervisor_pid);
    if (!p) return nullptr;

    VmSpace space{};
    space.pgd_phys = p->ttbr0;
    space.pid = p->pid;

    const VirtAddr entry = load_elf(space, elf_data, elf_size);
    if (!entry) return nullptr;

    const PhysAddr stack = mm::alloc_page();
    if (!stack) return nullptr;
    libk::memset(reinterpret_cast<void*>(stack), 0, PAGE_SIZE);
    const VirtAddr stack_base = USER_STACK_TOP - PAGE_SIZE;
    if (!mm::vm_map(space, stack_base, stack, RIGHT_READ | RIGHT_WRITE)) return nullptr;
    const VirtAddr sp_top = USER_STACK_TOP - 16;
    p->next_stack_slot = 1;

    const u32 thread_flags = (flags & SPAWN_SUSPENDED) ? THREAD_SUSPENDED : 0;
    Thread* t = sched::thread_create(p->pid, entry, sp_top, PRIO_SERVICE, 0, thread_flags);
    if (!t) return nullptr;
    if (out_main_thread) *out_main_thread = t;
    if (!(flags & SPAWN_SUSPENDED)) sched::enqueue(t);
    return p;
}

Process* spawn_service(const char* service_name, PrincipalClass principal, u32 supervisor_pid,
                       u32 flags, Thread** out_main_thread) {
    if (!service_name) return nullptr;

    const void* blob = nullptr;
    usize blob_size = 0;
    if (!archive_get(service_name, &blob, &blob_size)) return nullptr;
    Process* p = spawn_image(blob, blob_size, principal, supervisor_pid, flags, out_main_thread);
    if (p) sched::process_set_name(p, service_name);
    return p;
}

void create_init_process() {
    PhysAddr initrd_base = 0;
    usize initrd_size = 0;

    bool archive_ready = false;
    bool embedded_archive = false;
    if (discover_initrd(g_dtb_phys, &initrd_base, &initrd_size)) {
        archive_ready = init_archive(initrd_base, initrd_size);
        if (!archive_ready) {
            ANKO_LOG_WARN("kernel/init", "initrd archive parse failed, trying embedded archive");
            initrd_base = 0;
            initrd_size = 0;
        }
    }
    if (!archive_ready) {
        archive_ready = init_archive(0, 0);
        embedded_archive = archive_ready;
    }
    if (!archive_ready) {
        ANKO_LOG_ERROR("kernel/init", "no init archive");
        return;
    }
    if (embedded_archive && !archive_materialize(&initrd_base, &initrd_size)) {
        ANKO_LOG_ERROR("kernel/init", "embedded initrd materialize failed");
        return;
    }

    const void* manifest_blob = nullptr;
    usize manifest_size = 0;
    if (!archive_get("manifest.v0", &manifest_blob, &manifest_size)) {
        manifest_blob = nullptr;
        manifest_size = 0;
    }

    Process* init_proc = spawn_service("init", PRINCIPAL_NATIVE_TRUSTED, /*supervisor=*/0);
    if (!init_proc) {
        ANKO_LOG_ERROR("kernel/init", "spawn failed");
        return;
    }
    issue_boot_caps(*init_proc,
                    static_cast<PhysAddr>(reinterpret_cast<u64>(manifest_blob)),
                    initrd_base,
                    initrd_size);
    (void)manifest_size;
    ANKO_LOG_INFO("kernel/init", "init queued");
}

} // namespace kernel::init
