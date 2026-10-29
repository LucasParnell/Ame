// Spawn two workers from the isolated boot archive or an app image snapshot.
// App launches relay worker output and reap both peers before returning.
#include "workload.h"
#include "mochi/app_ipc.h"
#include "mochi/app.h"
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
#ifndef AME_IPC_BENCH_RING_ORDER
#define AME_IPC_BENCH_RING_ORDER 0
#endif
constexpr auto kGeometry = mochi::FrameGeometry::for_records(kMaxPayload, 7);
constexpr u32 kOrder =
    AME_IPC_BENCH_REFERENCE_FRAMING
        ? MX_DEFAULT_RING_ORDER
        : (AME_IPC_BENCH_RING_ORDER ? AME_IPC_BENCH_RING_ORDER : kGeometry.order);
[[maybe_unused]] constexpr u32 kPollBudget =
    AME_IPC_BENCH_SERVER_CPU ? AME_IPC_BENCH_POLL_BUDGET : 0;

[[noreturn]] void failure(const char* why) {
    constexpr char prefix[] = "IPC_BENCH_ERROR: ";
    mochi::app::write(prefix, sizeof(prefix) - 1);
    mochi::app::puts(why);
    mochi::app::puts("\n");
    mochi::app::exit(MX_ERR_FAULT);
}
void check(bool value, const char* why) {
    if (!value)
        failure(why);
}
CapHandle cap(int64_t value, const char* operation = "capability allocation/grant") {
    if (value <= 0) {
        mochi::app::puts("IPC_BENCH_STATUS: ");
        mochi::app::put_dec(value < 0 ? -value : value);
        mochi::app::puts("\n");
        failure(operation);
    }
    return static_cast<CapHandle>(value);
}

struct Ame : mochi::FramedChannel {
    Ame(mochi::Ring tx, mochi::Ring rx, CapHandle self, u32 count, u64 id, u64 peer_id, bool server)
        : FramedChannel(tx, rx,
                        {AME_IPC_BENCH_SERVER_CPU != 0, kPollBudget, AME_IPC_BENCH_ADAPTIVE != 0},
                        server ? mochi::IpcRole::Server : mochi::IpcRole::Caller,
                        AME_IPC_BENCH_REFERENCE_FRAMING ? mochi::FrameProtocol::Compatible
                                                        : mochi::FrameProtocol::CompleteRecords),
          self_thread(self), cpus(count), identity(id), peer(peer_id) {
    }
    CapHandle self_thread;
    u32 cpus;
    u64 identity, peer;
    bool spin = false;
    bool shared_system = false;
    const char* transport() const {
        return "ame_mochi_ring_framed";
    }
    const char* diagnostic_scope() const {
        return shared_system ? "shared_system" : "isolated_system";
    }
    u32 client_cpu() const {
        return 0;
    }
    u32 server_cpu(bool different) const {
        return different && cpus > 1 ? 1 : 0;
    }
    const char* placement_scope() const {
        return AME_IPC_BENCH_SERVER_CPU ? "different_cpu" : "same_cpu";
    }
    bool supports_placement(bool different) const {
        return different == (AME_IPC_BENCH_SERVER_CPU != 0);
    }
    bool supports_polling() const {
        return true;
    }
    void polling(bool value) {
        spin = value;
        check(mochi::FramedChannel::polling(value) == MX_OK, "polling policy");
    }
    u64 process() const {
        return identity;
    }
    u64 peer_process() const {
        return peer;
    }
    [[noreturn]] void fail(const char* why) {
        failure(why);
    }
    void pin(u32 cpu) {
        // Affinity is immutable after resume in Ame. The bootstrap pins both
        // suspended workers; each placement uses a fresh isolated boot.
        check(cpu < cpus && this->cpu() == cpu, "benchmark affinity");
    }
    u32 cpu() const {
        return static_cast<u32>(sys_thread_get_cpu());
    }
    u64 now() const {
        return mx_clock_ns();
    }
    void write(const char* data, size_t length) {
        // Parent-captured stdout when launched from msh; kernel debug otherwise.
        mochi::app::write(data, static_cast<uint32_t>(length));
    }
    Counters counters() {
        Counters result{};
        for (auto& value : result.values)
            value = kUnavailable;
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
    void send(const Message& message) {
#if AME_IPC_BENCH_REFERENCE_FRAMING
        ipc_bench::slotwise::send(*this, message);
#else
        check(message.size <= kMaxPayload, "outgoing length");
        // Copy each stream record now, then publish a whole available run. The
        // completion-ACK record ends the logical window; RPC remains immediate.
        const auto mode =
            message.kind == Stream ? mochi::FrameFlush::Buffered : mochi::FrameFlush::OnWait;
        check(mochi::FramedChannel::send(message.kind, message.sequence, message.payload,
                                         message.size, mode) == MX_OK,
              "frame send");
        if (message.kind == Closed)
            check(finish_reply() == MX_OK, "closing handoff");
#endif
    }
    void receive(Message& message) {
#if AME_IPC_BENCH_REFERENCE_FRAMING
        ipc_bench::slotwise::receive(*this, message);
#else
        mochi::FrameInfo info{};
        check(mochi::FramedChannel::receive(info, message.payload, kMaxPayload) == MX_OK,
              "frame receive");
        message.kind = info.type;
        message.sequence = info.sequence;
        message.size = info.size;
#endif
    }
};

// The app API owns resource construction, scoped handoff authority and all six
// named grants. This adapter supplies only workload roles and peer identity.
void wire(mochi::app::Process& child, const mochi::app::Process& peer,
          const mochi::app::ChannelPair& channel, bool server, bool app) {
    check(channel.share(child, server, "bench") == MX_OK, "worker channel");
    check(child.value("role", server ? 2 : 1) == MX_OK &&
              child.value("peer.identity", peer.identity) == MX_OK,
          "worker role and peer");
    if (app)
        check(child.value("bench.shared_system", 1) == MX_OK, "counter scope");
}
} // namespace

