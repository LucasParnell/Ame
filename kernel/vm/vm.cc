#include "anko/vm.h"
#include "anko/log.h"
#include "libk/string.h"
#include "libk/printf.h"

namespace drivers::timer { u64 tick_count(); }

extern "C" u64 g_boot_el;

namespace vm {
static u32 g_call_lock;
CallGuard::CallGuard(bool active) : active_(active), irq_(0) {
    if (!active_) return;
    asm volatile("mrs %0, DAIF; msr DAIFSet, #2" : "=r"(irq_) :: "memory");
    while (__atomic_exchange_n(&g_call_lock, 1u, __ATOMIC_ACQUIRE)) asm volatile("yield" ::: "memory");
}
CallGuard::~CallGuard() {
    if (!active_) return;
    __atomic_store_n(&g_call_lock, 0u, __ATOMIC_RELEASE);
    asm volatile("msr DAIF, %0" :: "r"(irq_) : "memory");
}
static void dispose_vm(void* target) { mm::slab_free(target, sizeof(Vm), alignof(Vm)); }


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
// Phase D device assignment counters.
u64 g_vm_device_created = 0;
u64 g_vm_device_active = 0;
u64 g_vm_device_destroyed = 0;
u64 g_vm_device_bar_mapped = 0;
u64 g_vm_device_irq_routes = 0;
u64 g_vm_device_irq_injected = 0;
u64 g_vm_device_mediated_deny = 0;
u64 g_vm_device_bme_enable = 0;
u64 g_vm_device_quarantine = 0;

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
    device_irq_init();
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
    case VM_DEBUG_VM_DEVICE_CREATED: return counter_load(g_vm_device_created);
    case VM_DEBUG_VM_DEVICE_ACTIVE: return counter_load(g_vm_device_active);
    case VM_DEBUG_VM_DEVICE_DESTROYED: return counter_load(g_vm_device_destroyed);
    case VM_DEBUG_VM_DEVICE_BAR_MAPPED: return counter_load(g_vm_device_bar_mapped);
    case VM_DEBUG_VM_DEVICE_IRQ_ROUTES: return counter_load(g_vm_device_irq_routes);
    case VM_DEBUG_VM_DEVICE_IRQ_INJECTED: return counter_load(g_vm_device_irq_injected);
    case VM_DEBUG_VM_DEVICE_MEDIATED_DENY: return counter_load(g_vm_device_mediated_deny);
    case VM_DEBUG_VM_DEVICE_BME_ENABLE: return counter_load(g_vm_device_bme_enable);
    case VM_DEBUG_VM_DEVICE_QUARANTINE: return counter_load(g_vm_device_quarantine);
    default: return 0;
    }
}

void note_error() {
    counter_inc(g_vm_errors);
}

void poll_blocked_vcpu_timers() {
    if (__atomic_exchange_n(&g_call_lock, 1u, __ATOMIC_ACQUIRE)) return;
    for (u32 i = 0; i < kMaxVms; ++i) {
        Vm* v = g_vms[i];
        if (!is_vm_live(v)) continue;
        for (u32 j = 0; j < v->vcpu_count; ++j) {
            if (v->vcpus[j]) poll_vcpu_timer(*v->vcpus[j]);
        }
    }
    __atomic_store_n(&g_call_lock, 0u, __ATOMIC_RELEASE);
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
    case VM_DEBUG_VM_DEVICE_CREATED: counter_inc(g_vm_device_created); break;
    case VM_DEBUG_VM_DEVICE_ACTIVE: counter_inc(g_vm_device_active); break;
    case VM_DEBUG_VM_DEVICE_DESTROYED: counter_inc(g_vm_device_destroyed); break;
    case VM_DEBUG_VM_DEVICE_BAR_MAPPED: counter_inc(g_vm_device_bar_mapped); break;
    case VM_DEBUG_VM_DEVICE_IRQ_ROUTES: counter_inc(g_vm_device_irq_routes); break;
    case VM_DEBUG_VM_DEVICE_IRQ_INJECTED: counter_inc(g_vm_device_irq_injected); break;
    case VM_DEBUG_VM_DEVICE_MEDIATED_DENY: counter_inc(g_vm_device_mediated_deny); break;
    case VM_DEBUG_VM_DEVICE_BME_ENABLE: counter_inc(g_vm_device_bme_enable); break;
    case VM_DEBUG_VM_DEVICE_QUARANTINE: counter_inc(g_vm_device_quarantine); break;
    default: break;
    }
}

