#include "anko/vm.h"
#include "anko/log.h"
#include "libk/string.h"
#include "libk/printf.h"

namespace drivers::timer { u64 tick_count(); }

extern "C" u64 g_boot_el;

namespace vm {

namespace {

VmRoot g_root{0x564D524Fu}; // 'VMRO'
Vm* g_vms[kMaxVms]{};
u32 g_next_vm_id = 1;

u64 g_vm_create_calls = 0;
u64 g_vm_created = 0;
u64 g_vm_destroyed = 0;
u64 g_vm_memory_created = 0;
u64 g_vm_memory_mapped = 0;
u64 g_vm_memory_unmapped = 0;
u64 g_vm_stage2_tables_created = 0;
u64 g_vm_stage2_pages_mapped = 0;
u64 g_vcpu_created = 0;
u64 g_vcpu_configured = 0;
u64 g_vcpu_run_attempts = 0;
u64 g_vcpu_run_notimpl = 0;
u64 g_vm_channels_created = 0;
u64 g_vm_errors = 0;
u64 g_el_current = 0;
u64 g_el2_available = 0;
u64 g_vhe_available = 0;
u64 g_guest_boot_stub = 0;
u64 g_guest_agent_start = 0;
u64 g_guest_linux_hello = 0;
u64 g_vcpu_wfx_exits = 0;
u64 g_vcpu_irq_exits = 0;
u64 g_vcpu_unsupported_exits = 0;

inline void counter_inc(u64& counter) {
    __atomic_add_fetch(&counter, 1ull, __ATOMIC_RELAXED);
}

inline u64 counter_load(const u64& counter) {
    return __atomic_load_n(&counter, __ATOMIC_RELAXED);
}

inline bool has_rights(const CapEntry* e, u32 rights) {
    return e && ((e->rights_mask & rights) == rights);
}

bool is_vm_live(const Vm* v) {
    return v && v->state != VM_EMPTY && v->state != VM_DEAD;
}

u32 find_free_vm_slot() {
    for (u32 i = 0; i < kMaxVms; ++i) {
        if (!is_vm_live(g_vms[i])) return i;
    }
    return kMaxVms;
}

} // namespace

void init() {
    u64 current_el = 0;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
    g_el_current = (current_el >> 2) & 0x3ull;
    g_el2_available = (g_boot_el >= 2) ? 1 : 0;
    g_vhe_available = 0;
}

VmRoot* root_object() {
    return &g_root;
}

u64 debug_counter(u64 id) {
    switch (id) {
    case VM_DEBUG_CREATE_CALLS: return counter_load(g_vm_create_calls);
    case VM_DEBUG_CREATED: return counter_load(g_vm_created);
    case VM_DEBUG_DESTROYED: return counter_load(g_vm_destroyed);
    case VM_DEBUG_MEMORY_CREATED: return counter_load(g_vm_memory_created);
    case VM_DEBUG_MEMORY_MAPPED: return counter_load(g_vm_memory_mapped);
    case VM_DEBUG_MEMORY_UNMAPPED: return counter_load(g_vm_memory_unmapped);
    case VM_DEBUG_STAGE2_TABLES_CREATED: return counter_load(g_vm_stage2_tables_created);
    case VM_DEBUG_STAGE2_PAGES_MAPPED: return counter_load(g_vm_stage2_pages_mapped);
    case VM_DEBUG_VCPU_CREATED: return counter_load(g_vcpu_created);
    case VM_DEBUG_VCPU_CONFIGURED: return counter_load(g_vcpu_configured);
    case VM_DEBUG_VCPU_RUN_ATTEMPTS: return counter_load(g_vcpu_run_attempts);
    case VM_DEBUG_VCPU_RUN_NOTIMPL: return counter_load(g_vcpu_run_notimpl);
    case VM_DEBUG_CHANNELS_CREATED: return counter_load(g_vm_channels_created);
    case VM_DEBUG_ERRORS: return counter_load(g_vm_errors);
    case VM_DEBUG_EL_CURRENT: return counter_load(g_el_current);
    case VM_DEBUG_EL2_AVAILABLE: return counter_load(g_el2_available);
    case VM_DEBUG_VHE_AVAILABLE: return counter_load(g_vhe_available);
    case VM_DEBUG_GUEST_BOOT_STUB: return counter_load(g_guest_boot_stub);
    case VM_DEBUG_GUEST_AGENT_START: return counter_load(g_guest_agent_start);
    case VM_DEBUG_GUEST_LINUX_HELLO: return counter_load(g_guest_linux_hello);
    case VM_DEBUG_VCPU_WFX_EXITS: return counter_load(g_vcpu_wfx_exits);
    case VM_DEBUG_VCPU_IRQ_EXITS: return counter_load(g_vcpu_irq_exits);
    case VM_DEBUG_VCPU_UNSUPPORTED_EXITS: return counter_load(g_vcpu_unsupported_exits);
    default: return 0;
    }
}

void note_error() {
    counter_inc(g_vm_errors);
}

void metric_inc(VmDebugCounter id) {
    switch (id) {
    case VM_DEBUG_CREATE_CALLS: counter_inc(g_vm_create_calls); break;
    case VM_DEBUG_CREATED: counter_inc(g_vm_created); break;
    case VM_DEBUG_DESTROYED: counter_inc(g_vm_destroyed); break;
    case VM_DEBUG_MEMORY_CREATED: counter_inc(g_vm_memory_created); break;
    case VM_DEBUG_MEMORY_MAPPED: counter_inc(g_vm_memory_mapped); break;
    case VM_DEBUG_MEMORY_UNMAPPED: counter_inc(g_vm_memory_unmapped); break;
    case VM_DEBUG_STAGE2_TABLES_CREATED: counter_inc(g_vm_stage2_tables_created); break;
    case VM_DEBUG_STAGE2_PAGES_MAPPED: counter_inc(g_vm_stage2_pages_mapped); break;
    case VM_DEBUG_VCPU_CREATED: counter_inc(g_vcpu_created); break;
    case VM_DEBUG_VCPU_CONFIGURED: counter_inc(g_vcpu_configured); break;
    case VM_DEBUG_VCPU_RUN_ATTEMPTS: counter_inc(g_vcpu_run_attempts); break;
    case VM_DEBUG_VCPU_RUN_NOTIMPL: counter_inc(g_vcpu_run_notimpl); break;
    case VM_DEBUG_CHANNELS_CREATED: counter_inc(g_vm_channels_created); break;
    case VM_DEBUG_ERRORS: counter_inc(g_vm_errors); break;
    case VM_DEBUG_GUEST_BOOT_STUB: counter_inc(g_guest_boot_stub); break;
    case VM_DEBUG_GUEST_AGENT_START: counter_inc(g_guest_agent_start); break;
    case VM_DEBUG_GUEST_LINUX_HELLO: counter_inc(g_guest_linux_hello); break;
    case VM_DEBUG_VCPU_WFX_EXITS: counter_inc(g_vcpu_wfx_exits); break;
    case VM_DEBUG_VCPU_IRQ_EXITS: counter_inc(g_vcpu_irq_exits); break;
    case VM_DEBUG_VCPU_UNSUPPORTED_EXITS: counter_inc(g_vcpu_unsupported_exits); break;
    default: break;
    }
}

CapEntry* require_cap(Process& caller, CapHandle handle, CapType type,
                      u32 need_rights, SysError* out_error) {
    if (!caller.cap_table) {
        if (out_error) *out_error = E_NOTFOUND;
        note_error();
        return nullptr;
    }

    CapEntry* e = cap::lookup_any(*caller.cap_table, handle);
    if (!e) {
        if (out_error) *out_error = E_NOTFOUND;
        note_error();
        return nullptr;
    }
    if (e->type_tag != type) {
        if (out_error) *out_error = E_BADTYPE;
        note_error();
        return nullptr;
    }
    if (!has_rights(e, need_rights)) {
        if (out_error) *out_error = E_PERM;
        note_error();
        return nullptr;
    }
    if (out_error) *out_error = E_OK;
    return e;
}

Vm* vm_from_cap(Process& caller, CapHandle vm_cap, u32 need_rights, SysError* out_error) {
    CapEntry* e = require_cap(caller, vm_cap, CAP_VM, need_rights, out_error);
    auto* v = e ? static_cast<Vm*>(e->target) : nullptr;
    if (!is_vm_live(v)) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    return v;
}

Vcpu* vcpu_from_cap(Process& caller, CapHandle vcpu_cap, u32 need_rights, SysError* out_error) {
    CapEntry* e = require_cap(caller, vcpu_cap, CAP_VCPU, need_rights, out_error);
    auto* vcpu = e ? static_cast<Vcpu*>(e->target) : nullptr;
    if (!vcpu || vcpu->state == VCPU_DEAD) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    return vcpu;
}

VmMemory* memory_from_cap(Process& caller, CapHandle memory_cap,
                          u32 need_rights, SysError* out_error) {
    CapEntry* e = require_cap(caller, memory_cap, CAP_VM_MEMORY, need_rights, out_error);
    auto* mem = e ? static_cast<VmMemory*>(e->target) : nullptr;
    if (!mem || mem->dead) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    return mem;
}

VmChannel* channel_from_cap(Process& caller, CapHandle channel_cap,
                            u32 need_rights, SysError* out_error) {
    CapEntry* e = require_cap(caller, channel_cap, CAP_VM_CHANNEL, need_rights, out_error);
    auto* channel = e ? static_cast<VmChannel*>(e->target) : nullptr;
    if (!channel || channel->dead) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    return channel;
}

Vm* find_vm_by_id(u32 vm_id) {
    if (vm_id == 0) return nullptr;
    for (u32 i = 0; i < kMaxVms; ++i) {
        if (g_vms[i] && g_vms[i]->vm_id == vm_id) return g_vms[i];
    }
    return nullptr;
}

void refresh_vm_state(Vm* v) {
    if (!v || v->state == VM_DEAD) return;

    bool has_mapped_memory = false;
    for (u32 i = 0; i < v->memory_count; ++i) {
        VmMemory* mem = v->memory_objects[i];
        if (mem && !mem->dead && mem->mapped_stage2) {
            has_mapped_memory = true;
            break;
        }
    }

    bool has_vcpu = false;
    bool has_ready_vcpu = false;
    for (u32 i = 0; i < v->vcpu_count; ++i) {
        Vcpu* vcpu = v->vcpus[i];
        if (!vcpu || vcpu->state == VCPU_DEAD) continue;
        has_vcpu = true;
        if (vcpu->state == VCPU_READY || vcpu->state == VCPU_CONFIGURED) {
            has_ready_vcpu = true;
        }
    }

    if (!has_mapped_memory) v->state = VM_CREATED;
    else if (!has_vcpu) v->state = VM_MEMORY_READY;
    else if (!has_ready_vcpu) v->state = VM_VCPU_READY;
    else v->state = VM_READY;
}

CapHandle create_vm(Process& caller, CapHandle root_cap, u64 flags, SysError* out_error) {
    metric_inc(VM_DEBUG_CREATE_CALLS);

    SysError cap_error = E_OK;
    CapEntry* root = require_cap(caller, root_cap, CAP_VM_ROOT, RIGHT_VM_CREATE, &cap_error);
    if (!root) {
        if (out_error) *out_error = cap_error;
        return kNullCap;
    }
    if (root->target != &g_root) {
        ANKO_LOG_ERROR("kernel/vm", "SYS_VM_CREATE root target mismatch");
        if (out_error) *out_error = E_NOTFOUND;
        note_error();
        return kNullCap;
    }

    const u32 slot = find_free_vm_slot();
    if (slot >= kMaxVms) {
        ANKO_LOG_ERROR("kernel/vm", "SYS_VM_CREATE no free vm slot");
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    Vm* v = static_cast<Vm*>(mm::heap_alloc(sizeof(Vm), alignof(Vm)));
    if (!v) {
        ANKO_LOG_ERROR("kernel/vm", "SYS_VM_CREATE heap alloc failed");
        if (out_error) *out_error = E_NOMEM;
        note_error();
        return kNullCap;
    }

    libk::memset(v, 0, sizeof(*v));
    v->vm_id = g_next_vm_id++;
    v->owner_pid = caller.pid;
    v->state = VM_CREATED;
    v->flags = flags;
    v->created_ticks = drivers::timer::tick_count();

    const SysError s2 = stage2_create(&v->stage2);
    if (s2 != E_OK) {
        ANKO_LOGF_ERROR("kernel/vm", "SYS_VM_CREATE stage2 create fail rc=%d",
                        static_cast<int>(s2));
        v->state = VM_DEAD;
        if (out_error) *out_error = s2;
        note_error();
        return kNullCap;
    }

    const CapHandle cap_h = cap::alloc(*caller.cap_table, CAP_VM, v,
                                       RIGHT_VM_CONTROL | RIGHT_VM_MAP |
                                           RIGHT_VM_INSPECT | RIGHT_VM_DESTROY |
                                           RIGHT_DUP);
    if (cap_h == kNullCap) {
        ANKO_LOG_ERROR("kernel/vm", "SYS_VM_CREATE cap alloc failed");
        v->state = VM_DEAD;
        stage2_destroy(&v->stage2);
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    g_vms[slot] = v;
    metric_inc(VM_DEBUG_CREATED);
    if (out_error) *out_error = E_OK;
    return cap_h;
}

SysError destroy_vm(Process& caller, CapHandle vm_cap) {
    SysError rc = E_OK;
    Vm* v = vm_from_cap(caller, vm_cap, RIGHT_VM_DESTROY, &rc);
    if (!v) return rc;

    for (u32 i = 0; i < v->memory_count; ++i) destroy_memory_object(v->memory_objects[i]);
    for (u32 i = 0; i < v->vcpu_count; ++i) {
        if (v->vcpus[i]) v->vcpus[i]->state = VCPU_DEAD;
    }
    for (u32 i = 0; i < v->channel_count; ++i) {
        if (v->channels[i]) v->channels[i]->dead = true;
    }
    stage2_destroy(&v->stage2);
    v->state = VM_DEAD;
    metric_inc(VM_DEBUG_DESTROYED);
    return E_OK;
}

SysError inspect_vm(Process& caller, CapHandle vm_cap, VmInfo* out) {
    if (!out) return E_INVALID;
    SysError rc = E_OK;
    Vm* v = vm_from_cap(caller, vm_cap, RIGHT_VM_INSPECT, &rc);
    if (!v) return rc;

    VmInfo info{};
    info.vm_id = v->vm_id;
    info.owner_pid = v->owner_pid;
    info.state = static_cast<u32>(v->state);
    info.memory_count = v->memory_count;
    info.vcpu_count = v->vcpu_count;
    info.channel_count = v->channel_count;
    info.guest_ram_bytes = v->guest_ram_bytes;
    info.mapped_regions = v->mapped_regions;
    info.flags = v->flags;
    info.stage2_mapped_pages = v->stage2.mapped_pages;
    info.stage2_built = v->stage2.built ? 1 : 0;
    info.stage2_active = v->stage2.active ? 1 : 0;
    *out = info;
    return E_OK;
}

} // namespace vm
