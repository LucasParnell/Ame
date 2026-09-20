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

#include "anko/smmuv3.h"

#include "anko/log.h"
#include "anko/mm.h"
#include "libk/string.h"

namespace iommu {

namespace {

constexpr u32 kMaxNativeArenas = 8;
constexpr u64 kNativeObject = 0x414D'454E'4154'4956ull; // "AMENATIV"

struct NativeArena {
    PhysAddr base;
    u64 size;
    u64 object;
    bool used;
    u8 pad[7];
};

NativeArena g_arenas[kMaxNativeArenas];
u32 g_arena_count;
IommuDomain* g_native_domain;
kernel::SpinLock g_native_lock;

bool native_owner_query(void*, PhysAddr pa, PageOwnership* out) {
    for (u32 i = 0; i < kMaxNativeArenas; ++i) {
        const NativeArena& a = g_arenas[i];
        if (!a.used) {
            continue;
        }
        if (pa >= a.base && pa < a.base + a.size) {
            out->object = a.object;
            out->dma_class = VmDmaClass::ApplianceRam;
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
    if (!smmuv3_ready()) {
        return E_OK; // no programmable IOMMU: the device already works
    }
    if (size == 0 || (base & (PAGE_SIZE - 1)) != 0 || (size & (PAGE_SIZE - 1)) != 0 ||
        (base + size) < base) {
        return E_INVALID;
    }
    kernel::LockGuard guard(g_native_lock);
    if (g_arena_count == kMaxNativeArenas) {
        return E_OVERFLOW;
    }

    IommuOwnerOps owner{};
    owner.query = native_owner_query;
    owner.release = native_release;
    owner.ctx = nullptr;

    if (!g_native_domain) {
        const SysError rc = domain_create(kNativeObject, owner, smmuv3_backend(), &g_native_domain);
        if (rc != E_OK) {
            ANKO_LOGF_ERROR("iommu/native", "native domain creation failed: %d",
                            static_cast<int>(rc));
            return rc;
        }
    }

    // Register the arena before mapping so the ownership query can see it.
    g_arenas[g_arena_count] = NativeArena{base, size, kNativeObject, true, {}};
    ++g_arena_count;

    IommuMap map{};
    map.iova = base;
    map.pa = base;
    map.length = size;
    map.perms = static_cast<u32>(DmaPerm::Read) | static_cast<u32>(DmaPerm::Write);
    map.reserved = 0;
    SysError rc = g_native_domain->map_batch(&map, 1);
    if (rc != E_OK) {
        ANKO_LOGF_ERROR("iommu/native", "arena map failed base=0x%llx size=0x%llx rc=%d",
                        static_cast<unsigned long long>(base),
                        static_cast<unsigned long long>(size), static_cast<int>(rc));
        return rc;
    }

    rc = g_native_domain->attach(device);
    if (rc != E_OK) {
        ANKO_LOGF_ERROR("iommu/native", "stream attach failed seg=%u sid=%u rc=%d", device.segment,
                        device.requester_or_stream_id, static_cast<int>(rc));
        return rc;
    }

    ANKO_LOGF_INFO("iommu/native", "arena registered seg=%u sid=%u base=0x%llx size=0x%llx",
                   device.segment, device.requester_or_stream_id,
                   static_cast<unsigned long long>(base), static_cast<unsigned long long>(size));
    return E_OK;
}

} // namespace iommu
