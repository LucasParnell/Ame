// Exercise the real native arena registrar with captured IOMMU owner callbacks.
#include "anko/smmuv3.h"
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
CapturedDomain& capture(iommu::IommuDomain& domain) {
    for (auto& entry : domains) if (&entry.domain == &domain) return entry;
    assert(false); return domains[0];
}
}
namespace libk { void* memset(void* dst, int c, usize n) { return std::memset(dst, c, n); } }
namespace anko::log { void writef(Level, const char*, const char*, ...) {} }
namespace iommu {
bool smmuv3_ready() { return true; }
const IommuBackendOps& smmuv3_backend() { static const IommuBackendOps ops{}; return ops; }
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
IommuDomain* stream_domain(IommuDeviceId device) { return attached[device.requester_or_stream_id]; }
DomainState domain_state(const IommuDomain& domain) { return domain.state_; }
}

int main() {
    using namespace iommu;
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
    std::puts("native DMA isolation probe ok");
}
