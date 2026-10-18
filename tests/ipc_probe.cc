// Use real IPC, capabilities, lifetimes, and scheduler queues. The fixture
// supplies allocation and hardware boundaries, never IPC ownership decisions.
#include "anko/ipc.h"
#include "anko/sched.h"
#include "anko/log.h"
#include "anko/spinlock.h"
#include "sched_internal.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

thread_local unsigned long long audit_now=1;
thread_local unsigned int audit_cpu=0;
static Process* process_directory[3]{};
static kernel::SpinLock directory_lock{};
static std::atomic<unsigned> ipis{0};
static int slab_countdown=-1;
static usize page_count=0;
struct Pages { PhysAddr base; usize count, remaining; };
static std::vector<Pages> pages;

void audit_wait() { std::abort(); } // every switching test supplies runnable work
namespace mm {
void* slab_alloc(usize size, usize alignment) {
    if (slab_countdown==0) return nullptr;
    if (slab_countdown>0) --slab_countdown;
    void* p=nullptr;
    return posix_memalign(&p,alignment<sizeof(void*)?sizeof(void*):alignment,size)?nullptr:p;
}
void slab_free(void* p, usize, usize) { std::free(p); }
PhysAddr alloc_pages_contiguous_for(u32, usize count) {
    void* p=nullptr;
    if (posix_memalign(&p,PAGE_SIZE,count*PAGE_SIZE)) return 0;
    const auto address=reinterpret_cast<PhysAddr>(p);
    pages.push_back({address,count,count}); page_count+=count;
    return address;
}
void free_page_for_identity(u64, PhysAddr address) {
    for (usize i=0;i<pages.size();++i) {
        auto& allocation=pages[i];
        if (address<allocation.base || address>=allocation.base+allocation.count*PAGE_SIZE) continue;
        assert(allocation.remaining && address%PAGE_SIZE==0);
        --page_count;
        if (!--allocation.remaining) {
            std::free(reinterpret_cast<void*>(allocation.base));
            pages[i]=pages.back(); pages.pop_back();
        }
        return;
    }
    std::abort();
}
void free_page_for(u32 pid, PhysAddr address) { free_page_for_identity(pid,address); }
void free_page(PhysAddr) { std::abort(); }
void vm_activate(PhysAddr) {}
}
namespace libk { void* memset(void* p,int value,usize count) { return std::memset(p,value,count); } }
namespace anko::log {
void write(Level,const char*,const char*) {}
void writef(Level,const char*,const char*,...) {}
}
[[noreturn]] void panic(const char* text) { std::fprintf(stderr,"%s\n",text); std::abort(); }
namespace smp { void send_wake_ipi(u32) { ++ipis; } }
namespace sched {
extern "C" [[noreturn]] void enter_user_frame(const TrapFrame*) { std::abort(); }
object::Ref<Process> find_process(u32 pid) {
    kernel::LockGuard guard(directory_lock);
    return object::Ref<Process>(pid<3?process_directory[pid]:nullptr);
}
object::Ref<Thread> find_thread(u32) { return nullptr; }
bool termination_pending() { return false; }
void finish_thread(Thread*) { std::abort(); }
void thread_destroy(Thread*) { std::abort(); }
void process_destroy(Process*) { std::abort(); }
}

static void dispatch(Thread& t) {
    auto& station=sched::current_eki(); station.current=&t;
    t.state=TS_RUNNING; t.execution_cpu=audit_cpu;
    t.last_start_ns=t.slice_start_ns=audit_now;
}

