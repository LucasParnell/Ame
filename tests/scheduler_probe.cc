// Scheduler regression checks. The runner substitutes privileged hardware
// boundaries; queue, accounting, context, and ownership logic use kernel code.
#include "anko/sched.h"
#include "anko/log.h"
#include "anko/spinlock.h"
#include "sched_internal.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <thread>
#include <atomic>
thread_local unsigned long long audit_now = 1;
thread_local unsigned int audit_cpu = 0;
unsigned ipis = 0;
unsigned idle_waits = 0;
unsigned vm_activations = 0;
u64 vm_last_root = 0;
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
void vm_activate(u64 root) { ++vm_activations; vm_last_root = root; }
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
void reset() { audit_cpu=0; audit_now=1; ipis=0; idle_waits=0; vm_activations=0; sched::init(); }
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

void compact_dispatch_checks() {
    reset();
    constexpr u32 principals = 3, bands = 6;
    u32 ids[principals];
    Thread threads[principals * bands]{};
    for (u32 p = 0; p < principals; ++p) {
        ids[p] = sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED, 1024, 0, 0);
        assert(ids[p]);
        for (u32 b = 0; b < bands; ++b) {
            auto& t = threads[p * bands + b];
            t = make_thread(ids[p], static_cast<SchedBand>(b + 1));
            if (p || b) sched::enqueue(&t);
        }
    }
    dispatch(threads[0]);
    for (u32 round = 0; round < 10000; ++round) {
        switch_after(1000);
        const auto& e = sched::current_eki();
        const auto* selected = e.current;
        assert(selected && !selected->on_runqueue && selected->state == TS_RUNNING);
        assert(selected->sched.sched_class != SCHED_BAND_IDLE);
        const auto& chosen = e.principal_entities[selected->principal_id];
        u32 queued = 0;
        // Independent minimum/membership checks across all runnable records,
        // without using either principal or thread heap lookup adapters.
        for (const auto& t : threads) {
            if (!t.on_runqueue) { assert(&t == selected); continue; }
            ++queued;
            assert(t.state == TS_RUNNABLE_QUEUED);
            if (t.sched.sched_class == SCHED_BAND_IDLE) continue;
            const auto& peer = e.principal_entities[t.principal_id];
            assert(chosen.service_vruntime_ns <= peer.service_vruntime_ns);
            if (t.principal_id == selected->principal_id &&
                t.sched.sched_class == selected->sched.sched_class)
                assert(sched::effective_key(selected) <= sched::effective_key(&t));
        }
        assert(queued == principals * bands - 1 && e.published_load == queued);
    }
    sched::current_eki().current = nullptr;
    for (auto& t : threads) sched::dequeue_thread(&t);
    for (u32 id : ids) sched::release_principal(id);
    std::puts("10000 cross-band dispatches preserve principal and per-band thread minima and membership");
}

void queue_capacity_checks() {
    reset();
    const auto pid = sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED, 1024, 0, 0);
    Thread threads[kFairQueueCap + 1]{};
    for (u32 i = 0; i <= kFairQueueCap; ++i) {
        threads[i] = make_thread(pid, SCHED_BAND_FAIR);
        sched::enqueue(&threads[i]);
    }
    const auto& station = sched::current_eki();
    const auto& pe = station.principal_entities[pid];
    assert(pe.runnable[SCHED_BAND_FAIR] == kFairQueueCap);
    assert(station.published_load == kFairQueueCap && station.fair_queue_overflows == 1);
    assert(!threads[kFairQueueCap].on_runqueue && threads[kFairQueueCap].state == TS_BLOCKED);

    // A running thread plus 64 queued peers overflows when preemption requeues
    // it. Parking exposes a blocked state: a wake observed in that window must
    // still be forwarded after the exception frame is saved.
    sched::set_online_cpu_count(2);
    auto& overflow=threads[kFairQueueCap];
    dispatch(overflow);
    std::atomic<bool> holding{false};
    std::thread waker([&] {
        kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&overflow.lifecycle_lock));
        holding.store(true,std::memory_order_release);
        while (overflow.state!=TS_BLOCKED) {}
        assert(overflow.tf.elr==0x12345678);
        overflow.deferred_wake_cpu=1;
    });
    while (!holding.load(std::memory_order_acquire)) {}
    TrapFrame saved{}; saved.elr=0x12345678;
    audit_now+=1000; sched::request_switch(true,SWITCH_TIMER); sched::post_syscall(&saved);
    waker.join();
    assert(overflow.state==TS_RUNNABLE_PENDING_REMOTE && overflow.execution_cpu==kInvalidCpu);
    assert(sched::eki_for_cpu(1).pending_wakes.head==&overflow && overflow.tf.elr==0x12345678);
    audit_cpu=1; sched::post_syscall(&saved); sched::dequeue_thread(&overflow); audit_cpu=0;
    sched::current_eki().current=nullptr;
    for (u32 i = 0; i < kFairQueueCap; ++i) sched::dequeue_thread(&threads[i]);
    assert(!pe.runnable[SCHED_BAND_FAIR] && !station.published_load);
    sched::release_principal(pid);
    std::puts("bounded runnable counters preserve all 64 queue slots and overflow rollback");
}