cap::Ref require_cap(Process& caller, CapHandle handle, CapType type,
                      u32 need_rights, SysError* out_error) {
    if (!caller.cap_table) {
        if (out_error) *out_error = E_NOTFOUND;
        note_error();
        return nullptr;
    }

    cap::Ref e = cap::lookup_any(*caller.cap_table, handle);
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

cap::Borrow<Vm> vm_from_cap(Process& caller, CapHandle vm_cap, u32 need_rights, SysError* out_error) {
    cap::Ref e = require_cap(caller, vm_cap, CAP_VM, need_rights, out_error);
    auto* v = e ? static_cast<Vm*>(e->target) : nullptr;
    if (!is_vm_live(v)) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    return cap::Borrow<Vm>(e);
}

cap::Borrow<Vcpu> vcpu_from_cap(Process& caller, CapHandle vcpu_cap, u32 need_rights, SysError* out_error) {
    cap::Ref e = require_cap(caller, vcpu_cap, CAP_VCPU, need_rights, out_error);
    auto* vcpu = e ? static_cast<Vcpu*>(e->target) : nullptr;
    if (!vcpu || vcpu->state == VCPU_DEAD) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    return cap::Borrow<Vcpu>(e);
}

cap::Borrow<VmMemory> memory_from_cap(Process& caller, CapHandle memory_cap,
                          u32 need_rights, SysError* out_error) {
    cap::Ref e = require_cap(caller, memory_cap, CAP_VM_MEMORY, need_rights, out_error);
    auto* mem = e ? static_cast<VmMemory*>(e->target) : nullptr;
    if (!mem || mem->dead) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    return cap::Borrow<VmMemory>(e);
}

cap::Borrow<VmChannel> channel_from_cap(Process& caller, CapHandle channel_cap,
                            u32 need_rights, SysError* out_error) {
    cap::Ref e = require_cap(caller, channel_cap, CAP_VM_CHANNEL, need_rights, out_error);
    auto* channel = e ? static_cast<VmChannel*>(e->target) : nullptr;
    if (!channel || channel->dead) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    return cap::Borrow<VmChannel>(e);
}

Vm* find_vm_by_id(u32 vm_id) {
    if (vm_id == 0) return nullptr;
    for (u32 i = 0; i < kMaxVms; ++i) {
        if (g_vms[i] && g_vms[i]->vm_id == vm_id) return g_vms[i];
    }
    return nullptr;
}

Vm* owner_for_vcpu(Vcpu* vcpu) {
    if (!vcpu || vcpu->state == VCPU_DEAD) return nullptr;

    if (vcpu->owner_vm_id != 0) {
        Vm* by_id = find_vm_by_id(vcpu->owner_vm_id);
        if (by_id && is_vm_live(by_id)) {
            for (u32 i = 0; i < by_id->vcpu_count; ++i) {
                if (by_id->vcpus[i] == vcpu) return by_id;
            }
        }
    }

    // The VM owns the authoritative vCPU membership list.  Use it as a
    // recovery path if the cached owner_vm_id was left zero or was otherwise
    // stale.  This keeps vcpu_run from depending on duplicated ownership
    // metadata in the hot path.
    for (u32 i = 0; i < kMaxVms; ++i) {
        Vm* candidate = g_vms[i];
        if (!is_vm_live(candidate)) continue;
        for (u32 j = 0; j < candidate->vcpu_count; ++j) {
            if (candidate->vcpus[j] == vcpu) {
                vcpu->owner_vm_id = candidate->vm_id;
                return candidate;
            }
        }
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
    cap::Ref root = require_cap(caller, root_cap, CAP_VM_ROOT, RIGHT_VM_CREATE, &cap_error);
    if (!root) {
        if (out_error) *out_error = cap_error;
        return kNullCap;
    }
    if (root->target != &g_root) {
        ANKO_LOG_ERROR("kernel/vm", "sys_vm_create root target mismatch");
        if (out_error) *out_error = E_NOTFOUND;
        note_error();
        return kNullCap;
    }

    const u32 slot = find_free_vm_slot();
    if (slot >= kMaxVms) {
        ANKO_LOG_ERROR("kernel/vm", "sys_vm_create no free vm slot");
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    Vm* v = static_cast<Vm*>(mm::slab_alloc(sizeof(Vm), alignof(Vm)));
    if (!v) {
        ANKO_LOG_ERROR("kernel/vm", "sys_vm_create heap alloc failed");
        if (out_error) *out_error = E_NOMEM;
        note_error();
        return kNullCap;
    }

    libk::memset(v, 0, sizeof(*v));
    v->vm_id = g_next_vm_id++;
    v->owner_pid = caller.pid;
    v->owner_identity = sched::process_identity(caller);
    v->state = VM_CREATED;
    v->flags = flags;
    v->created_ticks = drivers::timer::tick_count();

    const SysError s2 = stage2_create(&v->stage2);
    if (s2 != E_OK) {
        ANKO_LOGF_ERROR("kernel/vm", "sys_vm_create stage2 create fail rc=%d",
                        static_cast<int>(s2));
        v->state = VM_DEAD;
        mm::slab_free(v, sizeof(Vm), alignof(Vm));
        if (out_error) *out_error = s2;
        note_error();
        return kNullCap;
    }

    v->lifetime = object::create(v, dispose_vm);
    if (!v->lifetime) {
        stage2_destroy(&v->stage2); dispose_vm(v);
        if (out_error) *out_error = E_NOMEM;
        return kNullCap;
    }
    const CapHandle cap_h = cap::alloc(*caller.cap_table, CAP_VM, v,
                                       RIGHT_VM_CONTROL | RIGHT_VM_MAP |
                                           RIGHT_VM_INSPECT | RIGHT_VM_DESTROY |
                                           RIGHT_DUP);
    if (cap_h == kNullCap) {
        ANKO_LOG_ERROR("kernel/vm", "sys_vm_create cap alloc failed");
        v->state = VM_DEAD;
        stage2_destroy(&v->stage2);
        object::release(v->lifetime);
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    g_vms[slot] = v;
    metric_inc(VM_DEBUG_CREATED);
    if (out_error) *out_error = E_OK;
    return cap_h;
}

static void retire_vm(Vm* v) {
    if (!is_vm_live(v)) return;
    v->state = VM_DEAD;
    // Device assignments bound to this VM must be revoked (DMA-safe, §8.12)
    // before the stage-2 tables and RAM pages backing their BAR mappings and
    // DMA ranges are freed.
    device_release_for_vm(v);
    // CallGuard excludes guest entry. Invalidate translations before pages.
    stage2_destroy(&v->stage2);
    for (u32 i = 0; i < v->memory_count; ++i) {
        auto* mem = v->memory_objects[i]; if (!mem) continue;
        sched::sweep_process_caps(mem); object::release(mem->lifetime);
        v->memory_objects[i] = nullptr;
    }
    for (u32 i = 0; i < v->vcpu_count; ++i) {
        auto* cpu = v->vcpus[i]; if (!cpu) continue;
        cpu->state = VCPU_DEAD;
        sched::sweep_process_caps(cpu); object::release(cpu->lifetime);
        v->vcpus[i] = nullptr;
    }
    for (u32 i = 0; i < v->channel_count; ++i) {
        auto* channel = v->channels[i]; if (!channel) continue;
        channel->dead = true;
        sched::sweep_process_caps(channel);
        // Ring and doorbell endpoints are capabilities whose target is the
        // endpoint object, not the channel. They are handed out by
        // channel_endpoint() (and may be re-shared). Revoke every process's
        // endpoints so the channel's creator reference is the last one left
        // and dispose_channel() can release and free them exactly once.
        {
            void* const endpoints[] = {channel->host_to_guest, channel->guest_to_host,
                                       channel->host_doorbell, channel->guest_doorbell};
            for (void* endpoint : endpoints) sched::sweep_process_caps(endpoint);
        }
        object::release(channel->lifetime);
        v->channels[i] = nullptr;
    }
    for (auto& slot : g_vms) if (slot == v) slot = nullptr;
    sched::sweep_process_caps(v); metric_inc(VM_DEBUG_DESTROYED);
    object::release(v->lifetime);
}

void destroy_owned_process(u64 identity) {
    CallGuard guard(true);
    device_owner_died(identity);
    for (auto* v : g_vms) if (v && v->owner_identity == identity) retire_vm(v);
}

SysError destroy_vm(Process& caller, CapHandle vm_cap) {
    SysError rc = E_OK;
    auto v = vm_from_cap(caller, vm_cap, RIGHT_VM_DESTROY, &rc);
    if (!v) return rc;
    retire_vm(v);
    return E_OK;
}

SysError inspect_vm(Process& caller, CapHandle vm_cap, VmInfo* out) {
    if (!out) return E_INVALID;
    SysError rc = E_OK;
    auto v = vm_from_cap(caller, vm_cap, RIGHT_VM_INSPECT, &rc);
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
