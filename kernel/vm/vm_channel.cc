#include "anko/vm.h"
#include "libk/string.h"
#include "midix/abi/dango_channel_guest.h"

namespace vm {

namespace {
void release_creator(void* target) {
    if (!target) return;
    auto* record = object::acquire(target);
    object::release(record); object::release(record);
}


u32 g_next_channel_id = 1;

// Monotonic appliance channel boot generation. Every create_channel() is a
// fresh channel rendezvous, so the value advances and every reference captured
// under the previous generation is stale (spec §13.3). Never zero: the guest
// treats a zero descriptor field as "no generation yet".
u64 g_channel_boot_generation = 0;

u64 next_boot_generation() {
    u64 g = __atomic_add_fetch(&g_channel_boot_generation, 1ull, __ATOMIC_RELAXED);
    if (g == 0) g = __atomic_add_fetch(&g_channel_boot_generation, 1ull, __ATOMIC_RELAXED);
    return g;
}

bool ring_rights_valid(u32 rights) {
    return rights != 0 && (rights & ~(RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP)) == 0;
}

bool doorbell_rights_valid(u32 rights) {
    return rights != 0 && (rights & ~(RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP)) == 0;
}

} // namespace

CapHandle create_channel(Process& caller, CapHandle vm_cap, u32 order, u32 flags,
                         SysError* out_error) {
    SysError rc = E_OK;
    auto v = vm_from_cap(caller, vm_cap, RIGHT_VM_CONTROL, &rc);
    if (!v) {
        if (out_error) *out_error = rc;
        return kNullCap;
    }
    if (flags != 0 || order < 4 || order > 10) {
        if (out_error) *out_error = E_INVALID;
        return kNullCap;
    }
    if (v->channel_count >= kMaxVmChannels || g_next_channel_id == 0) {
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    Ring* h2g = ipc::ring_create_object(caller.pid, order, flags);
    Ring* g2h = ipc::ring_create_object(caller.pid, order, flags);
    Doorbell* host_db = ipc::doorbell_create_object(caller.pid);
    Doorbell* guest_db = ipc::doorbell_create_object(caller.pid);
    if (!h2g || !g2h || !host_db || !guest_db) {
        release_creator(h2g); release_creator(g2h); release_creator(host_db); release_creator(guest_db);
        if (out_error) *out_error = E_NOMEM;
        note_error();
        return kNullCap;
    }

    VmChannel* channel = static_cast<VmChannel*>(
        mm::slab_alloc(sizeof(VmChannel), alignof(VmChannel)));
    if (!channel) {
        release_creator(h2g); release_creator(g2h); release_creator(host_db); release_creator(guest_db);
        if (out_error) *out_error = E_NOMEM;
        note_error();
        return kNullCap;
    }
    libk::memset(channel, 0, sizeof(*channel));
    channel->channel_id = g_next_channel_id++;
    channel->owner_vm_id = v->vm_id;
    channel->host_to_guest = h2g;
    channel->guest_to_host = g2h;
    channel->host_doorbell = host_db;
    channel->guest_doorbell = guest_db;
    channel->order = order;
    channel->flags = flags;
    // (Re)establishing the appliance's channel advances its boot generation;
    // any reference the peer captured under the previous generation is now
    // stale and is rejected by the generation check in map/stage/notify.
    channel->boot_generation = next_boot_generation();
    v->channel_generation = channel->boot_generation;

    channel->owner_lifetime = v->lifetime; object::retain(channel->owner_lifetime);
    channel->lifetime = object::create(channel, dispose_channel);
    if (!channel->lifetime) { dispose_channel(channel); if (out_error) *out_error = E_NOMEM; return kNullCap; }
    const CapHandle cap_h = cap::alloc(*caller.cap_table, CAP_VM_CHANNEL, channel,
                                       RIGHT_VM_CHANNEL_SEND |
                                           RIGHT_VM_CHANNEL_RECV |
                                           RIGHT_VM_CHANNEL_NOTIFY |
                                           RIGHT_VM_CHANNEL_INSPECT |
                                           RIGHT_DUP);
    if (cap_h == kNullCap) {
        object::release(channel->lifetime);
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    v->channels[v->channel_count++] = channel;
    metric_inc(VM_DEBUG_CHANNELS_CREATED);
    if (out_error) *out_error = E_OK;
    return cap_h;
}

CapHandle channel_endpoint(Process& caller, CapHandle channel_cap, u32 endpoint_kind,
                           u32 rights, SysError* out_error) {
    u32 needed = RIGHT_VM_CHANNEL_INSPECT;
    if (endpoint_kind == VM_CHANNEL_HOST_TO_GUEST_RING || endpoint_kind == VM_CHANNEL_HOST_TO_GUEST_DOORBELL) {
        needed |= RIGHT_VM_CHANNEL_SEND;
    } else if (endpoint_kind == VM_CHANNEL_GUEST_TO_HOST_RING || endpoint_kind == VM_CHANNEL_GUEST_TO_HOST_DOORBELL) {
        needed |= RIGHT_VM_CHANNEL_RECV;
    }
    if ((endpoint_kind == VM_CHANNEL_HOST_TO_GUEST_DOORBELL ||
                endpoint_kind == VM_CHANNEL_GUEST_TO_HOST_DOORBELL) &&
               (rights & RIGHT_NOTIFY)) {
        needed |= RIGHT_VM_CHANNEL_NOTIFY;
    }

    SysError rc = E_OK;
    auto channel = channel_from_cap(caller, channel_cap, needed, &rc);
    if (!channel) {
        if (out_error) *out_error = rc;
        return kNullCap;
    }
    // A stale generation hands out nothing: the endpoint would name an object
    // the appliance has already invalidated.
    if (Vm* owner = find_vm_by_id(channel->owner_vm_id);
        !owner || !channel_generation_current(*owner, *channel)) {
        if (out_error) *out_error = E_NOTFOUND;
        note_error();
        return kNullCap;
    }

    CapHandle endpoint = kNullCap;
    switch (endpoint_kind) {
    case VM_CHANNEL_HOST_TO_GUEST_RING:
        if (!ring_rights_valid(rights)) rc = E_INVALID;
        else endpoint = ipc::ring_cap_alloc(caller.pid, channel->host_to_guest, rights);
        break;
    case VM_CHANNEL_GUEST_TO_HOST_RING:
        if (!ring_rights_valid(rights)) rc = E_INVALID;
        else endpoint = ipc::ring_cap_alloc(caller.pid, channel->guest_to_host, rights);
        break;
    case VM_CHANNEL_HOST_TO_GUEST_DOORBELL:
        if (!doorbell_rights_valid(rights)) rc = E_INVALID;
        else endpoint = ipc::doorbell_cap_alloc(caller.pid, channel->host_doorbell, rights);
        break;
    case VM_CHANNEL_GUEST_TO_HOST_DOORBELL:
        if (!doorbell_rights_valid(rights)) rc = E_INVALID;
        else endpoint = ipc::doorbell_cap_alloc(caller.pid, channel->guest_doorbell, rights);
        break;
    default:
        rc = E_INVALID;
        break;
    }

    if (rc != E_OK) {
        if (out_error) *out_error = rc;
        note_error();
        return kNullCap;
    }
    if (endpoint == kNullCap) {
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    if (out_error) *out_error = E_OK;
    return endpoint;
}

SysError inspect_channel(Process& caller, CapHandle channel_cap, VmChannelInfo* out) {
    if (!out) return E_INVALID;
    SysError rc = E_OK;
    auto channel = channel_from_cap(caller, channel_cap,
                                          RIGHT_VM_CHANNEL_INSPECT, &rc);
    if (!channel) return rc;

    VmChannelInfo info{};
    info.channel_id = channel->channel_id;
    info.owner_vm_id = channel->owner_vm_id;
    info.order = channel->order;
    info.flags = channel->flags;
    info.messages_host_to_guest = channel->messages_host_to_guest;
    info.messages_guest_to_host = channel->messages_guest_to_host;
    info.endpoint_present_mask =
        (channel->host_to_guest ? 1ull : 0ull) |
        (channel->guest_to_host ? 2ull : 0ull) |
        (channel->host_doorbell ? 4ull : 0ull) |
        (channel->guest_doorbell ? 8ull : 0ull);
    *out = info;
    return E_OK;
}

void dispose_channel(void* target) {
    auto* channel = static_cast<VmChannel*>(target);
    auto* owner = channel->owner_lifetime;
    release_creator(channel->host_to_guest); release_creator(channel->guest_to_host);
    release_creator(channel->host_doorbell); release_creator(channel->guest_doorbell);
    if (channel->descriptor_page) mm::free_page(channel->descriptor_page);
    if (channel->shm_page) mm::free_page(channel->shm_page);
    mm::slab_free(channel, sizeof(VmChannel), alignof(VmChannel));
    object::release(owner);
}

// All channel mapping/HVC/notify operations run under CallGuard. No raw
// channel/vCPU pointer is installed in a doorbell or asynchronous callback.
// VM teardown destroys stage 2 before releasing these pages and endpoints.
SysError map_channel(Process& caller, CapHandle channel_cap, CapHandle vcpu_cap,
                     u64 ipa, u64 reserved_size, u32 guest_irq) {
    SysError rc = E_OK;
    auto channel = channel_from_cap(caller, channel_cap,
        RIGHT_VM_CHANNEL_SEND | RIGHT_VM_CHANNEL_RECV |
        RIGHT_VM_CHANNEL_NOTIFY | RIGHT_VM_CHANNEL_INSPECT, &rc);
    if (!channel) return rc;
    auto cpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_CONTROL, &rc);
    if (!cpu) return rc;
    if (cpu->owner_vm_id != channel->owner_vm_id) return E_PERM;
    Vm* owner = find_vm_by_id(channel->owner_vm_id);
    if (!owner || owner->state == VM_DEAD) return E_DEAD;
    // Stale generation: the peer is holding a descriptor from a previous
    // appliance boot. Reject before mutating anything.
    if (!channel_generation_current(*owner, *channel)) return E_NOTFOUND;
    if (channel->target || owner->stage2.active) return E_BUSY;
    if (!ipa || (ipa & (PAGE_SIZE - 1)) || (reserved_size & (PAGE_SIZE - 1)) ||
        guest_irq < 32 || guest_irq >= kVgicMaxIrqs) return E_INVALID;
    if (device_irq_vector_used(*cpu, guest_irq)) return E_BUSY;
    for (u32 i = 0; i < owner->channel_count; ++i) {
        const auto* other = owner->channels[i];
        if (other && other->target == cpu.get() && other->guest_irq == guest_irq) return E_BUSY;
    }
    const u64 ring_bytes = (sizeof(RingHeader) + (1ull << channel->order) * sizeof(RingSlot) +
                            PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    const u64 bytes = PAGE_SIZE + 2 * ring_bytes;
    if (reserved_size < bytes || ipa >= (1ull << owner->stage2.ipa_bits) ||
        reserved_size > (1ull << owner->stage2.ipa_bits) - ipa) return E_INVALID;
    rc = stage2_validate_no_overlap(&owner->stage2, ipa, reserved_size);
    if (rc != E_OK) return rc;
    if ((bytes + DANGO_SHM_GUEST_SIZE) / PAGE_SIZE >
        kMaxStage2Mappings - owner->stage2.mapped_pages) return E_OVERFLOW;
    const PhysAddr page = mm::alloc_page();
    if (!page) return E_NOMEM;
    libk::memset(reinterpret_cast<void*>(page), 0, PAGE_SIZE);
    auto& layout = *reinterpret_cast<dango_channel_layout*>(page);
    layout.desc = {DANGO_CH_MAGIC, DANGO_CH_VERSION, 0, channel->order, DANGO_CH_ARM_SUPPORTED,
                   channel->boot_generation, ipa + PAGE_SIZE, ipa + PAGE_SIZE + ring_bytes,
                   0, 0, guest_irq, 0};
    layout.channel_id = channel->channel_id;
    layout.ring_bytes = static_cast<u32>(ring_bytes);
    layout.mapping_bytes = static_cast<u32>(2 * ring_bytes);
    u64 off = 0;
    for (; off < bytes; off += PAGE_SIZE) {
        PhysAddr pa = page;
        if (off >= PAGE_SIZE + ring_bytes)
            pa = channel->guest_to_host->phys_base + off - PAGE_SIZE - ring_bytes;
        else if (off >= PAGE_SIZE)
            pa = channel->host_to_guest->phys_base + off - PAGE_SIZE;
        rc = stage2_map_page(&owner->stage2, ipa + off, pa,
                            RIGHT_READ | (off ? RIGHT_WRITE : 0));
        if (rc != E_OK) break;
    }
    if (rc != E_OK) {
        while (off) { off -= PAGE_SIZE; (void)stage2_unmap_page(&owner->stage2, ipa + off); }
        mm::free_page(page);
        return rc;
    }
    // Phase L: the shared-arena descriptor window is always mapped before first
    // guest entry, read-only, as one zeroed page repeated across the window, so
    // the guest dango_shm driver always reads an empty table instead of taking a
    // stage-2 translation fault on an IPA the host has not published.
    const PhysAddr shm_page = mm::alloc_page();
    if (!shm_page) {
        for (u64 u = 0; u < bytes; u += PAGE_SIZE) (void)stage2_unmap_page(&owner->stage2, ipa + u);
        mm::free_page(page);
        return E_NOMEM;
    }
    libk::memset(reinterpret_cast<void*>(shm_page), 0, PAGE_SIZE);
    u64 shm_off = 0;
    for (; shm_off < DANGO_SHM_GUEST_SIZE; shm_off += PAGE_SIZE) {
        rc = stage2_map_page(&owner->stage2, DANGO_SHM_GUEST_BASE + shm_off, shm_page, RIGHT_READ);
        if (rc != E_OK) break;
    }
    if (rc != E_OK) {
        while (shm_off) {
            shm_off -= PAGE_SIZE;
            (void)stage2_unmap_page(&owner->stage2, DANGO_SHM_GUEST_BASE + shm_off);
        }
        mm::free_page(shm_page);
        for (u64 u = 0; u < bytes; u += PAGE_SIZE) (void)stage2_unmap_page(&owner->stage2, ipa + u);
        mm::free_page(page);
        return rc;
    }
    // These pages are deliberately not VmMemory objects: collect_vm_ram()
    // cannot authorize them for GPU DMA. There is no IOMMU map here.
    channel->shm_page = shm_page;
    channel->descriptor_page = page;
    channel->target = cpu.get();
    channel->guest_base = ipa;
    channel->guest_irq = guest_irq;
    return E_OK;
}

// Publish the VM's shared staging window (guest IPA + size) into the channel
// descriptor. Staging pages are host-writable and guest-mappable but never
// DMA-authorizable; the window is validated against free stage-2 IPA space so
// the guest can map exactly those pages. Runs under CallGuard like the other
// channel operations, and is a no-op-safe failure until map_channel() has
// installed the descriptor page.
SysError stage_channel(Process& caller, CapHandle channel_cap, u64 ipa, u64 size) {
    SysError rc = E_OK;
    auto channel = channel_from_cap(caller, channel_cap,
        RIGHT_VM_CHANNEL_INSPECT | RIGHT_VM_CHANNEL_SEND | RIGHT_VM_CHANNEL_RECV, &rc);
    if (!channel) return rc;
    // No descriptor page yet: map_channel() has not run. Fail safe, do nothing.
    if (!channel->descriptor_page) return E_INVALID;
    Vm* owner = find_vm_by_id(channel->owner_vm_id);
    if (!owner || owner->state == VM_DEAD) return E_DEAD;
    if (!channel_generation_current(*owner, *channel)) return E_NOTFOUND;
    if (ipa == 0 || size == 0) return E_INVALID;
    if ((ipa & (PAGE_SIZE - 1)) != 0 || (size & (PAGE_SIZE - 1)) != 0) return E_INVALID;
    if (ipa != kStagingWindowBase || size != kStagingWindowSize) return E_INVALID;
    if (ipa >= (1ull << owner->stage2.ipa_bits) ||
        size > (1ull << owner->stage2.ipa_bits) - ipa) {
        return E_INVALID;
    }

    // The window must exactly describe the VM's shared staging object.
    VmMemory* staging = nullptr;
    for (u32 i = 0; i < owner->memory_count; ++i) {
        VmMemory* mem = owner->memory_objects[i];
        if (mem && !mem->dead && mem->shared_staging) { staging = mem; break; }
    }
    if (!staging || staging->size != size) return E_INVALID;

    rc = stage2_validate_no_overlap(&owner->stage2, ipa, size);
    if (rc != E_OK) return rc;

    auto& layout = *reinterpret_cast<dango_channel_layout*>(channel->descriptor_page);
    layout.desc.shared_staging_pa = ipa;
    layout.desc.shared_staging_size = size;
    channel->staging_base = ipa;
    channel->staging_size = size;
    return E_OK;
}

SysError notify_channel(Process& caller, CapHandle channel_cap) {
    SysError rc = E_OK;
    auto channel = channel_from_cap(caller, channel_cap, RIGHT_VM_CHANNEL_NOTIFY, &rc);
    if (!channel) return rc;
    Vm* owner = find_vm_by_id(channel->owner_vm_id);
    if (!owner || owner->state == VM_DEAD) return E_DEAD;
    if (!channel_generation_current(*owner, *channel)) return E_NOTFOUND;
    if (!channel->target) return E_INVALID;
    // Back-pressure, never a silent drop or a spin: a full host->guest ring
    // means the guest has not drained, so there is nothing to publish into and
    // the caller gets a distinct status it can retry on.
    if (channel->host_to_guest && channel_ring_full(*channel->host_to_guest)) return E_BUSY;
    channel_publish(*channel);
    return E_OK;
}

// Appliance-death propagation. Called when a vCPU is destroyed (VM retire,
// owner-process death) before its storage is released: detach every channel
// whose target is this vCPU and clear the guest vector so no publication can
// reach a freed vCPU and no stale vGIC pending bit survives the teardown.
void channel_detach_vcpu(Vcpu& cpu) {
    Vm* owner = find_vm_by_id(cpu.owner_vm_id);
    if (!owner) return;
    for (u32 i = 0; i < owner->channel_count; ++i) {
        VmChannel* channel = owner->channels[i];
        if (!channel || channel->target != &cpu) continue;
        vgic_pending_clear(cpu, channel->guest_irq);
        channel->armed = false;
        channel->target = nullptr;
    }
}


} // namespace vm
