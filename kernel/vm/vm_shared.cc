// kernel/vm/vm_shared.cc — dynamic shared arenas (Phase L, spec §12.3).
//
// A native owner attaches an existing pinned page set (a kernel-backed
// CAP_ARENA) into a VM so the appliance can share an arbitrary buffer without
// rebooting. The attachment is one capability-bearing object that couples two
// lifetimes:
//
//   * host arena lifetime — the record retains the arena's object record, so
//     the backing pages stay pinned for at least as long as the guest mapping;
//   * guest mapping lifetime — the record owns the stage-2 mapping (and retains
//     the VM record), so the mapping can never outlive the VM.
//
// Releasing the attachment's last reference tears both down together: the
// stage-2 range is unmapped and both retained records are released. There is no
// partial state in which one side survives the other.
//
// DMA is explicit and deliberately NOT implied by attachment. A region enters
// stage 2 as iommu::VmDmaClass::None and is absent from every DMA domain; it
// becomes reachable by an assigned device only after the owner calls
// shared_arena_register_dma(), which is the sole writer of `dma_class` and is
// guarded against widening an existing permission set. This mirrors how
// collect_vm_ram() authorises appliance RAM, but as a separate, opt-in step.

#include "anko/vm.h"

#include "anko/log.h"
#include "anko/panic.h"
#include "libk/string.h"