struct Fixture {
    Process processes[2]{};
    CapTable tables[2]{};
    Thread threads[72]{};
    u32 count=0;
    Fixture() {
        audit_cpu=0; audit_now=1; ipis=0; sched::init(); sched::set_online_cpu_count(kMaxCpus);
        for (u32 i=0;i<2;++i) {
            auto& p=processes[i]; p.pid=i+1; p.generation=1; p.cap_table=&tables[i];
            p.lifetime=object::create(&p,nullptr); assert(p.lifetime);
            process_directory[p.pid]=&p;
        }
    }
    Thread& thread(u32 cpu=0,u32 process=0) {
        assert(count<72);
        auto& t=threads[count++]; t.tid=count; t.owner=&processes[process]; t.pid=t.owner->pid;
        t.cpu_id=cpu; t.home_cpu=cpu; t.last_cpu=kInvalidCpu; t.affinity_mask=1ull<<cpu;
        t.state=TS_SUSPENDED; t.execution_cpu=t.deferred_wake_cpu=kInvalidCpu;
        t.sched.sched_class=SCHED_BAND_FAIR; t.sched.weight=NICE_0_WEIGHT;
        t.owner_lifetime=t.owner->lifetime; object::retain(t.owner_lifetime);
        t.lifetime=object::create(&t,[](void* target) {
            object::release(static_cast<Thread*>(target)->owner_lifetime);
        });
        assert(t.lifetime); return t;
    }
    void park(Thread& t) {
        // Model the completion of the frame-save boundary without dispatching
        // another user frame. The dedicated wake test exercises post_syscall.
        assert(t.state==TS_SLEEPING);
        t.execution_cpu=kInvalidCpu; sched::current_eki().current=nullptr;
        sched::current_eki().switch_pending=false;
    }
    ~Fixture() {
        slab_countdown=-1;
        for (u32 cpu=0;cpu<kMaxCpus;++cpu) {
            audit_cpu=cpu; auto& station=sched::current_eki(); station.current=nullptr;
            TrapFrame frame{}; sched::post_syscall(&frame); // detach any remote publications
            for (u32 i=0;i<count;++i) {
                auto& t=threads[i];
                if (t.cpu_id==cpu) sched::dequeue_thread(&t);
            }
        }
        for (u32 i=0;i<count;++i) { threads[i].state=TS_DEAD; ipc::doorbell_clear_waiter(&threads[i]); }
        for (u32 cpu=0;cpu<kMaxCpus;++cpu) { audit_cpu=cpu; ipc::poll_deadline_waiters(); }
        for (auto& table:tables) cap::clear(table);
        for (u32 i=0;i<count;++i) object::release(threads[i].lifetime);
        for (auto& p:processes) { process_directory[p.pid]=nullptr; object::release(p.lifetime); }
        assert(!object::live_count() && !page_count);
        audit_cpu=0;
    }
};

static void rights_and_allocation() {
    Fixture f; auto& runner=f.thread(); dispatch(runner);
    const auto db=ipc::doorbell_create(1); assert(db);
    const auto notify=ipc::doorbell_share(1,db,2,RIGHT_NOTIFY);
    const auto wait=ipc::doorbell_share(1,db,2,RIGHT_WAIT);
    assert(notify && wait && !ipc::doorbell_share(2,notify,1,RIGHT_WAIT));
    assert(ipc::notify(wait,2)==E_NOTFOUND);
    assert(ipc::wait_doorbell(notify,0,2)==E_NOTFOUND);
    assert(ipc::notify(notify,2)==E_OK && ipc::doorbell_counter(wait,2)==1);
    cap::revoke(f.tables[1],notify);
    assert(ipc::notify(notify,2)==E_NOTFOUND);
    const auto ring=ipc::ring_create(1,4,0); assert(ring && page_count);
    const auto read=ipc::ring_share(1,ring,2,RIGHT_READ|RIGHT_DUP);
    assert(read && !ipc::ring_share(2,read,1,RIGHT_WRITE));
    auto borrowed=ipc::ring_from_cap(2,read); assert(borrowed && borrowed->order==4);
    cap::revoke(f.tables[0],ring); cap::revoke(f.tables[1],read);
    assert(page_count); // in-flight borrowed authority owns the backing page
    assert(!ipc::ring_create(1,3,0) && !ipc::ring_create(1,11,0));
    std::puts("IPC rights attenuation, stale handles, and borrowed ring storage pass");
}

static void allocation_failures() {
    Fixture f; auto& runner=f.thread(); dispatch(runner);
    for (int stage=0;stage<2;++stage) {
        const auto objects=object::live_count();
        slab_countdown=stage; assert(!ipc::ring_create(1,4,0));
        slab_countdown=-1; assert(!page_count && object::live_count()==objects);
        slab_countdown=stage; assert(!ipc::doorbell_create(1));
        slab_countdown=-1; assert(object::live_count()==objects);
    }
    for (u32 i=0;i<kCapTableSize;++i) assert(cap::alloc(f.tables[0],CAP_ARENA,&runner,RIGHT_READ));
    const auto objects=object::live_count();
    assert(!ipc::doorbell_create(1) && !ipc::ring_create(1,4,0));
    assert(object::live_count()==objects && !page_count);
    std::puts("IPC object/record/capacity failures reclaim every root and page");
}

