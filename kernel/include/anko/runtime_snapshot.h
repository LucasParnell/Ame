#pragma once
#include "anko/sched.h"
#include "midix/abi/diagnostics.h"

namespace sched {
// Caller holds the process lifecycle lock (or owns an offline test table).
// Copy at most eight compact rows while protected; user copies and formatting
// happen after unlocking. This reader adds no work to the accounting hot path.
inline bool copy_runtime_batch(Process* const* table, u32 limit, u32 first,
                                MxDiagRuntimeBatch* out) {
    if (!table || !out || !first || first > limit) return false;
    *out = {};
    out->version = MX_DIAG_VERSION;
    out->first_pid = first;
    const u32 end = first + ((limit - first + 1 < MX_DIAG_RUNTIME_ROWS)
        ? limit - first + 1 : MX_DIAG_RUNTIME_ROWS);
    out->next_pid = end <= limit ? end : 0;
    for (u32 pid = first; pid < end; ++pid) {
        const auto* process = table[pid];
        if (!process) continue;
        auto& row = out->rows[out->count++];
        row.identity = (static_cast<u64>(process->generation) << 32) | process->pid;
        row.runtime_ns = __atomic_load_n(&process->runtime_ns, __ATOMIC_RELAXED);
        row.pid = process->pid;
        row.principal_id = process->principal_id;
        row.flags = __atomic_load_n(&process->dying, __ATOMIC_ACQUIRE);
        for (u32 i = 0; i + 1 < sizeof(row.name); ++i) row.name[i] = process->name[i];
    }
    return true;
}
} // namespace sched