void return_frame_checks() {
    reset();
    const u32 pid = sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED, 1024, 0, 0);
    auto t = make_thread(pid, SCHED_BAND_FAIR);
    Process owner{}; owner.ttbr0 = 0x12345000; t.owner = &owner;
    dispatch(t);
    TrapFrame frame{};
    for (u32 i = 0; i < 31; ++i) frame.x[i] = 0x1000 + i;
    frame.elr = 0x2000; frame.spsr = 0x40; frame.sp_el0 = 0x3000;
    fpsimd::live = {};
    fpsimd::live.v[31][1] = 0x123456789abcdef0;
    fpsimd::live.fpcr = 1u << 22;
    fpsimd::live.fpsr = 0x1f;
    fpsimd::saves = fpsimd::restores = 0;
    sched::post_syscall(&frame);
    assert(sched::current() == &t && !t.tf.elr);
    assert(!fpsimd::saves && !fpsimd::restores); // no extra traffic on ordinary syscalls
    assert(vm_activations == 1 && vm_last_root == owner.ttbr0);
    sched::yield(); sched::post_syscall(&frame);
    assert(sched::current() == &t && !std::memcmp(&frame, &t.tf, sizeof(frame)));
    assert(fpsimd::saves == 1 && fpsimd::restores == 1);
    assert(!std::memcmp(&fpsimd::live, &t.fpsimd, sizeof(t.fpsimd)));
    sched::current_eki().current = nullptr;
    sched::release_principal(pid);
    std::puts("same-thread return restores address space; switch saves every frame register");

    bool destroyed = false;
    auto* record = object::create(&destroyed, [](void* p) { *static_cast<bool*>(p) = true; });
    {
        object::Ref<bool> retained(&destroyed, record);
        object::release(record);
        assert(retained && !destroyed);
    }
    assert(destroyed);
    std::puts("cached lifetime identity retains the object through creator release");
}