static void counter_wrap() {
    Fixture f;
    auto& waiter = f.thread();
    dispatch(waiter);
    const auto handle = ipc::doorbell_create(1);
    assert(handle);
    auto entry = cap::lookup(f.tables[0], handle, CAP_DOORBELL);
    auto* db = static_cast<Doorbell*>(entry->target);
    __atomic_store_n(&db->counter, ~u64{0}, __ATOMIC_RELEASE);
    assert(ipc::wait_doorbell(handle, ~u64{0}, 1) == E_OK);
    f.park(waiter);
    assert(ipc::notify_object(db) == E_OK);
    assert(ipc::doorbell_counter(handle, 1) == 0);
    assert(waiter.on_runqueue && !waiter.waiting_doorbell);
    std::puts("locked counter publication preserves wraparound and waiter wakeup");
}

static void frame_and_timeout() {
    Fixture f; auto& waiter=f.thread(); auto& fallback=f.thread();
    const auto handle=ipc::doorbell_create(1);
    auto authority=cap::lookup(f.tables[0],handle,CAP_DOORBELL);
    auto* db=static_cast<Doorbell*>(authority->target); db->home_cpu=1;
    waiter.affinity_mask=3;
    dispatch(waiter); sched::enqueue(&fallback);
    assert(ipc::wait_doorbell(handle,0,1)==E_OK && db->waiter==&waiter);
    audit_cpu=1; assert(ipc::notify_object(db)==E_OK);
    assert(!waiter.on_runqueue && !sched::current_eki().pending_wakes.head && waiter.deferred_wake_cpu==1);
    audit_cpu=0; TrapFrame frame{}; frame.elr=0xabcdef01;
    sched::post_syscall(&frame);
    assert(sched::current()==&fallback && waiter.tf.elr==0xabcdef01);
    audit_cpu=1; sched::post_syscall(&frame);
    assert(waiter.on_runqueue && waiter.tf.elr==0xabcdef01);
    sched::dequeue_thread(&waiter); waiter.affinity_mask=1; waiter.cpu_id=0; db->home_cpu=0;
    audit_cpu=0; sched::current_eki().current=nullptr; fallback.execution_cpu=kInvalidCpu;
    dispatch(waiter);
    assert(ipc::wait_doorbell_until(handle,1,100,1)==E_OK); f.park(waiter);
    audit_now=99; ipc::poll_deadline_waiters(); assert(db->waiter==&waiter);
    audit_now=100; ipc::poll_deadline_waiters(); assert(!db->waiter && waiter.on_runqueue);
    sched::dequeue_thread(&waiter); dispatch(waiter);
    assert(ipc::wait_doorbell_until(handle,1,300,1)==E_OK); f.park(waiter);
    assert(ipc::notify_object(db)==E_OK); sched::dequeue_thread(&waiter); dispatch(waiter);
    assert(ipc::wait_doorbell_until(handle,2,500,1)==E_OK); f.park(waiter);
    audit_now=300; ipc::poll_deadline_waiters(); assert(db->waiter==&waiter);
    audit_now=500; ipc::poll_deadline_waiters(); assert(!db->waiter && waiter.on_runqueue);
    std::puts("remote wake saves the full frame; stale timer sequences cannot cancel a new wait");
}

static void deadline_capacity() {
    Fixture f; CapHandle handles[65]{};
    for (u32 i=0;i<65;++i) {
        auto& t=f.thread(); dispatch(t); handles[i]=ipc::doorbell_create(1); assert(handles[i]);
        const auto rc=ipc::wait_doorbell_until(handles[i],0,1000000,1);
        if (i<64) { assert(rc==E_OK); f.park(t); }
        else assert(rc==E_OVERFLOW && t.state==TS_RUNNING);
    }
    // A notification satisfies the wait even when every timer slot is occupied.
    assert(ipc::notify(handles[64],1)==E_OK);
    assert(ipc::wait_doorbell_until(handles[64],0,1000000,1)==E_OK);
    ipc::doorbell_clear_waiter(&f.threads[0]); ipc::poll_deadline_waiters();
    assert(ipc::wait_doorbell_until(handles[64],1,1000000,1)==E_OK); f.park(f.threads[64]);
    std::puts("64 timer slots enforce capacity and reclaim cancellation before admission");
}

