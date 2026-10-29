#include "anko/runtime_snapshot.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
    Process first{}, last{};
    Process* table[MX_DIAG_PROCESS_LIMIT + 1]{};
    first.pid = 1; first.generation = 7; first.runtime_ns = 123456; first.principal_id = 9;
    std::memset(first.name, 'a', sizeof(first.name)); // Reader always terminates.
    last.pid = MX_DIAG_PROCESS_LIMIT; last.generation = 8; last.runtime_ns = 9000;
    table[1] = &first; table[MX_DIAG_PROCESS_LIMIT] = &last;
    MxDiagRuntimeBatch batch{};
    assert(!sched::copy_runtime_batch(table, MX_DIAG_PROCESS_LIMIT, 0, &batch));
    assert(!sched::copy_runtime_batch(table, MX_DIAG_PROCESS_LIMIT, 65, &batch));
    assert(!sched::copy_runtime_batch(table, MX_DIAG_PROCESS_LIMIT, 1, nullptr));
    assert(sched::copy_runtime_batch(table, MX_DIAG_PROCESS_LIMIT, 1, &batch));
    assert(batch.count == 1 && batch.next_pid == 9 && batch.rows[0].runtime_ns == 123456);
    assert(batch.rows[0].identity == (7ull << 32 | 1) && !batch.rows[0].name[31]);
    const auto old = batch.rows[0].identity;
    ++first.generation;
    first.runtime_ns = 0;
    assert(sched::copy_runtime_batch(table, MX_DIAG_PROCESS_LIMIT, 1, &batch));
    assert(batch.rows[0].identity != old && !batch.rows[0].runtime_ns);
    assert(sched::copy_runtime_batch(table, MX_DIAG_PROCESS_LIMIT, 9, &batch));
    assert(!batch.count && batch.next_pid == 17 && !batch.rows[0].identity);
    assert(sched::copy_runtime_batch(table, MX_DIAG_PROCESS_LIMIT, 57, &batch));
    assert(batch.count == 1 && !batch.next_pid && batch.rows[0].pid == 64);
    assert(sched::copy_runtime_batch(table, MX_DIAG_PROCESS_LIMIT, 64, &batch));
    assert(batch.count == 1 && !batch.next_pid);
    std::puts("runtime snapshot probe ok");
}
