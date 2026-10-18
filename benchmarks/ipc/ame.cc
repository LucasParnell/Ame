// Isolated benchmark init spawns two ordinary processes, wires capabilities,
// then parks. Only those peers run the portable workload; no normal services.
#include "workload.h"
#include "mochi/boot.h"
#include "mochi/state.h"
#include "mochi/wait.h"
#include "mochi/framed_ring.h"
#include "slotwise_reference.h"
#include "libk/cpu.h"
#include "mochi/ipc_budget.h"
#include "midix/format/bundle.h"
#include "midix/abi/image.h"
#include "midix/abi/debug_counters.h"

#ifndef AME_IPC_BENCH_ITERATIONS
#define AME_IPC_BENCH_ITERATIONS 1024
#define AME_IPC_BENCH_WARMUP 256
#define AME_IPC_BENCH_SAMPLES 4096
#define AME_IPC_BENCH_ROUNDS 5
#define AME_IPC_BENCH_WINDOW 32
#define AME_IPC_BENCH_PAYLOAD_MASK 31
#define AME_IPC_BENCH_SPIN 0
#endif

#ifndef AME_IPC_BENCH_SERVER_CPU
#define AME_IPC_BENCH_SERVER_CPU 0
#endif

// Bounded userspace poll before a wait arms its sleep flag. Cross-core only:
// a same-core peer cannot make progress while this thread spins.
#ifndef AME_IPC_BENCH_POLL_BUDGET
#define AME_IPC_BENCH_POLL_BUDGET 64
#endif

// 1 = let BudgetController choose the budget from observed parks; the compile
// time value then only seeds it.
#ifndef AME_IPC_BENCH_ADAPTIVE
#define AME_IPC_BENCH_ADAPTIVE 0
#endif

