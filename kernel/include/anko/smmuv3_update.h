#pragma once
// STE replacement is a cold operation. All callbacks are required and must
// prove their part of publication/completion. Failure leaves the old/new root
// potentially reachable; the core retains both its binding and page pins.
#include "anko/smmuv3_enc.h"
#include "anko/syscall.h"

namespace iommu::smmuv3 {

struct SteUpdateOps {
    SysError (*publish)(void* ctx, const Ste& entry);
    SysError (*submit)(void* ctx, const Cmd& command);
    SysError (*sync)(void* ctx);
    void* ctx;
};

inline SysError replace_ste(Ste& entry, const Ste& next, u32 sid, const SteUpdateOps& ops) {
    if (!ops.publish || !ops.submit || !ops.sync || &entry == &next) return E_INVALID;
    const bool old_valid = (entry.w[0] & 1u) != 0;
    const bool old_stage2 = old_valid && ((entry.w[0] >> 1) & 7u) == kCfgS2Translate;
    const u16 old_vmid = static_cast<u16>(entry.w[4]);
    if (old_valid) {
        // Break before changing any live context field. CFGI alone does not
        // evict translations tagged by the previous VMID (IHI 0070 §5.2.4).
        __atomic_store_n(reinterpret_cast<u64*>(entry.w), 0ull, __ATOMIC_RELEASE);
        auto rc = ops.publish(ops.ctx, entry);
        if (rc != E_OK) return rc;
        rc = ops.submit(ops.ctx, make_cmd_cfgi_ste(sid));
        if (rc != E_OK) return rc;
        if (old_stage2) {
            rc = ops.submit(ops.ctx, make_cmd_tlbi_s12_vmall(old_vmid));
            if (rc != E_OK) return rc;
        }
        rc = ops.sync(ops.ctx);
        if (rc != E_OK) return rc;
    }
    for (u32 i = 2; i < 16; ++i) entry.w[i] = next.w[i];
    // Publish the payload while invalid, including on implementations whose
    // cache lines are smaller than an STE. Valid must become visible last.
    auto rc = ops.publish(ops.ctx, entry);
    if (rc != E_OK) return rc;
    const u64 first = static_cast<u64>(next.w[0]) | (static_cast<u64>(next.w[1]) << 32);
    __atomic_store_n(reinterpret_cast<u64*>(entry.w), first, __ATOMIC_RELEASE);
    rc = ops.publish(ops.ctx, entry);
    if (rc != E_OK) return rc;
    rc = ops.submit(ops.ctx, make_cmd_cfgi_ste(sid));
    return rc == E_OK ? ops.sync(ops.ctx) : rc;
}

} // namespace iommu::smmuv3