static void handoff_and_reply() {
    Fixture f; auto& receiver=f.thread(); auto& sender=f.thread();
    dispatch(receiver); const auto handle=ipc::doorbell_create(1);
    auto cap=cap::lookup(f.tables[0],handle,CAP_DOORBELL);
    auto* db=static_cast<Doorbell*>(cap->target); db->sched_hints=IPC_HINT_HANDOFF_OK;
    sched::enqueue(&sender);
    assert(ipc::wait_doorbell(handle,0,1)==E_OK);
    TrapFrame frame{}; frame.elr=0x1111; sched::post_syscall(&frame);
    assert(sched::current()==&sender);
    assert(ipc::notify_handoff(handle,0xface,1)==E_OK);
    assert(sender.state==TS_BLOCKED && sender.reply_target==&receiver);
    frame.elr=0x2222; sched::post_syscall(&frame);
    assert(sched::current()==&receiver && frame.x[1]==0xface);
    assert(ipc::reply_handoff(0xbeef,1)==E_OK);
    assert(sender.tf.x[0]==0xbeef && !sender.reply_target && !receiver.reply_target);
    assert(db->handoff_count==1 && db->wake_count==1);
    assert(ipc::reply_handoff(0,1)==E_INVALID);
    std::puts("same-CPU request/reply handoff preserves payload, return value, and ownership");
}

#ifndef AME_IPC_LEGACY_BENCH_FIXTURE
static void ring_waits_and_exchange() {
    Fixture f;
    auto& receiver = f.thread(); auto& sender = f.thread();
    dispatch(receiver);
    const auto request = ipc::ring_create(1, 7, MX_RING_PACKED_FRAMES);
    const auto reply = ipc::ring_create(1, 7, MX_RING_PACKED_FRAMES);
    const auto request_db = ipc::doorbell_create(1, IPC_HINT_HANDOFF_OK | IPC_HINT_SERVER);
    const auto reply_db = ipc::doorbell_create(1);
    const auto space_db = ipc::doorbell_create(1);
    auto req = ipc::ring_from_cap(1, request), rep = ipc::ring_from_cap(1, reply);
    auto* request_page = reinterpret_cast<RingHeader*>(req->phys_base);
    auto* reply_page = reinterpret_cast<RingHeader*>(rep->phys_base);
    assert(request_page->space.features == MX_RING_PACKED_FRAMES);
    assert(ipc::wait_ring(request, request_db, 0, 2, 1) == E_INVALID);
    // Publication that wins before waiter registration needs no notification.
    request_page->head.head = 1;
    assert(ipc::wait_ring(request, request_db, 0, MX_RING_WAIT_READABLE, 1) == E_OK);
    assert(receiver.state == TS_RUNNING);
    request_page->tail.tail = 1;
    sched::enqueue(&sender);
    assert(ipc::wait_ring(request, request_db, 1, MX_RING_WAIT_READABLE, 1) == E_OK);
    TrapFrame frame{};
    sched::post_syscall(&frame);
    assert(sched::current() == &sender);
    request_page->head.head = 2;
    // Invalid authority must leave the real notify counter and waiter untouched.
    assert(ipc::exchange_ring(request_db, reply, kNullCap, 0, 0, 1) == E_NOTFOUND);
    assert(ipc::doorbell_counter(request_db, 1) == 0);
    assert(ipc::exchange_ring(request_db, reply, reply_db, 0, MX_IPC_EXCHANGE_HANDOFF, 1) == E_OK);
    assert(sender.state == TS_BLOCKED && sender.reply_target == &receiver);
    sched::post_syscall(&frame);
    assert(sched::current() == &receiver);
    request_page->tail.tail = 2;
    reply_page->head.head = 1;
    assert(ipc::exchange_ring(reply_db, request, request_db, 2, MX_IPC_EXCHANGE_REPLY, 1) == E_OK);
    assert(receiver.state == TS_SLEEPING && !receiver.on_runqueue);
    sched::post_syscall(&frame);
    assert(sched::current() == &sender && !sender.reply_target && !receiver.reply_target);
    // Producer credit waits compare the tail, not head or a doorbell snapshot.
    reply_page->head.head = 128;
    assert(ipc::wait_ring(reply, space_db, 0, MX_RING_WAIT_WRITABLE, 1) == E_OK);
    f.park(sender);
    reply_page->tail.tail = 1;
    assert(ipc::notify(space_db, 1) == E_OK && sender.on_runqueue);
    std::puts("ring cursor waits, producer credits, atomic exchange/reply, and authority failures pass");
}
#endif

