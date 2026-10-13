// Scheduler regression checks. The runner substitutes privileged hardware
// boundaries; queue, accounting, context, and ownership logic use kernel code.
#include "anko/sched.h"
#include "anko/log.h"
#include "sched_internal.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <thread>
thread_local unsigned long long audit_now = 1;
thread_local unsigned int audit_cpu = 0;
unsigned ipis = 0;
unsigned idle_waits = 0;
Thread* wake_on_idle = nullptr;
bool retired_storage = false;
bool verify_retirement = false;
void audit_wait() {
    assert(++idle_waits < 1000);
    if (verify_retirement) assert(retired_storage);
    audit_now += 10000000;
    if (wake_on_idle) { auto* t=wake_on_idle; wake_on_idle=nullptr; sched::enqueue(t); }
    sched::tick();
}
namespace mm {
void* slab_alloc(usize n, usize alignment) { void* p=nullptr; return posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, n) ? nullptr : p; }
void slab_free(void* p, usize, usize) { std::free(p); }
void free_page(PhysAddr) { std::abort(); }
void vm_activate(u64) {}
}
namespace libk { void* memset(void* p, int v, usize n) { return std::memset(p,v,n); } }
namespace anko::log { void write(Level, const char*, const char*) {} void writef(Level, const char*, const char*, ...) {} }
[[noreturn]] void panic(const char* text) { std::fprintf(stderr,"%s\n",text); std::abort(); }
namespace smp { void send_wake_ipi(u32) { ++ipis; } }
namespace sched {
extern "C" [[noreturn]] void enter_user_frame(const TrapFrame*) { std::abort(); }
object::Ref<Process> find_process(u32) { return nullptr; }
object::Ref<Thread> find_thread(u32) { return nullptr; }
bool termination_pending() { return false; }
void finish_thread(Thread* t) { assert(t->lifetime); object::release(t->lifetime); }
void thread_destroy(Thread*) { std::abort(); }
void process_destroy(Process*) { std::abort(); }
}
Thread make_thread(u32 principal, SchedBand band, u32 weight=NICE_0_WEIGHT) {
    Thread t{}; t.execution_cpu=t.deferred_wake_cpu=kInvalidCpu; t.principal_id=principal; t.state=TS_SUSPENDED;
    t.sched.sched_class=band; t.sched.weight=weight; t.home_cpu=t.last_cpu=kInvalidCpu;
    return t;
}
void reset() { audit_cpu=0; audit_now=1; ipis=0; idle_waits=0; sched::init(); }
void dispatch(Thread& t) { auto& e=sched::current_eki(); e.current=&t; t.state=TS_RUNNING; t.execution_cpu=audit_cpu; t.last_start_ns=t.slice_start_ns=audit_now; }
void switch_after(u64 ns) { audit_now += ns; TrapFrame frame{}; sched::request_switch(true, SWITCH_TIMER); sched::post_syscall(&frame); }

void heap_checks() {
    Thread nodes[kFairQueueCap]{}; bool present[kFairQueueCap]{}; FairThreadQueue queue{};
    u32 rng=0x42c0ffee;
    auto random=[&] { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; };
    for (int step=0; step<30000; ++step) {
        const u32 i=random()%kFairQueueCap;
        if (!present[i]) {
            nodes[i].vruntime_ns=random(); nodes[i].sched.sched_class=SCHED_BAND_FAIR;
            assert(sched::ftq_push(queue,&nodes[i])); present[i]=true;
        } else if (random()&1) {
            if (random()&1) nodes[i].heap_idx=kFairQueueCap+1; // exercise lookup fallback
            assert(sched::ftq_remove(queue,&nodes[i])); present[i]=false;
        } else {
            auto* removed=sched::ftq_pop_min(queue); assert(removed); present[removed-nodes]=false;
        }
        u32 count=0; u64 min=~0ull;
        for (u32 j=0;j<kFairQueueCap;++j) if (present[j]) { ++count; if(nodes[j].vruntime_ns<min) min=nodes[j].vruntime_ns; }
        assert(queue.size==count);
        if (count) assert(sched::effective_key(sched::ftq_peek_min(queue))==min);
        for (u32 j=0;j<queue.size;++j) {
            assert(queue.data[j]->heap_idx==j);
            if(j) assert(sched::effective_key(queue.data[(j-1)/2])<=sched::effective_key(queue.data[j]));
        }
    }
    std::puts("thread heap: 30000 mixed insert/pop/remove operations preserve minimum and indexes");
}

