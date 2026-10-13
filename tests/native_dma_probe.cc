// Exercise the real native arena registrar with captured IOMMU owner callbacks.
#include "anko/iommu.h"
#include "anko/log.h"
#include <cassert>
#include <cstring>
#include <cstdio>

namespace {
struct CapturedDomain {
    iommu::IommuDomain domain;
    iommu::IommuOwnerOps owner;
    u64 identity;
};
CapturedDomain domains[8];
u32 count;
iommu::IommuDomain* attached[16];
bool fail_attach;
bool fail_destroy;   // models a backend that cannot prove invalidation
bool backend_ready;  // models hardware routing authorizing DMA
bool recycle_destroyed_slot;
CapturedDomain& capture(iommu::IommuDomain& domain) {
    for (auto& entry : domains) if (&entry.domain == &domain) return entry;
    assert(false); return domains[0];
}
const iommu::IommuBackendOps& probe_backend() {
    static const iommu::IommuBackendOps ops{}; // software model: no hooks
    return ops;
}
}
namespace libk { void* memset(void* dst, int c, usize n) { return std::memset(dst, c, n); } }
namespace anko::log { void writef(Level, const char*, const char*, ...) {} }
namespace iommu {
// Software-model routing: segment 0, streams 0..15 are routed to the probe
// backend; anything else has no route and must be denied (default deny).
const IommuBackendOps* backend_for(IommuDeviceId device) {
    if (device.segment != 0 || device.requester_or_stream_id >= 16) return nullptr;
    return backend_ready ? &probe_backend() : nullptr;
}
SysError domain_create(u64 identity, const IommuOwnerOps& owner, const IommuBackendOps&, IommuDomain** out) {
    assert(count < 8);
    auto& entry = domains[count++];
    entry.domain.state_ = DomainState::Active;
    entry.owner = owner; entry.identity = identity; *out = &entry.domain;
    return E_OK;
}
SysError domain_map_batch(IommuDomain& domain, const IommuMap* maps, u32 number) {
    auto& entry = capture(domain);
    for (u32 i = 0; i < number; ++i) {
        PageOwnership owner{};
        assert(entry.owner.query(entry.owner.ctx, maps[i].pa, &owner));
        assert(owner.object == entry.identity);
    }
    return E_OK;
}
SysError domain_attach(IommuDomain& domain, IommuDeviceId device) {
    if (fail_attach) return E_FAULT;
    assert(device.requester_or_stream_id < 16);
    attached[device.requester_or_stream_id] = &domain;
    return E_OK;
}
SysError domain_destroy(IommuDomain* domain) {
    if (fail_destroy) return E_FAULT;
    for (auto& a : attached) if (a == domain) a = nullptr;
    domain->state_ = DomainState::Free;
    // Models another principal allocating the pool slot after reclamation.
    if (recycle_destroyed_slot) domain->state_ = DomainState::Active;
    return E_OK;
}
IommuDomain* stream_domain(IommuDeviceId device) { return attached[device.requester_or_stream_id]; }
DomainState domain_state(const IommuDomain& domain) { return domain.state_; }
}

int main() {
    using namespace iommu;
    backend_ready = true;
    assert(register_native_dma_arena({0, 1}, 0x10000, PAGE_SIZE) == E_OK);
    assert(register_native_dma_arena({0, 2}, 0x20000, PAGE_SIZE) == E_OK);
    assert(attached[1] && attached[2] && attached[1] != attached[2]);
    auto& a = capture(*attached[1]); auto& b = capture(*attached[2]);
    PageOwnership owner{};
    assert(a.identity != b.identity);
    assert(a.owner.query(a.owner.ctx, 0x10000, &owner));
    assert(owner.dma_class == DmaClass::NativeDevice);
    assert(!a.owner.query(a.owner.ctx, 0x20000, &owner));
    assert(!b.owner.query(b.owner.ctx, 0x10000, &owner));
    assert(register_native_dma_arena({0, 2}, 0x10000, PAGE_SIZE) == E_PERM);
    assert(register_native_dma_arena({0, 1}, 0x10000, PAGE_SIZE) == E_OK && count == 2);
    assert(register_native_dma_arena({0, 1}, 0x30000, PAGE_SIZE) == E_OK && count == 2);
    assert(a.owner.query(a.owner.ctx, 0x30000, &owner));
    assert(!b.owner.query(b.owner.ctx, 0x30000, &owner));
    fail_attach = true;
    assert(register_native_dma_arena({0, 3}, 0x40000, PAGE_SIZE) == E_FAULT);
    fail_attach = false;
    assert(register_native_dma_arena({0, 3}, 0x50000, PAGE_SIZE) == E_FAULT);
    assert(!attached[3]);
    assert(register_native_dma_arena({0, 4}, 0x10001, PAGE_SIZE) == E_INVALID);
    a.domain.state_ = DomainState::Quarantined;
    assert(register_native_dma_arena({0, 1}, 0x10000, PAGE_SIZE) == E_FAULT);
    a.domain.state_ = DomainState::Active;
    attached[1] = nullptr;
    assert(register_native_dma_arena({0, 1}, 0x10000, PAGE_SIZE) == E_FAULT);

    // Routing: an unrouted device is denied registration (default deny).
    assert(register_native_dma_arena({1, 1}, 0x90000, PAGE_SIZE) == E_NOTIMPL);
    // A routed backend that stopped authorizing DMA denies new arenas.
    backend_ready = false;
    assert(register_native_dma_arena({0, 5}, 0x90000, PAGE_SIZE) == E_NOTIMPL);
    backend_ready = true;
    // Unregistering an unknown owner is reported, not ignored.
    assert(unregister_native_dma_arenas({0, 42}) == E_NOTFOUND);
    // A failed destroy retains the owner and refuses further registration...
    fail_destroy = true;
    assert(unregister_native_dma_arenas({0, 2}) == E_FAULT);
    assert(register_native_dma_arena({0, 2}, 0x60000, PAGE_SIZE) == E_FAULT);
    // ...and a later retry, once invalidation is proven, reclaims cleanly.
    fail_destroy = false;
    recycle_destroyed_slot = true;
    assert(unregister_native_dma_arenas({0, 2}) == E_OK);
    recycle_destroyed_slot = false;
    assert(!attached[2]);
    // A cleared owner registers fresh, with a distinct identity and isolation.
    assert(register_native_dma_arena({0, 2}, 0x20000, PAGE_SIZE) == E_OK);
    assert(attached[2] && attached[2] != &b.domain);
    auto& b2 = capture(*attached[2]);
    assert(b2.identity != b.identity);
    assert(!b2.owner.query(b2.owner.ctx, 0x10000, &owner));
    assert(!b2.owner.query(b2.owner.ctx, 0x30000, &owner));
    std::puts("native DMA isolation probe ok");
}