extern "C" [[noreturn]] void _start() {
    auto& context = mochi::app::start();
    check(&mochi::app::start() == &context, "idempotent app startup");
    const auto* boot = context.boot;
    if (const auto* role = mochi::boot::find(boot, "role")) {
        auto memory = mochi::make_state(0x20000000, 256 * 1024);
        auto* state = memory.alloc<State>(1);
        check(state, "benchmark state arena");
        const auto link = mochi::app::ChannelPair::map(boot, 0x18000000, 0x18020000, "bench");
        check(link.valid(), "benchmark endpoint");
        Ame backend{link.tx(),
                    link.rx(),
                    mochi::boot::require_handle(boot, "thread.self"),
                    context.cpu_count(),
                    mochi::boot::find(boot, "self.identity")->aux0,
                    mochi::boot::find(boot, "peer.identity")->aux0,
                    role->aux0 == 2};
        backend.shared_system = mochi::boot::find(boot, "bench.shared_system") != nullptr;
        if (role->aux0 == 2)
            ipc_bench::server(backend, *state);
        else {
            const Config config{AME_IPC_BENCH_ITERATIONS, AME_IPC_BENCH_WARMUP,
                                AME_IPC_BENCH_SAMPLES,    AME_IPC_BENCH_ROUNDS,
                                AME_IPC_BENCH_WINDOW,     AME_IPC_BENCH_PAYLOAD_MASK,
                                AME_IPC_BENCH_SPIN != 0};
            ipc_bench::client(backend, *state, config);
        }
        mochi::app::exit(0);
    }
    const bool app = mochi::boot::find(boot, "app.image.size") != nullptr;
    const u8* image = nullptr;
    u64 image_size = 0;
    if (app) {
        const auto executable = context.self_image();
        check(static_cast<bool>(executable), "app executable snapshot");
        image = executable.data;
        image_size = executable.size;
    } else {
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
        image = bundle.data + object.offset;
        image_size = object.size;
    }
    static mochi::app::Process children[2];
    for (u32 index = 0; index < 2; ++index)
        check(children[index].configure(boot, nullptr, index) == MX_OK, "worker launcher");
    const auto wake = app ? cap(sys_doorbell_create()) : kNullCap;
    mochi::app::SpawnOptions options{};
    options.name = "bench.ipc";
    options.principal = MX_POLICY_PRINCIPAL_NATIVE_TRUSTED;
    options.self_image = false;
    options.capture_stdout = app;
    options.output_wake = wake;
    for (auto& child : children)
        check(child.spawn({image, image_size}, options) == MX_OK, "spawn benchmark worker");
    auto& client = children[0];
    auto& server = children[1];
    constexpr u32 features = AME_IPC_BENCH_REFERENCE_FRAMING ? 0 : MX_RING_PACKED_FRAMES;
    mochi::app::ChannelPair channel;
    check(channel.create(server, {kOrder, kMaxPayload, 3}, true, features) == MX_OK,
          "benchmark channel");
    check(context.cpu_count() && context.cpu_count() <= 8, "CPU count");
    wire(client, server, channel, false, app);
    wire(server, client, channel, true, app);
    check(server.resume(u64{1} << AME_IPC_BENCH_SERVER_CPU) == MX_OK && client.resume(1) == MX_OK,
          "pin and start workers");
    if (app) {
        mx_i32_t status = 0;
        check(mochi::app::wait_all(children, 2, wake, &status) == MX_OK,
              "wait for benchmark workers");
        mochi::app::exit(status);
    }
    mochi::park();
}