void remote_batch_checks() {
    reset();
    sched::set_online_cpu_count(4);
    const auto pid = sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED, NICE_0_WEIGHT, 0, 0);
    constexpr unsigned producers = 3, per_producer = 8, count = producers * per_producer;
    Thread threads[count]{};
    for (auto& t : threads) {
        t = make_thread(pid, SCHED_BAND_FAIR);
        t.affinity_mask = 2;
        t.cpu_id = t.home_cpu = 1;
    }
    std::atomic<bool> go{false};
    std::thread writers[producers];
    for (unsigned source = 0; source < producers; ++source) {
        writers[source] = std::thread([&, source] {
            audit_cpu = source == 0 ? 0 : source + 1;
            while (!go.load(std::memory_order_acquire)) {}
            for (unsigned i = 0; i < per_producer; ++i)
                sched::wake_remote(&threads[source * per_producer + i], 1);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& writer : writers) writer.join();
    assert(ipis == 1); // one empty transition, not one SGI per thread
    audit_cpu = 1;
    sched::drain_pending_wakes();
    assert(sched::current_eki().published_load == count);
    for (auto& t : threads) {
        assert(t.on_runqueue && t.state == TS_RUNNABLE_QUEUED && t.cpu_id == 1);
        sched::dequeue_thread(&t);
    }
    assert(!sched::current_eki().pending_wakes.head);
    sched::drain_pending_wakes(); // empty fast path must not change membership
    audit_cpu = 0;
    threads[0].state = TS_SLEEPING;
    sched::wake_remote(&threads[0], 1);
    assert(ipis == 2); // draining reopened the signal transition
    audit_cpu = 1;
    sched::drain_pending_wakes();
    assert(threads[0].on_runqueue && sched::current_eki().published_load == 1);
    sched::dequeue_thread(&threads[0]);
    sched::release_principal(pid);
    std::puts("concurrent remote wake batches retain every thread and coalesce IPIs until drain");
}

static void handoff_fairness_checks() {
    for (bool realtime : {false, true}) {
        reset();
        const auto pid = sched::allocate_principal(PRINCIPAL_NATIVE_TRUSTED, 1024, 0, 0);
        auto first = make_thread(pid, SCHED_BAND_FAIR);
        auto second = make_thread(pid, SCHED_BAND_FAIR);
        auto competitor = make_thread(pid, realtime ? SCHED_BAND_RT_NATIVE : SCHED_BAND_FAIR);
        dispatch(first);
        sched::enqueue(&competitor);
        bool ran = false;
        for (unsigned i = 0; i < 9; ++i) {
            auto* current = sched::current();
            auto* next = current == &first ? &second : &first;
            current->state = TS_BLOCKED;
            next->state = TS_SLEEPING;
            sched::set_handoff_next(next);
            sched::request_switch(false, SWITCH_HANDOFF);
            ++audit_now;
            TrapFrame frame{};
            sched::post_syscall(&frame);
            if (sched::current() == &competitor) { ran = true; break; }
            assert(!realtime && "handoff bypassed queued realtime work");
            assert(sched::current() == next);
        }
        assert(ran && "unbounded handoff chain starved queued work");
        sched::current_eki().current = nullptr;
        sched::dequeue_thread(&first); sched::dequeue_thread(&second); sched::dequeue_thread(&competitor);
        sched::release_principal(pid);
    }
    std::puts("handoff bursts yield to fair competitors and never bypass queued realtime work");
}

int main() {
    handoff_fairness_checks();
    heap_checks();
    compact_dispatch_checks();
    queue_capacity_checks();
    return_frame_checks();
    remote_batch_checks();
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
    auto retired=make_thread(pid,SCHED_BAND_FAIR); sched::enqueue(&retired); sched::dequeue_thread(&retired);
    dispatch(retired); audit_now += 7000000; sched::account_current(sched::current_eki());
    sched::current_eki().current = nullptr; retired.execution_cpu = kInvalidCpu;
    sched::release_principal(pid);
    auto recycled=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0); assert(recycled==pid);
    auto replacement=make_thread(recycled,SCHED_BAND_FAIR); sched::enqueue(&replacement);
    auto& queue=sched::current_eki().fair_principals;
    assert(queue.size==1 && queue.data[0]->principal_id==pid);
    const auto& reused = sched::current_eki().principal_entities[pid];
    assert(!reused.service_vruntime_ns && !reused.thread_min_vruntime_ns);
    sched::dequeue_thread(&replacement); dispatch(replacement); switch_after(1000);
    assert(reused.service_vruntime_ns == 1000 && reused.thread_min_vruntime_ns == 1000);
    assert(sched::principal_runtime(pid) == 1000);
    std::puts("principal reuse resets both fairness clocks and authoritative runtime");

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
    SchedContext context{}; context.budget_ns=1000000; context.period_ns=100000000; context.budget_window.reset(audit_now);
    auto context_thread=make_thread(pid,SCHED_BAND_FAIR); context_thread.sched_context=&context; dispatch(context_thread);
    audit_now+=2000000; sched::tick(); assert(!sched::sched_context_available(&context)); sched::post_syscall(&frame);
    assert(idle_waits && sched::current()==&context_thread && context.budget_window.snapshot().used_ns==0);
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
        assert(sched::principal_runtime(pid)==2000000 && shared->budget_window.snapshot().used_ns==2000000);
        cap::clear(table);
    }
    assert(!sched::principal_allocated(pid));
    std::puts("four CPUs charge a retained principal and pooled context without lost updates");
    SchedContext rollover{}; rollover.budget_ns=10; rollover.period_ns=100; rollover.budget_window.reset(1);
    sched::charge_sched_context(&rollover,1,201);
    assert(rollover.budget_window.snapshot().period_start_ns==201 && rollover.budget_window.snapshot().used_ns==1);
    sched::charge_sched_context(&rollover,1,190);
    assert(rollover.budget_window.snapshot().period_start_ns==201 && rollover.budget_window.snapshot().used_ns==2);
    std::puts("late accounting timestamps do not underflow period rollover");

    // Preemption keeps both threads under station ownership. A remote lifecycle
    // reader can hold either guard without preventing this CPU from dispatching.
    reset(); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,1000000000,1000000000000);
    auto first=make_thread(pid,SCHED_BAND_FAIR), second=make_thread(pid,SCHED_BAND_FAIR);
    first.lifetime=object::create(&first,nullptr); second.lifetime=object::create(&second,nullptr);
    SchedContext local_context{}; local_context.budget_ns=1000000000; local_context.period_ns=1000000000000;
    local_context.budget_window.reset(audit_now);
    first.sched_context=second.sched_context=&local_context;
    sched::enqueue(&second); dispatch(first);
    first.lifecycle_lock=second.lifecycle_lock=1;
    for (u32 i=0;i<1000;++i) switch_after(1000);
    first.lifecycle_lock=second.lifecycle_lock=0;
    assert(first.switches && second.switches);
    assert(sched::principal_runtime(pid)==1000000);
    assert(local_context.budget_window.snapshot().used_ns==1000000);
    sched::current_eki().current=nullptr;
    sched::dequeue_thread(&first); sched::dequeue_thread(&second);
    object::release(first.lifetime); object::release(second.lifetime); sched::release_principal(pid);
    std::puts("budgeted preemption progresses with both lifecycle guards held by remote readers");

    // All CPUs race the same rollover, then charge identical timestamps. A
    // separate reset/store would lose some updates; a paired CAS cannot do so.
    sched::BudgetWindow window{};
    constexpr u64 origin=(1ull<<60)+123, period=1000000000000ull;
    window.reset(origin);
    std::atomic<bool> start{false};
    std::thread chargers[8];
    for (u32 cpu=0;cpu<8;++cpu) chargers[cpu]=std::thread([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (u32 i=0;i<10000;++i) window.charge(17,origin+period*3+7,period);
    });
    start.store(true,std::memory_order_release);
    for (auto& charger:chargers) charger.join();
    auto charged=window.snapshot();
    assert(charged.used_ns==8*10000*17 && charged.period_start_ns==origin+period*3);
    window.charge(1,origin,period); // delayed timestamp cannot reopen an old period
    charged=window.snapshot();
    assert(charged.used_ns==8*10000*17+1 && charged.period_start_ns==origin+period*3);
    window.charge(~0ull,origin,period);
    assert(window.charge(1,origin,period).used_ns==~0ull);
    std::puts("eight CPUs race a full-width budget rollover without lost charges or wraparound");

    reset(); pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    const auto busy_before = sched::busy_runtime_ns();
    Thread unbudgeted[kMaxCpus]; std::thread owners[kMaxCpus];
    for (u32 cpu=0;cpu<kMaxCpus;++cpu) {
        unbudgeted[cpu]=make_thread(pid,SCHED_BAND_FAIR);
        owners[cpu]=std::thread([&,cpu] {
            audit_cpu=cpu; audit_now=1; dispatch(unbudgeted[cpu]);
            for (u32 i=0;i<10000;++i) { audit_now+=100; sched::account_current(sched::current_eki()); }
            sched::current_eki().current=nullptr;
        });
    }
    for (auto& owner:owners) owner.join();
    assert(sched::principal_runtime(pid)==kMaxCpus*1000000ull);
    const auto busy_after = sched::busy_runtime_ns();
    assert(busy_after - busy_before == kMaxCpus*1000000ull);
    sched::release_principal(pid);
    const auto fresh=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    assert(fresh==pid && sched::principal_runtime(fresh)==0);
    assert(sched::busy_runtime_ns() == busy_after); // exited work remains in CPU usage
    sched::release_principal(fresh);
    std::puts("CPU-owned runtime columns sum exactly and all reset on principal reuse");

    reset(); sched::set_online_cpu_count(2);
    pid=sched::allocate_principal(PRINCIPAL_NATIVE_SANDBOXED,1024,0,0);
    first=make_thread(pid,SCHED_BAND_FAIR); second=make_thread(pid,SCHED_BAND_FAIR);
    first.lifetime=object::create(&first,nullptr); second.lifetime=object::create(&second,nullptr);
    sched::enqueue(&second); dispatch(first);
    CapTable controls{}; Process controller{}; controller.cap_table=&controls;
    controller.principal_id=pid; controller.principal_class=PRINCIPAL_NATIVE_SANDBOXED;
    const auto first_cap=cap::alloc(controls,CAP_THREAD,&first,RIGHT_THREAD_CONTROL);
    const auto context_cap=sched::sched_context_create(controller,SchedContext{.band=SCHED_BAND_FAIR},&error);
    assert(first_cap && context_cap);
    std::atomic<bool> ready{false}, stop{false};
    std::thread remote([&] {
        audit_cpu=1;
        ready.store(true,std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            sched::apply_wake_metadata(&first,WAKE_DOORBELL,IPC_HINT_NONE);
            assert(sched::target_cpu_for_wake(&first,nullptr,IPC_HINT_NONE,kInvalidCpu)==0);
            sched::wake_remote(&first,1);
            assert(sched::sched_context_bind_thread(controller,first_cap,context_cap)==E_BUSY);
        }
    });
    while (!ready.load(std::memory_order_acquire)) {}
    for (u32 i=0;i<20000;++i) switch_after(1000);
    stop.store(true,std::memory_order_release); remote.join();
    assert(!first.wake_credit_ns && !first.sched_context && !first.remote_next);
    assert(sched::principal_runtime(pid)==20000000);
    sched::current_eki().current=nullptr;
    sched::dequeue_thread(&first); sched::dequeue_thread(&second);
    cap::clear(controls);
    object::release(first.lifetime); object::release(second.lifetime); sched::release_principal(pid);
    std::puts("remote wake/metadata/context-binding attempts cannot mutate station-owned dispatch");
    return 0;
}