namespace vm {

namespace {

// DMA owner identity a registered region reports to the IOMMU core. Distinct
// from the device-assignment tag so the two authorisation sources can never be
// confused for one another.
constexpr u64 kSharedArenaObjectTag = 0x414D'4553'4841'5245ull; // "AMESHARE"

struct SharedArenaPool {
    SharedArena slots[kMaxSharedArenas];
    u32 next_id;
    u32 reserved;
    kernel::SpinLock lock;
};

SharedArenaPool g_pool{};

u64 g_shared_attached = 0;
u64 g_shared_teardowns = 0;
u64 g_shared_dma_registered = 0;

// A slot is claimed exactly when its id is non-zero; a fully-disposed slot is
// zeroed, so id == 0 marks it free. `lifetime` alone is not enough because it is
// installed last in shared_arena_create().
SharedArena* alloc_slot() {
    for (u32 i = 0; i < kMaxSharedArenas; ++i) {
        if (g_pool.slots[i].shared_id == 0 && g_pool.slots[i].lifetime == nullptr) {
            return &g_pool.slots[i];
        }
    }
    return nullptr;
}

void unlink_from_vm(SharedArena* shared) {
    Vm* vm = shared->owner_vm;
    if (!vm) {
        return;
    }
    for (u32 i = 0; i < vm->shared_arena_count; ++i) {
        if (vm->shared_arenas[i] != shared) {
            continue;
        }
        vm->shared_arenas[i] = vm->shared_arenas[--vm->shared_arena_count];
        vm->shared_arenas[vm->shared_arena_count] = nullptr;
        return;
    }
}

// Guest physical range an attachment may occupy: page aligned, non-empty, above
// the appliance's reserved low layout, and inside the stage-2 IPA width.
// Overlap with RAM, existing mappings and trap ranges is enforced separately by
// stage2_validate_no_overlap(), so this only bounds the shape.
bool shared_range_valid(const Vm& vm, u64 guest_pa, u64 size) {
    if (size == 0 || (guest_pa & (PAGE_SIZE - 1)) != 0 || (size & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    if (guest_pa < kMinDeviceGuestIpa) {
        return false;
    }
    const u64 ipa_limit = 1ull << vm.stage2.ipa_bits;
    if (guest_pa >= ipa_limit) {
        return false;
    }
    u64 end = 0;
    if (__builtin_add_overflow(guest_pa, size, &end)) {
        return false;
    }
    return end <= ipa_limit;
}

// Shared buffers are device-visible normal memory: writable and/or readable,
// never executable in the guest.
bool shared_rights_valid(u32 rights) {
    return rights != 0 && (rights & ~(RIGHT_READ | RIGHT_WRITE)) == 0;
}

bool dma_perms_valid(u32 perms) {
    const u32 mask = static_cast<u32>(iommu::DmaPerm::Read) |
                     static_cast<u32>(iommu::DmaPerm::Write);
    return perms != 0 && (perms & ~mask) == 0;
}

} // namespace

SysError shared_arena_create(Vm* vm, Arena* arena, object::Record* arena_record, u64 guest_pa,
                             u64 size, u32 rights, u32 flags, SharedArena** out) {
    if (out) *out = nullptr;
    if (!vm || !arena || !arena_record) {
        return E_INVALID;
    }
    // An attachment needs a pinned physical backing: a VA-only registered arena
    // (phys_base == 0) has no host pages to map.
    if (arena->phys_base == 0 || arena->size == 0) {
        return E_INVALID;
    }
    // Attach the whole page set. A sub-range attachment would let the guest
    // mapping and the retained host pages disagree about extent.
    if (size != arena->size) {
        return E_INVALID;
    }
    if (flags != kSharedArenaFlagNone) {
        return E_INVALID;
    }
    if (!shared_range_valid(*vm, guest_pa, size)) {
        return E_INVALID;
    }
    if (!shared_rights_valid(rights)) {
        return E_INVALID;
    }
    if (!vm->stage2.built) {
        return E_INVALID;
    }
    if (vm->shared_arena_count >= kMaxSharedArenas) {
        return E_OVERFLOW;
    }
    if (!stage2_has_capacity(&vm->stage2, size / PAGE_SIZE)) {
        return E_OVERFLOW;
    }
    // The single stage-2 authority for "is this IPA range free": rejects overlap
    // with RAM, device mappings and trap ranges, and refuses an active stage 2.
    const SysError overlap = stage2_validate_no_overlap(&vm->stage2, guest_pa, size);
    if (overlap != E_OK) {
        return overlap;
    }

    SharedArena* shared = nullptr;
    {
        kernel::LockGuard guard(g_pool.lock);
        shared = alloc_slot();
        if (!shared) {
            return E_OVERFLOW;
        }
        libk::memset(shared, 0, sizeof(*shared));
        shared->shared_id = g_pool.next_id++;
        if (shared->shared_id == 0) {
            shared->shared_id = g_pool.next_id++;
        }
    }
    shared->owner_vm = vm;
    shared->owner_vm_id = vm->vm_id;
    shared->arena = arena;
    shared->object_tag = kSharedArenaObjectTag ^ static_cast<u64>(shared->shared_id);
    shared->host_phys_base = arena->phys_base;
    shared->size = size;
    shared->guest_pa_base = guest_pa;
    shared->rights = rights;
    shared->flags = flags;
    // Non-DMA unless explicitly registered: the region enters stage 2 with no
    // DMA class and zero device permissions.
    shared->dma_class = iommu::VmDmaClass::None;
    shared->dma_perms = 0;

    const u64 pages = size / PAGE_SIZE;
    for (u64 i = 0; i < pages; ++i) {
        const SysError rc = stage2_map_page(&vm->stage2, guest_pa + i * PAGE_SIZE,
                                            arena->phys_base + i * PAGE_SIZE, rights);
        if (rc != E_OK) {
            for (u64 j = 0; j < i; ++j) {
                (void)stage2_unmap_page(&vm->stage2, guest_pa + j * PAGE_SIZE);
            }
            libk::memset(shared, 0, sizeof(*shared));
            return rc;
        }
    }
    shared->mapped_stage2 = true;

    shared->arena_lifetime = arena_record;
    object::retain(shared->arena_lifetime);
    shared->owner_lifetime = vm->lifetime;
    object::retain(shared->owner_lifetime);
    shared->lifetime = object::create(shared, dispose_shared_arena);
    if (!shared->lifetime) {
        for (u64 j = 0; j < pages; ++j) {
            (void)stage2_unmap_page(&vm->stage2, guest_pa + j * PAGE_SIZE);
        }
        shared->mapped_stage2 = false;
        object::release(shared->arena_lifetime);
        object::release(shared->owner_lifetime);
        libk::memset(shared, 0, sizeof(*shared));
        return E_NOMEM;
    }

    vm->shared_arenas[vm->shared_arena_count++] = shared;
    ++g_shared_attached;
    ANKO_LOGF_INFO("kernel/vm", "shared: attached id=%u vm=%u ipa=0x%llx size=0x%llx dma=none",
                   shared->shared_id, shared->owner_vm_id,
                   static_cast<unsigned long long>(guest_pa),
                   static_cast<unsigned long long>(size));
    if (out) *out = shared;
    return E_OK;
}

SysError shared_arena_teardown(SharedArena* shared) {
    if (!shared) {
        return E_INVALID;
    }
    if (shared->dead) {
        return E_OK;
    }
    Vm* vm = shared->owner_vm;
    // Only unmap while the VM's stage 2 still exists. A retired VM has already
    // destroyed its tables (stage2_destroy), so the mapping is gone regardless;
    // touching the zeroed stage-2 space would just fail harmlessly.
    if (vm && vm->state != VM_DEAD && shared->mapped_stage2) {
        const u64 pages = shared->size / PAGE_SIZE;
        for (u64 i = 0; i < pages; ++i) {
            (void)stage2_unmap_page(&vm->stage2, shared->guest_pa_base + i * PAGE_SIZE);
        }
    }
    shared->mapped_stage2 = false;
    shared->dead = true;
    unlink_from_vm(shared);
    ++g_shared_teardowns;
    return E_OK;
}

SysError shared_arena_register_dma(SharedArena* shared, u32 perms) {
    if (!shared) {
        return E_INVALID;
    }
    // Guard: a region can only be registered while it is live, guest-mapped and
    // still bound to its VM. There is no transition out of SharedGpu.
    if (shared->dead || !shared->mapped_stage2 || !shared->owner_vm) {
        return E_DEAD;
    }
    if (!dma_perms_valid(perms)) {
        return E_INVALID;
    }
    if (shared->dma_class == iommu::VmDmaClass::SharedGpu) {
        // Idempotent, but never an upgrade: re-registration may only keep or
        // narrow the permission set captured at the first transition.
        if ((perms & ~shared->dma_perms) != 0) {
            return E_INVALID;
        }
        return E_OK;
    }
    // The transition itself. Guarded so nothing else can set SharedGpu, and so a
    // class the IOMMU core would not honour can never be recorded.
    ANKO_ASSERT(shared->dma_class == iommu::VmDmaClass::None);
    if (!iommu::dma_class_perms(iommu::VmDmaClass::SharedGpu)) {
        return E_INVALID;
    }
    shared->dma_class = iommu::VmDmaClass::SharedGpu;
    shared->dma_perms = perms;
    ++g_shared_dma_registered;
    ANKO_LOGF_INFO("kernel/vm", "shared: registered SharedGpu id=%u pa=0x%llx perms=%x",
                   shared->shared_id, static_cast<unsigned long long>(shared->host_phys_base),
                   perms);
    return E_OK;
}

iommu::VmDmaClass shared_arena_dma_class(const SharedArena& shared) {
    return shared.dma_class;
}

bool shared_arena_dma_query(void* ctx, PhysAddr pa, iommu::PageOwnership* out) {
    auto* shared = static_cast<SharedArena*>(ctx);
    if (!shared || !out) {
        return false;
    }
    // An unregistered (None) region is owned by nobody: the IOMMU core then
    // refuses every mapping request for it, so it is absent from every domain.
    if (shared->dead || shared->dma_class == iommu::VmDmaClass::None) {
        return false;
    }
    if (pa < shared->host_phys_base || pa - shared->host_phys_base >= shared->size) {
        return false;
    }
    out->object = shared->object_tag;
    out->dma_class = shared->dma_class;
    libk::memset(out->pad, 0, sizeof(out->pad));
    return true;
}

void dispose_shared_arena(void* target) {
    auto* shared = static_cast<SharedArena*>(target);
    if (!shared) {
        return;
    }
    (void)shared_arena_teardown(shared);
    object::Record* arena = shared->arena_lifetime;
    object::Record* owner = shared->owner_lifetime;
    libk::memset(shared, 0, sizeof(*shared));
    // Dropping the VM reference last: releasing it may free the VM, after which
    // `shared` must already be zeroed above (it is, and it is not touched again).
    object::release(arena);
    object::release(owner);
}

void shared_arena_release_for_vm(Vm* vm) {
    if (!vm) {
        return;
    }
    for (u32 i = 0; i < kMaxSharedArenas; ++i) {
        SharedArena* shared = &g_pool.slots[i];
        if (shared->lifetime == nullptr || shared->owner_vm != vm || shared->dead) {
            continue;
        }
        (void)shared_arena_teardown(shared);
    }
}

cap::Borrow<SharedArena> shared_arena_from_cap(Process& caller, CapHandle shared_cap,
                                               u32 need_rights, SysError* out_error) {
    cap::Ref e = require_cap(caller, shared_cap, CAP_VM_SHARED, need_rights, out_error);
    auto* shared = e ? static_cast<SharedArena*>(e->target) : nullptr;
    if (!shared || !shared->lifetime) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    // A retired VM has already destroyed its stage 2. Detect that lazily so a
    // stale handle can neither report a live mapping nor be registered after the
    // mapping is gone.
    if (!shared->dead && shared->owner_vm && shared->owner_vm->state == VM_DEAD) {
        (void)shared_arena_teardown(shared);
    }
    if (shared->dead) {
        if (out_error) *out_error = E_DEAD;
        note_error();
        return nullptr;
    }
    return cap::Borrow<SharedArena>(e);
}

CapHandle attach_shared_memory(Process& caller, CapHandle vm_cap, CapHandle arena_cap,
                               u64 guest_pa, u64 size, u32 rights, u32 flags,
                               SysError* out_error) {
    SysError rc = E_OK;
    auto vm = vm_from_cap(caller, vm_cap, RIGHT_VM_CONTROL, &rc);
    if (!vm) {
        if (out_error) *out_error = rc;
        return kNullCap;
    }

    // The arena must be a pinned page set the caller is allowed to map; a
    // VA-only registration is rejected by shared_arena_create (no phys_base).
    cap::Ref arena_e = require_cap(caller, arena_cap, CAP_ARENA, RIGHT_MAP, &rc);
    if (!arena_e) {
        if (out_error) *out_error = rc;
        return kNullCap;
    }
    auto* arena = static_cast<Arena*>(arena_e->target);
    if (!arena) {
        if (out_error) *out_error = E_NOTFOUND;
        note_error();
        return kNullCap;
    }

    SharedArena* shared = nullptr;
    rc = shared_arena_create(vm.get(), arena, arena_e->object, guest_pa, size, rights, flags,
                             &shared);
    if (rc != E_OK) {
        if (out_error) *out_error = rc;
        note_error();
        return kNullCap;
    }

    const CapHandle cap_h = cap::alloc(*caller.cap_table, CAP_VM_SHARED, shared,
                                       RIGHT_READ | RIGHT_WRITE | RIGHT_VM_INSPECT | RIGHT_DUP);
    if (cap_h == kNullCap) {
        // Never leave a half-attached region behind: drop the creator reference,
        // which tears the mapping down and releases both retained records.
        object::release(shared->lifetime);
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }
    if (out_error) *out_error = E_OK;
    return cap_h;
}

SysError register_shared_memory_dma(Process& caller, CapHandle shared_cap, u32 perms) {
    SysError rc = E_OK;
    auto shared = shared_arena_from_cap(caller, shared_cap, RIGHT_WRITE, &rc);
    if (!shared) {
        return rc;
    }
    return shared_arena_register_dma(shared.get(), perms);
}

SysError inspect_shared_memory(Process& caller, CapHandle shared_cap, SharedArenaInfo* out) {
    if (!out) {
        return E_INVALID;
    }
    SysError rc = E_OK;
    auto shared = shared_arena_from_cap(caller, shared_cap, RIGHT_VM_INSPECT, &rc);
    if (!shared) {
        return rc;
    }

    SharedArenaInfo info{};
    info.shared_id = shared->shared_id;
    info.owner_vm_id = shared->owner_vm_id;
    info.rights = shared->rights;
    info.flags = shared->flags;
    info.guest_pa_base = shared->guest_pa_base;
    info.size = shared->size;
    info.dma_class = static_cast<u32>(shared->dma_class);
    info.mapped_stage2 = shared->mapped_stage2 ? 1 : 0;
    info.host_phys_base = shared->host_phys_base;
    *out = info;
    return E_OK;
}

SharedArenaStats shared_arena_stats() {
    SharedArenaStats stats{};
    stats.attached = __atomic_load_n(&g_shared_attached, __ATOMIC_RELAXED);
    stats.teardowns = __atomic_load_n(&g_shared_teardowns, __ATOMIC_RELAXED);
    stats.dma_registered = __atomic_load_n(&g_shared_dma_registered, __ATOMIC_RELAXED);
    return stats;
}

} // namespace vm