int main() {
    heap_checks();
    static_assert(alignof(Eki) >= 64);
    static_assert(__builtin_offsetof(Eki, pending_wakes) % 64 == 0);
    std::printf("layout: Thread=%zu PrincipalEntity=%zu Eki=%zu align=%zu pending=%zu\n",
        sizeof(Thread), sizeof(PrincipalEntity), sizeof(Eki), alignof(Eki), __builtin_offsetof(Eki,pending_wakes));
    reset();
    auto pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,1000000,100000000);
    auto budgeted=make_thread(pid,SCHED_BAND_FAIR);
    auto waiter=make_thread(pid,SCHED_BAND_FAIR);
    dispatch(budgeted); sched::enqueue(&waiter);
    audit_now+=2000000; sched::tick(); assert(sched::principal_throttled(pid));
    TrapFrame frame{}; sched::post_syscall(&frame);
    assert(idle_waits && !sched::principal_throttled(pid));
    assert(sched::current()==&waiter && sched::current_eki().fair_principals.size==1);
    assert(budgeted.on_runqueue && !waiter.on_runqueue);
    std::puts("budget exhaustion waits for refill and preserves queued work");

    reset(); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto retired=make_thread(pid,SCHED_BAND_FAIR); sched::enqueue(&retired); sched::dequeue_thread(&retired); sched::release_principal(pid);
    auto recycled=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0); assert(recycled==pid);
    auto replacement=make_thread(recycled,SCHED_BAND_FAIR); sched::enqueue(&replacement);
    auto& queue=sched::current_eki().fair_principals;
    assert(queue.size==1 && queue.data[0]->principal_id==pid);
    std::puts("principal reuse leaves one heap entry");

    reset(); sched::set_online_cpu_count(3);
    auto p1=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto p2=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto restricted=make_thread(p1,SCHED_BAND_FAIR); restricted.affinity_mask=5;
    auto other=make_thread(p2,SCHED_BAND_FAIR); sched::enqueue(&restricted); sched::enqueue(&other);
    audit_cpu=1; sched::maybe_send_steal_request(sched::current_eki()); assert(sched::current_eki().steal_req_in_flight);
    audit_cpu=0; sched::handle_pending_steal_requests(sched::current_eki());
    assert(restricted.cpu_id==0 && restricted.on_runqueue && other.cpu_id==1);
    std::puts("stealing respects destination affinity");

    reset(); sched::set_online_cpu_count(2); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    Thread siblings[4]; for(auto& t:siblings) { t=make_thread(pid,SCHED_BAND_FAIR); sched::enqueue(&t); }
    audit_cpu=1; sched::maybe_send_steal_request(sched::current_eki()); assert(sched::current_eki().steal_req_in_flight);
    audit_cpu=0; sched::handle_pending_steal_requests(sched::current_eki());
    assert(sched::eki_for_cpu(0).published_steal_load==3);
    std::puts("one principal with several threads supplies stealable work");

    reset(); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto sender=make_thread(pid,SCHED_BAND_FAIR); auto receiver=make_thread(pid,SCHED_BAND_LATENCY);
    dispatch(sender); sender.state=TS_BLOCKED; receiver.state=TS_SLEEPING;
    sched::set_handoff_next(&receiver); sched::request_switch(false,SWITCH_HANDOFF); sched::post_syscall(&frame);
    assert(sched::current()==&receiver && receiver.slice_ns && receiver.handoff_deadline_ns);
    audit_now+=10000000; sched::tick(); assert(sched::switch_pending());
    assert(!receiver.has_donation && !receiver.handoff_deadline_ns);
    std::puts("handoff receiver has a bounded quantum");

    reset(); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto sleeper=make_thread(pid,SCHED_BAND_FAIR); dispatch(sleeper);
    audit_now+=4000000; sched::block(&sleeper); sched::account_current(sched::current_eki());
    assert(sleeper.runtime_ns==4000000 && sched::principal_runtime(pid)==4000000);
    std::puts("blocking charges the last execution interval");

    reset(); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto low=make_thread(pid,SCHED_BAND_FAIR,1024); auto high=make_thread(pid,SCHED_BAND_FAIR,4096);
    dispatch(low); sched::enqueue(&high);
    for(int i=0;i<2000;i++) switch_after(10000000);
    const double share=double(high.runtime_ns)/double(low.runtime_ns+high.runtime_ns);
    assert(share>0.79 && share<0.81);
    std::printf("weights 1:4: high thread share %.3f\n",share);

    reset(); p1=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0); p2=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto latency=make_thread(p1,SCHED_BAND_LATENCY); auto fair=make_thread(p2,SCHED_BAND_FAIR);
    dispatch(latency); sched::enqueue(&fair);
    for(int i=0;i<100;i++) switch_after(10000000);
    assert(fair.runtime_ns>=400000000 && latency.runtime_ns>=400000000);
    std::puts("latency-band work cannot starve another fair principal");

    reset(); p1=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0); p2=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto running_fair=make_thread(p1,SCHED_BAND_FAIR); auto waking_vm=make_thread(p2,SCHED_BAND_VM_LATENCY);
    dispatch(running_fair); sched::wake_local(&waking_vm); assert(sched::switch_pending());
    assert(sched::band_rank(SCHED_BAND_VM_LATENCY)<sched::band_rank(SCHED_BAND_FAIR));
    std::puts("wake precedence matches dispatch precedence");

    reset(); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    SchedContext context{}; context.budget_ns=1000000; context.period_ns=100000000; context.period_start_ns=audit_now;
    auto context_thread=make_thread(pid,SCHED_BAND_FAIR); context_thread.sched_context=&context; dispatch(context_thread);
    audit_now+=2000000; sched::tick(); assert(!sched::sched_context_available(&context)); sched::post_syscall(&frame);
    assert(idle_waits && sched::current()==&context_thread && context.used_ns==0);
    std::puts("context budget independently waits for its period");

    reset(); sched::set_online_cpu_count(2); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto blocking=make_thread(pid,SCHED_BAND_FAIR); dispatch(blocking); blocking.tf.elr=0x1111;
    blocking.state=TS_SLEEPING; sched::block(&blocking); sched::wake_remote(&blocking,1);
    audit_cpu=1; sched::drain_pending_wakes();
    assert(!blocking.on_runqueue && !sched::current_eki().pending_wakes.head && blocking.deferred_wake_cpu==1);
    audit_cpu=0; frame.elr=0x2222;
    // Keep the old CPU occupied after publishing the remote wake.
    p2=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto successor=make_thread(p2,SCHED_BAND_FAIR); sched::enqueue(&successor); sched::post_syscall(&frame);
    audit_cpu=1; sched::drain_pending_wakes();
    assert(blocking.on_runqueue && blocking.cpu_id==1 && blocking.tf.elr==0x2222 && blocking.execution_cpu==kInvalidCpu);
    std::puts("remote wake publishes only after the old CPU saves its frame");

    reset(); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    auto dying=make_thread(pid,SCHED_BAND_FAIR); auto next_after_idle=make_thread(pid,SCHED_BAND_FAIR);
    retired_storage=false; verify_retirement=true; wake_on_idle=&next_after_idle;
    dying.lifetime=object::create(&dying,[](void*) { retired_storage=true; }); assert(dying.lifetime);
    dispatch(dying); dying.state=TS_DEAD; sched::request_switch(false,SWITCH_EXIT); sched::post_syscall(&frame);
    assert(retired_storage && idle_waits && sched::current()==&next_after_idle);
    verify_retirement=false;
    std::puts("idle station releases retired thread storage before waiting");

    reset(); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,1000000000,1000000000000);
    CapTable table{}; Process creator{}; creator.cap_table=&table;
    creator.principal_id=pid; creator.principal_class=PRINCIPAL_NATIVE_SANDBOXED;
    SchedContext request{}; request.band=SCHED_BAND_FAIR;
    request.budget_ns=1000000000; request.period_ns=1000000000000;
    SysError error=E_OK;
    auto invalid=request; invalid.period_ns=0;
    assert(!sched::sched_context_create(creator,invalid,&error) && error==E_INVALID);
    const auto cap=sched::sched_context_create(creator,request,&error); assert(cap && error==E_OK);
    {
        auto shared=sched::sched_context_from_cap(creator,cap,&error); assert(shared);
        sched::release_principal(pid); // the context keeps the bucket alive
        assert(sched::principal_allocated(pid));
        Thread runners[4]; std::thread workers[4];
        for(u32 cpu=0;cpu<4;++cpu) {
            runners[cpu]=make_thread(pid,SCHED_BAND_FAIR); runners[cpu].sched_context=shared.get();
            workers[cpu]=std::thread([&,cpu] {
                audit_cpu=cpu; audit_now=1; dispatch(runners[cpu]);
                for(int i=0;i<5000;++i) { audit_now+=100; sched::account_current(sched::current_eki()); }
            });
        }
        for(auto& worker:workers) worker.join();
        assert(sched::principal_runtime(pid)==2000000 && shared->used_ns==2000000);
        cap::clear(table);
    }
    assert(!sched::principal_allocated(pid));
    std::puts("four CPUs charge a retained principal and pooled context without lost updates");
    SchedContext rollover{}; rollover.budget_ns=10; rollover.period_ns=100; rollover.period_start_ns=1;
    sched::charge_sched_context(&rollover,1,201);
    assert(rollover.period_start_ns==201 && rollover.used_ns==1);
    sched::charge_sched_context(&rollover,1,190);
    assert(rollover.period_start_ns==201 && rollover.used_ns==2);
    std::puts("late accounting timestamps do not underflow period rollover");

}