static void concurrent_notifications() {
    Fixture f; auto& runner=f.thread(); dispatch(runner);
    Doorbell* independent[kMaxCpus]{};
    for (auto& db:independent) { db=ipc::doorbell_create_object(1); assert(db); }
    const auto before=ipc::notify_calls(), empty=ipc::notify_no_waiter();
    std::thread cpus[kMaxCpus];
    for (u32 cpu=0;cpu<kMaxCpus;++cpu) cpus[cpu]=std::thread([&,cpu] {
        audit_cpu=cpu;
        for (u32 i=0;i<10000;++i) assert(ipc::notify_object(independent[cpu])==E_OK);
    });
    for (auto& cpu:cpus) cpu.join();
    assert(ipc::notify_calls()-before==kMaxCpus*10000ull && ipc::notify_no_waiter()-empty==kMaxCpus*10000ull);
    for (auto* db:independent) {
        assert(db->counter==10000);
        auto* root=object::acquire(db); object::release(root); object::release(root);
    }
    std::puts("independent CPU notification streams preserve exact aggregated metrics");
}

static void registration_race() {
    Fixture f; auto& waiter=f.thread(); auto& fallback=f.thread(); auto& sender=f.thread(1,1);
    dispatch(waiter); const auto receive=ipc::doorbell_create(1);
    const auto send=ipc::doorbell_share(1,receive,2,RIGHT_NOTIFY); assert(send);
    std::atomic<u32> request{0}, done{0};
    constexpr u32 rounds=2000;
    std::thread producer([&] {
        audit_cpu=1; dispatch(sender);
        for (u32 i=1;i<=rounds;++i) {
            while (request.load(std::memory_order_acquire)<i) {}
            assert(ipc::notify(send,2)==E_OK);
            done.store(i,std::memory_order_release);
        }
        sched::current_eki().current=nullptr;
    });
    for (u32 i=1;i<=rounds;++i) {
        dispatch(waiter); fallback.state=TS_SUSPENDED; sched::enqueue(&fallback);
        request.store(i,std::memory_order_release);
        assert(ipc::wait_doorbell(receive,i-1,1)==E_OK);
        TrapFrame frame{}; frame.elr=i; sched::post_syscall(&frame);
        while (done.load(std::memory_order_acquire)<i) {}
        sched::post_syscall(&frame);
        auto& station=sched::current_eki(); station.current=nullptr;
        assert(waiter.state==TS_RUNNING || (waiter.state==TS_RUNNABLE_QUEUED && waiter.on_runqueue));
        sched::dequeue_thread(&waiter); sched::dequeue_thread(&fallback);
        waiter.execution_cpu=fallback.execution_cpu=kInvalidCpu;
        assert(!waiter.waiting_doorbell && waiter.deferred_wake_cpu==kInvalidCpu);
        station.switch_pending=false;
    }
    producer.join();
    assert(ipc::doorbell_counter(receive,1)==rounds);
    std::puts("2000 cross-CPU notify/registration/frame-publication races lose no wake");
}

int main() {
    static_assert(sizeof(Doorbell)==64);
    rights_and_allocation(); allocation_failures(); frame_and_timeout();
    counter_wrap();
    deadline_capacity(); handoff_and_reply();
#ifndef AME_IPC_LEGACY_BENCH_FIXTURE
    ring_waits_and_exchange();
#endif
    concurrent_notifications(); registration_race();
    return 0;
}
