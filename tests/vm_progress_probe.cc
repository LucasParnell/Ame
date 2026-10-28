#include "anko/vm_progress.h"
#include <cassert>
#include <cstdio>
#include <thread>
int main() {
    vm::ProgressRecord record{};
    MxDiagVmProgress out{};
    assert(record.snapshot(1, out) && out.stage == 0 && out.cpu == 1);
    record.publish(MX_VM_PROGRESS_MALI_READ, 123, 0x1000, 7);
    assert(record.snapshot(1, out));
    assert(out.sequence == 2 && out.stage == MX_VM_PROGRESS_MALI_READ);
    assert(out.sampled_ns == 123 && out.detail0 == 0x1000 && out.detail1 == 7);
    // A writer stranded in its critical section cannot hang another CPU's
    // diagnostic syscall or leak a partially published row as stable.
    __atomic_store_n(&record.sequence, 3, __ATOMIC_RELEASE);
    out.detail0 = 99;
    assert(!record.snapshot(1, out) && out.detail0 == 99);
    vm::ProgressRecord concurrent{};
    std::thread writer([&] {
        for (unsigned i = 1; i <= 200000; ++i) concurrent.publish(i, 2ull*i, i, ~u64{i});
    });
    for (unsigned i = 0; i < 200000; ++i) {
        if (!concurrent.snapshot(1, out) || !out.sequence) continue;
        assert(out.stage == out.detail0 && out.sampled_ns == out.detail0*2);
        assert(out.detail1 == ~out.detail0);
    }
    writer.join();
    std::puts("vm progress probe ok");
}
