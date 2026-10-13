// kernel/iommu/native.cc — IOMMU contexts for native (non-assigned) devices.
//
// Spec ref: AME GPU DRIVER APPLIANCE SPEC §8.7, §8.12.
//
// Once the SMMU enforces translation, a native driver such as drv.usb.xhci has
// to have a translation context or its DMA faults.  A native driver owns a
// pinned kernel DMA arena and presents physical addresses to its device, so the
// kernel registers exactly that arena — a scoped set of kernel-owned pages, not
// an identity map of RAM — as a domain with IOVA == PA and attaches the
// device's stream.  This is the "wired arena registered at the driver's stream
// ID" model; nothing outside a registered arena is reachable.

#include "anko/iommu.h"

#include "anko/log.h"
#include "anko/mm.h"
#include "libk/string.h"

namespace iommu {

namespace {

constexpr u32 kMaxNativeArenas = 8;
constexpr u32 kMaxNativeOwners = 8;
constexpr u64 kNativeObject = 0x414D'454E'4154'4956ull; // "AMENATIV"

struct NativeArena {
    PhysAddr base;
    u64 size;
};

struct NativeOwner {
    IommuDeviceId device;
    IommuDomain* domain;
    u64 identity;
    u32 arena_count;
    bool used;
    bool failed;
    NativeArena arenas[kMaxNativeArenas]; // dense cold ownership extents
};
NativeOwner g_owners[kMaxNativeOwners];
kernel::SpinLock g_native_lock;
u64 g_next_identity = kNativeObject;

bool native_owner_query(void* context, PhysAddr pa, PageOwnership* out) {
    const auto& owner = *static_cast<NativeOwner*>(context);
    for (u32 i = 0; i < owner.arena_count; ++i) {
        const NativeArena& a = owner.arenas[i];
        if (pa >= a.base && pa < a.base + a.size) {
            out->object = owner.identity;
            out->dma_class = DmaClass::NativeDevice;
            libk::memset(out->pad, 0, sizeof(out->pad));
            return true;
        }
    }
    return false;
}

void native_release(void*, PhysAddr, u64) {
    // Kernel-owned arenas stay pinned for the device's lifetime.
}

} // namespace

SysError register_native_dma_arena(IommuDeviceId device, PhysAddr base, u64 size) {
    if (size == 0 || (base & (PAGE_SIZE - 1)) != 0 || (size & (PAGE_SIZE - 1)) != 0 ||
        (base + size) < base) {
        return E_INVALID;
    }
    const auto* backend = backend_for(device);
    if (!backend) return E_NOTIMPL;
    kernel::LockGuard guard(g_native_lock);
    NativeOwner* native = nullptr;
    for (u32 i = 0; i < kMaxNativeOwners; ++i) {
        auto& owner = g_owners[i];
        if (owner.used && owner.device.segment == device.segment &&
            owner.device.requester_or_stream_id == device.requester_or_stream_id) native = &owner;
        for (u32 j = 0; j < owner.arena_count; ++j) {
            const auto& arena = owner.arenas[j];
            if (base < arena.base + arena.size && arena.base < base + size) {
                if (owner.device.segment == device.segment &&
                    owner.device.requester_or_stream_id == device.requester_or_stream_id &&
                    arena.base == base && arena.size == size) {
                    if (owner.failed || !owner.domain || domain_state(*owner.domain) != DomainState::Active ||
                        stream_domain(device) != owner.domain) return E_FAULT;
                    return E_OK;
                }
                return E_PERM;
            }
        }
    }
    if (!native) {
        for (u32 i = 0; i < kMaxNativeOwners; ++i) {
            if (g_owners[i].used) continue;
            native = &g_owners[i];
            *native = {};
            native->used = true;
            native->device = device;
            if (g_next_identity == ~0ull) { native->used = false; return E_OVERFLOW; }
            native->identity = g_next_identity++;
            break;
        }
    }
    if (!native || native->arena_count == kMaxNativeArenas) return E_OVERFLOW;
    if (native->failed) return E_FAULT;

    IommuOwnerOps owner{};
    owner.query = native_owner_query;
    owner.release = native_release;
    owner.ctx = native;

    if (!native->domain) {
        const SysError rc = domain_create(native->identity, owner, *backend, &native->domain);
        if (rc != E_OK) {
            native->failed = true;
            ANKO_LOGF_ERROR("iommu/native", "native domain creation failed: %d",
                            static_cast<int>(rc));
            return rc;
        }
    }

    // Register the arena before mapping so the ownership query can see it.
    native->arenas[native->arena_count++] = NativeArena{base, size};

    IommuMap map{};
    map.iova = base;
    map.pa = base;
    map.length = size;
    map.perms = static_cast<u32>(DmaPerm::Read) | static_cast<u32>(DmaPerm::Write);
    map.reserved = 0;
    SysError rc = domain_map_batch(*native->domain, &map, 1);
    if (rc != E_OK) {
        // Failed publication may have quarantined the domain. Retain its
        // ownership records and pinned pages; never recycle uncertain DMA.
        native->failed = true;
        ANKO_LOGF_ERROR("iommu/native", "arena map failed base=0x%llx size=0x%llx rc=%d",
                        static_cast<unsigned long long>(base),
                        static_cast<unsigned long long>(size), static_cast<int>(rc));
        return rc;
    }

    if (stream_domain(device) == native->domain) return E_OK;
    rc = domain_attach(*native->domain, device);
    if (rc != E_OK) {
        native->failed = true;
        ANKO_LOGF_ERROR("iommu/native", "stream attach failed seg=%u sid=%u rc=%d", device.segment,
                        device.requester_or_stream_id, static_cast<int>(rc));
        return rc;
    }

    ANKO_LOGF_INFO("iommu/native", "arena registered seg=%u sid=%u base=0x%llx size=0x%llx",
                   device.segment, device.requester_or_stream_id,
                   static_cast<unsigned long long>(base), static_cast<unsigned long long>(size));
    return E_OK;
}

SysError unregister_native_dma_arenas(IommuDeviceId device) {
    kernel::LockGuard guard(g_native_lock);
    for (auto& owner : g_owners) {
        if (!owner.used || owner.device.segment != device.segment ||
            owner.device.requester_or_stream_id != device.requester_or_stream_id) continue;
        if (owner.domain) {
            if (domain_destroy(owner.domain) != E_OK) {
                owner.failed = true;
                return E_FAULT;
            }
        }
        owner = {};
        return E_OK;
    }
    return E_NOTFOUND;
}

} // namespace iommu
