#pragma once
#include "anko/types.h"
#include "midix/abi/diagnostics.h"

namespace vm {
// One CPU owns each row. Atomic fields avoid races with a remote diagnostic
// reader. A stuck writer must never make the reader spin indefinitely.
struct alignas(64) ProgressRecord {
    u64 sequence, stage, sampled_ns, detail0, detail1;
    void publish(u64 next_stage, u64 now, u64 a, u64 b) {
        const u64 old = __atomic_load_n(&sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&sequence, old + 1, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        __atomic_store_n(&stage, next_stage, __ATOMIC_RELAXED);
        __atomic_store_n(&sampled_ns, now, __ATOMIC_RELAXED);
        __atomic_store_n(&detail0, a, __ATOMIC_RELAXED);
        __atomic_store_n(&detail1, b, __ATOMIC_RELAXED);
        __atomic_store_n(&sequence, old + 2, __ATOMIC_RELEASE);
    }
    bool snapshot(u32 cpu, MxDiagVmProgress& out) const {
        for (u32 attempt = 0; attempt < 3; ++attempt) {
            const u64 before = __atomic_load_n(&sequence, __ATOMIC_ACQUIRE);
            if (before & 1) continue;
            MxDiagVmProgress row{MX_DIAG_VERSION, cpu, before,
                __atomic_load_n(&stage, __ATOMIC_RELAXED),
                __atomic_load_n(&sampled_ns, __ATOMIC_RELAXED),
                __atomic_load_n(&detail0, __ATOMIC_RELAXED),
                __atomic_load_n(&detail1, __ATOMIC_RELAXED)};
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (before == __atomic_load_n(&sequence, __ATOMIC_ACQUIRE)) {
                out = row;
                return true;
            }
        }
        return false;
    }
};
void note_progress(u64 stage, u64 detail0 = 0, u64 detail1 = 0);
bool read_progress(u32 cpu, MxDiagVmProgress& out);
} // namespace vm
