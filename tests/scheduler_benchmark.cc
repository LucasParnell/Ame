// Host timing of the production scheduler with the same hardware substitutions
// as the regression probe. Report warm host costs, never predicted phone times.
#define main scheduler_regression_main
#include "scheduler_probe.cc"
#undef main
#include <chrono>
#include <atomic>

static void shared_accounting(bool budgeted) {
    reset();
    constexpr u32 rounds=100000;
    constexpr u64 budget=1000000000000ull, period=10000000000000ull;
    const u32 pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,
                                           budgeted ? budget : 0,budgeted ? period : 0);
    assert(pid);
    CapTable caps{}; Process creator{}; creator.cap_table=&caps;
    creator.principal_id=pid; creator.principal_class=PRINCIPAL_NATIVE_SANDBOXED;
    SchedContext request{}; request.band=SCHED_BAND_FAIR;
    request.budget_ns=budget; request.period_ns=period;
    SysError error=E_OK;
    const auto context_cap=budgeted ? sched::sched_context_create(creator,request,&error) : kNullCap;
    auto context=budgeted ? sched::sched_context_from_cap(creator,context_cap,&error) : cap::Borrow<SchedContext>{nullptr};
    assert(!budgeted || context);
    Thread runners[kMaxCpus]{};
    std::thread cpus[kMaxCpus];
    std::atomic<u32> ready{0}; std::atomic<bool> go{false};
    for (u32 cpu=0;cpu<kMaxCpus;++cpu) {
        runners[cpu]=make_thread(pid,SCHED_BAND_FAIR);
        runners[cpu].sched_context=context.get();
        cpus[cpu]=std::thread([&,cpu] {
            audit_cpu=cpu; audit_now=1; dispatch(runners[cpu]);
            ready.fetch_add(1,std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {}
            for (u32 i=0;i<rounds;++i) {
                audit_now+=100;
                sched::account_current(sched::current_eki());
            }
            sched::current_eki().current=nullptr;
        });
    }
    while (ready.load(std::memory_order_acquire)!=kMaxCpus) {}
    const auto begin=std::chrono::steady_clock::now();
    go.store(true,std::memory_order_release);
    for (auto& cpu:cpus) cpu.join();
    const auto end=std::chrono::steady_clock::now();
    assert(sched::principal_runtime(pid)==u64{kMaxCpus}*rounds*100);
    const double ns=std::chrono::duration<double,std::nano>(end-begin).count()/(kMaxCpus*rounds);
    std::printf("case=shared_%s threads=%u accounting_ns=%.1f principal_bytes=%zu station_bytes=%zu\n",
                budgeted ? "budgeted" : "unbudgeted",kMaxCpus,ns,sizeof(PrincipalEntity),sizeof(Eki));
    cap::clear(caps);
    sched::release_principal(pid);
}

int main() {
    constexpr u32 iterations = 200000;
    for (u32 count : {1u, 8u, 32u}) {
        reset();
        Thread threads[32]{};
        for (u32 i = 0; i < count; ++i) {
            const u32 pid = sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED, 1024, 0, 0);
            threads[i] = make_thread(pid, static_cast<SchedBand>(1 + i % 5));
            threads[i].lifetime = object::create(&threads[i], nullptr);
            sched::enqueue(&threads[i]);
        }
        // Populate unrelated capability objects: the running thread already
        // owns its lifetime record and need not rediscover it in this registry.
        unsigned objects[1024];
        object::Record* records[1024];
        for (u32 i = 0; i < 1024; ++i) records[i] = object::create(&objects[i], nullptr);
        auto& station = sched::current_eki();
        sched::dequeue_thread(&threads[0]);
        dispatch(threads[0]);
        const auto begin = std::chrono::steady_clock::now();
        for (u32 i = 0; i < iterations; ++i) switch_after(1000);
        const auto end = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(end - begin).count() / iterations;
        std::printf("threads=%u dispatch_ns=%.1f switches=%llu principal_bytes=%zu station_bytes=%zu\n", count, ns,
                    (unsigned long long)station.context_switches, sizeof(PrincipalEntity), sizeof(Eki));
        station.current = nullptr;
        for (u32 i = 0; i < count; ++i) {
            sched::dequeue_thread(&threads[i]);
            object::release(threads[i].lifetime);
            sched::release_principal(threads[i].principal_id);
        }
        for (auto* record : records) object::release(record);
    }
    shared_accounting(false);
    shared_accounting(true);
}
