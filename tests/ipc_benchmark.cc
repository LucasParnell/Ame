// Same real IPC/scheduler fixture as regression tests, optimized host timings.
#define main ipc_regression_main
#include "ipc_probe.cc"
#undef main
#include <chrono>

template <class Operation> static void measure(const char* name,u32 rounds,Operation operation) {
    const auto begin=std::chrono::steady_clock::now();
    for (u32 i=0;i<rounds;++i) operation();
    const auto end=std::chrono::steady_clock::now();
    const auto ns=std::chrono::duration<double,std::nano>(end-begin).count()/rounds;
    std::printf("case=%s ns=%.2f\n",name,ns);
}

int main() {
    Fixture f; auto& runner=f.thread(); auto& waiter=f.thread(); dispatch(runner);
    const auto handle=ipc::doorbell_create(1);
    auto authority=cap::lookup(f.tables[0],handle,CAP_DOORBELL);
    auto* db=static_cast<Doorbell*>(authority->target);
    measure("notify_empty_small_registry",100000,[&] { assert(ipc::notify(handle,1)==E_OK); });
    // Registry pressure is explicit. The baseline still rediscovers processes,
    // waiters and creation roots among these unrelated objects on each call.
    unsigned unrelated[2048]{}; object::Record* records[2048]{};
    for (u32 i=0;i<2048;++i) records[i]=object::create(&unrelated[i],nullptr);
    measure("notify_empty",100000,[&] { assert(ipc::notify(handle,1)==E_OK); });
    measure("counter_read",100000,[&] { assert(ipc::doorbell_counter(handle,1)); });
    measure("wait_immediate",100000,[&] { assert(ipc::wait_doorbell(handle,0,1)==E_OK); });
    measure("notify_deferred_waiter",100000,[&] {
        waiter.state=TS_SLEEPING; waiter.execution_cpu=0; waiter.deferred_wake_cpu=kInvalidCpu;
        waiter.waiting_doorbell=db; db->waiter=&waiter;
        assert(ipc::notify_object(db)==E_OK && waiter.deferred_wake_cpu==0);
    });
    waiter.execution_cpu=kInvalidCpu; waiter.state=TS_SUSPENDED;
    measure("timer_empty",200000,[&] { ipc::poll_deadline_waiters(); });
    measure("doorbell_create_share_revoke",2000,[&] {
        const auto created=ipc::doorbell_create(1); assert(created);
        const auto shared=ipc::doorbell_share(1,created,2,RIGHT_NOTIFY); assert(shared);
        cap::revoke(f.tables[0],created); cap::revoke(f.tables[1],shared);
    });
    measure("ring_create_share_revoke",2000,[&] {
        const auto created=ipc::ring_create(1,4,0); assert(created);
        const auto shared=ipc::ring_share(1,created,2,RIGHT_READ); assert(shared);
        cap::revoke(f.tables[0],created); cap::revoke(f.tables[1],shared);
    });
    CapTable empty{};
    measure("cap_clear_empty",10000,[&] { cap::clear(empty); });
    const auto original=cap::alloc(empty,CAP_ARENA,&unrelated[0],RIGHT_READ|RIGHT_DUP); assert(original);
    for (u32 i=1;i<kCapTableSize;++i) assert(cap::dup(empty,original,RIGHT_READ));
    measure("cap_full_reject",100000,[&] { assert(!cap::dup(empty,original,RIGHT_READ)); });
    cap::clear(empty);

    Doorbell* streams[kMaxCpus]{};
    for (auto& stream:streams) { stream=ipc::doorbell_create_object(1); assert(stream); }
    constexpr u32 rounds=100000;
    std::thread cpus[kMaxCpus]; std::atomic<u32> ready{0}; std::atomic<bool> go{false};
    for (u32 cpu=0;cpu<kMaxCpus;++cpu) cpus[cpu]=std::thread([&,cpu] {
        audit_cpu=cpu; ready.fetch_add(1,std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {}
        for (u32 i=0;i<rounds;++i) assert(ipc::notify_object(streams[cpu])==E_OK);
    });
    while (ready.load(std::memory_order_acquire)!=kMaxCpus) {}
    const auto begin=std::chrono::steady_clock::now(); go.store(true,std::memory_order_release);
    for (auto& cpu:cpus) cpu.join();
    const auto end=std::chrono::steady_clock::now();
    const auto ns=std::chrono::duration<double,std::nano>(end-begin).count()/(rounds*kMaxCpus);
    std::printf("case=independent_cpu_notify ns=%.2f\n",ns);
    for (auto* stream:streams) {
        auto* root=object::acquire(stream); object::release(root); object::release(root);
    }
    for (auto* record:records) object::release(record);
    std::printf("layout doorbell=%zu cap_ref=%zu cap_table=%zu\n",sizeof(Doorbell),sizeof(cap::Ref),sizeof(CapTable));
}