namespace {
using namespace ipc_bench;
using u8 = std::uint8_t;
constexpr u32 kOrder = AME_IPC_BENCH_REFERENCE_FRAMING ? MX_DEFAULT_RING_ORDER : MX_DEFAULT_FRAMED_RING_ORDER;
[[maybe_unused]] constexpr u32 kPollBudget = AME_IPC_BENCH_SERVER_CPU ? AME_IPC_BENCH_POLL_BUDGET : 0;

[[noreturn]] void failure(const char* why) {
    constexpr char prefix[] = "IPC_BENCH_ERROR: ";
    sys_debug_puts(prefix, sizeof(prefix) - 1);
    sys_debug_puts(why, libk::str::length(why));
    sys_debug_puts("\n", 1);
    mochi::park();
}
void check(bool value, const char* why) { if (!value) failure(why); }
CapHandle cap(int64_t value) { check(value > 0, "capability allocation/grant"); return static_cast<CapHandle>(value); }

struct Ame {
    mochi::Ring outgoing, incoming;
    CapHandle self_thread;
    u32 cpus;
    u64 identity, peer;
    bool spin = false;
    mochi::BudgetController budget{};
    bool pending_notify = false, server_role = false;
    const char* transport() const { return "ame_mochi_ring_framed"; }
    const char* diagnostic_scope() const { return "isolated_system"; }
    u32 client_cpu() const { return 0; }
    u32 server_cpu(bool different) const { return different && cpus > 1 ? 1 : 0; }
    const char* placement_scope() const { return AME_IPC_BENCH_SERVER_CPU ? "different_cpu" : "same_cpu"; }
    bool supports_placement(bool different) const { return different == (AME_IPC_BENCH_SERVER_CPU != 0); }
    bool supports_polling() const { return true; }
    void polling(bool value) {
        spin = value;
        // Negotiated case policy: while spin is set the peer never arms its
        // sleep flag, so publishes can skip the wake handshake entirely.
        outgoing.peer_polls = value;
        incoming.peer_polls = value;
    }
    u64 process() const { return identity; }
    u64 peer_process() const { return peer; }
    [[noreturn]] void fail(const char* why) { failure(why); }
    void pin(u32 cpu) {
        // Affinity is immutable after resume in Ame. The bootstrap pins both
        // suspended workers; each placement uses a fresh isolated boot.
        check(cpu < cpus && this->cpu() == cpu, "benchmark affinity");
    }
    u32 cpu() const { return static_cast<u32>(sys_thread_get_cpu()); }
    u64 now() const { return mx_clock_ns(); }
    void write(const char* data, size_t length) { sys_debug_puts(data, length); }
    Counters counters() {
        Counters result{};
        for (auto& value : result.values) value = kUnavailable;
        result.values[ContextSwitches] = sys_debug_counter(DBG_CONTEXT_SWITCHES);
        result.values[IpcSyscalls] = sys_debug_counter(DBG_IPC_SYSCALL_COUNT);
        result.values[DoorbellSleeps] = sys_debug_counter(DBG_DOORBELL_SLEEPS);
        result.values[Notifications] = sys_debug_counter(DBG_NOTIFY_CALLS);
        result.values[Yields] = sys_debug_counter(DBG_YIELD_COUNT);
        result.values[RemoteWakes] = sys_debug_counter(DBG_REMOTE_WAKES);
        result.values[IpisSent] = sys_debug_counter(DBG_IPI_SENT);
        result.values[TimerPreempts] = sys_debug_counter(DBG_TIMER_PREEMPTS);
        result.values[LocalWakes] = sys_debug_counter(DBG_LOCAL_WAKES);
        return result;
    }
    void flush_notify() {
        if (pending_notify) {
            check(mx_doorbell_notify(outgoing.doorbell) == MX_OK, "flush deferred notify");
            pending_notify = false;
        }
    }
    void send(const Message& message) {
#if AME_IPC_BENCH_REFERENCE_FRAMING
        ipc_bench::slotwise::send(*this, message);
#else
        check(message.size <= kMaxPayload, "outgoing length");
        mochi::FrameWriter writer(message.kind, message.sequence, message.payload, message.size);
        for (;;) {
            const auto result = writer.push(outgoing);
            check(result != mochi::FrameProgress::Invalid, "outgoing frame/ring");
            if (result == mochi::FrameProgress::Complete) {
                if (message.kind == Closed) {
                    flush_notify();
                    // The final response has no following receive to complete
                    // a synchronous reply token. Resolve it before process exit;
                    // ordinary/cross-core peers have no token and return INVALID.
                    const auto replied = sys_reply_handoff(MX_OK);
                    check(replied == MX_OK || replied == MX_ERR_INVALID, "closing handoff");
                }
                return;
            }
            if (result == mochi::FrameProgress::Blocked) {
                flush_notify();
                if (spin) libk::cpu::relax();
                else check(outgoing.wait_writable(writer.required_space(outgoing),
                                                  outgoing.poll_budget), "producer space wait");
            }
        }
#endif
    }
    void receive(Message& message) {
#if AME_IPC_BENCH_REFERENCE_FRAMING
        ipc_bench::slotwise::receive(*this, message);
#else
        if (pending_notify) {
            if (spin) flush_notify();
            else {
                const uint32_t flags = server_role ? MX_IPC_EXCHANGE_REPLY : MX_IPC_EXCHANGE_HANDOFF;
                check(incoming.wait_readable_after_notify(outgoing.doorbell, flags), "exchange/wait");
                pending_notify = false;
            }
        }
        mochi::FrameReader reader(message.payload, kMaxPayload);
        for (;;) {
            const auto result = reader.pop(incoming, 16, !spin);
            check(result != mochi::FrameProgress::Invalid, "incoming frame/ring");
            if (result == mochi::FrameProgress::Complete) break;
            if (result == mochi::FrameProgress::Blocked) {
                if (spin) libk::cpu::relax();
#if AME_IPC_BENCH_ADAPTIVE
                else {
                    bool parked = false;
                    check(incoming.wait_readable(budget.budget, &parked), "ring receive/wait");
                    if (const u32 next = budget.note_wait(parked)) (void)mx_ipc_policy_set_budget(next);
                }
#else
                else check(incoming.wait_readable_auto(), "ring receive/wait");
#endif
            }
        }
        message.kind = reader.type(); message.sequence = reader.sequence(); message.size = reader.size();
#endif
    }
};

struct Child { CapHandle process, thread; u64 identity; };
struct Channel { CapHandle requests, replies, request_db, reply_db, request_space, reply_space; };
Child spawn(const u8* image, size_t size, CapHandle supervisor) {
    static const char name[32] = "bench.ipc";
    const auto result = sys_spawn(image, size, MX_POLICY_PRINCIPAL_NATIVE_TRUSTED,
                                  supervisor, MX_SPAWN_SUSPENDED, name);
    Child child{cap(result.value), cap(result.detail), 0};
    MxCapInfo info{};
    check(mx_cap_info(child.process, &info) == MX_OK, "process identity");
    child.identity = info.base;
    return child;
}
void wire(Child child, Child peer, const Channel& channel, bool server, u32 cpus, u64 va) {
    const auto arena = cap(sys_arena_create(4096, 0));
    check(!mx_map_page(arena, va, RIGHT_READ | RIGHT_WRITE), "child BootInfo map");
    auto* boot = reinterpret_cast<MxBootInfo*>(va);
    libk::zero(*boot);
    // Slot zero must be the first grant into a new process.
    check(sys_cap_grant(child.process, arena, RIGHT_READ | RIGHT_MAP) == (u64{1} << 32), "bootstrap slot");
    boot->magic = MX_BOOT_MAGIC; boot->version = MX_BOOT_VERSION;
    auto add = [&](const char* name, u32 type, CapHandle handle, u64 value = 0) {
        check(boot->entry_count < MX_BOOT_MAX_ENTRIES, "BootInfo capacity");
        auto& entry = boot->entries[boot->entry_count++];
        entry.type = type; entry.handle = handle; entry.aux0 = value;
        libk::str::copy_into(entry.name, sizeof(entry.name), name);
    };
    const u32 rw = RIGHT_READ | RIGHT_WRITE | RIGHT_MAP;
    add("bench.tx", MX_BOOT_ENTRY_RING,
        cap(sys_ring_share(server ? channel.replies : channel.requests, child.process, rw)), kOrder);
    add("bench.rx", MX_BOOT_ENTRY_RING,
        cap(sys_ring_share(server ? channel.requests : channel.replies, child.process, rw)), kOrder);
    add("bench.tx.db", MX_BOOT_ENTRY_DOORBELL,
        cap(sys_doorbell_share(server ? channel.reply_db : channel.request_db, child.process, RIGHT_NOTIFY)));
    add("bench.rx.db", MX_BOOT_ENTRY_DOORBELL,
        cap(sys_doorbell_share(server ? channel.request_db : channel.reply_db, child.process, RIGHT_WAIT)));
    add("bench.tx.space", MX_BOOT_ENTRY_DOORBELL,
        cap(sys_doorbell_share(server ? channel.reply_space : channel.request_space, child.process, RIGHT_WAIT)));
    add("bench.rx.space", MX_BOOT_ENTRY_DOORBELL,
        cap(sys_doorbell_share(server ? channel.request_space : channel.reply_space, child.process, RIGHT_NOTIFY)));
    add("thread.self", MX_BOOT_ENTRY_HANDLE,
        cap(sys_cap_grant(child.process, child.thread, RIGHT_READ | RIGHT_THREAD_CONTROL)));
    add("role", MX_BOOT_ENTRY_VALUE, 0, server ? 2 : 1);
    add("cpu.count", MX_BOOT_ENTRY_VALUE, 0, cpus);
    add("self.identity", MX_BOOT_ENTRY_VALUE, 0, child.identity);
    add("peer.identity", MX_BOOT_ENTRY_VALUE, 0, peer.identity);
}
} // namespace

extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map();
    if (const auto* role = mochi::boot::find(boot, "role")) {
        auto memory = mochi::make_state(0x20000000, 256 * 1024);
        auto* state = memory.alloc<State>(1);
        check(state, "benchmark state arena");
        const auto link = mochi::boot::map_endpoint(boot,
            {"bench.tx", 0x18000000, "bench.tx.db", kOrder, "bench.tx.space"},
            {"bench.rx", 0x18010000, "bench.rx.db", kOrder, "bench.rx.space"});
        check(link.valid(), "benchmark endpoint");
        Ame backend{link.tx(), link.rx(), mochi::boot::require_handle(boot, "thread.self"),
                    static_cast<u32>(mochi::boot::find(boot, "cpu.count")->aux0),
                    mochi::boot::find(boot, "self.identity")->aux0,
                    mochi::boot::find(boot, "peer.identity")->aux0};
        backend.server_role = role->aux0 == 2;
#if !AME_IPC_BENCH_REFERENCE_FRAMING && AME_IPC_BENCH_SERVER_CPU == 0
        // Cross-core consumers can overlap their work with production. Wake
        // them on first publication instead of deferring until the whole window.
        backend.outgoing.deferred_notify = &backend.pending_notify;
#endif
        // Publish the benchmark's bounded poll through the kernel policy path,
        // then read it back the way a service or profiler would.
        (void)mx_ipc_policy_set_budget(kPollBudget);
        backend.outgoing.poll_budget = mx_ipc_policy_budget();
        backend.incoming.poll_budget = backend.outgoing.poll_budget;
        check(backend.incoming.poll_budget == kPollBudget, "ipc policy roundtrip");
        backend.budget.seed(backend.incoming.poll_budget);
        // Only cross-core placement may spin: a same-core peer needs this CPU.
        backend.incoming.cross_core = AME_IPC_BENCH_SERVER_CPU != 0;
        backend.outgoing.cross_core = AME_IPC_BENCH_SERVER_CPU != 0;
        if (role->aux0 == 2) ipc_bench::server(backend, *state);
        else {
            const Config config{AME_IPC_BENCH_ITERATIONS, AME_IPC_BENCH_WARMUP, AME_IPC_BENCH_SAMPLES,
                                AME_IPC_BENCH_ROUNDS, AME_IPC_BENCH_WINDOW, AME_IPC_BENCH_PAYLOAD_MASK,
                                AME_IPC_BENCH_SPIN != 0};
            ipc_bench::client(backend, *state, config);
        }
        mx_exit(0);
        mochi::park();
    }
    const auto archive = mochi::boot::require_handle(boot, "boot.initrd");
    check(!mx_map_page(archive, 0x20000000, RIGHT_READ), "benchmark archive mapping");
    midix::format::Bundle bundle{};
    bundle.data = reinterpret_cast<const u8*>(0x20000000);
    bundle.size = mochi::boot::find(boot, "boot.initrd")->aux0;
    check(bundle.size >= sizeof(bundle.header), "benchmark archive header");
    (void)libk::copy_exact(libk::as_bytes(&bundle.header, sizeof(bundle.header)),
                          libk::as_bytes(bundle.data, sizeof(bundle.header)));
    MxBundleObject object{};
    check(midix::format::find_object(bundle, "bench.ipc", object), "benchmark executable");
    const auto supervisor = mochi::boot::require_handle(boot, "supervisor");
    const auto client = spawn(bundle.data + object.offset, object.size, supervisor);
    const auto server = spawn(bundle.data + object.offset, object.size, supervisor);
    constexpr u32 features = AME_IPC_BENCH_REFERENCE_FRAMING ? 0 : MX_RING_PACKED_FRAMES;
    const Channel channel{cap(sys_ring_create(kOrder, features)), cap(sys_ring_create(kOrder, features)),
                          cap(sys_doorbell_create(MX_IPC_HINT_LATENCY | MX_IPC_HINT_HANDOFF_OK | MX_IPC_HINT_SERVER)),
                          cap(sys_doorbell_create()), cap(sys_doorbell_create()), cap(sys_doorbell_create())};
    const auto cpus = static_cast<u32>(mochi::boot::find(boot, "cpu.count")->aux0);
    check(cpus && cpus <= 8, "CPU count");
    wire(client, server, channel, false, cpus, 0x30000000);
    wire(server, client, channel, true, cpus, 0x30010000);
    check(!sys_thread_set_affinity(client.thread, 1) && !sys_thread_set_affinity(server.thread, u64{1} << AME_IPC_BENCH_SERVER_CPU), "initial affinity");
    check(!sys_thread_resume(server.thread) && !sys_thread_resume(client.thread), "peer start");
    mochi::park();
}
