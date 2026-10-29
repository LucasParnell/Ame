// Separate contract: generate directly into a loan, validate its shared view,
// then return ownership. RPC returns the same buffer; it is not a copied echo.
#include "workload.h"
#include "mochi/app_ipc.h"
#include "mochi/app_loan.h"
#include "mochi/state.h"
#include "mochi/wait.h"
#include "midix/format/bundle.h"

namespace {
using namespace ipc_bench;
using mochi::LoanDescriptor;
constexpr u32 kBytes = 4096, kBuffers = 32;
constexpr u32 kIterations = AME_IPC_BENCH_ITERATIONS, kWarmup = AME_IPC_BENCH_WARMUP;
constexpr u32 kRounds = AME_IPC_BENCH_ROUNDS, kWindow = AME_IPC_BENCH_WINDOW;
static_assert(kWindow && kWindow <= kBuffers && kIterations && kWarmup && kRounds);
enum Wire : u32 { Setup = 1, Data, Last, Reply, Stop, Stopped };
struct Settings {
    u32 loan, stream;
};
struct Scratch {
    alignas(64) uint8_t bytes[kBytes];
    LoanDescriptor returned[kBuffers];
    LoanDescriptor sent[kBuffers];
    char line[512];
};
[[noreturn]] void fail(const char* why) {
    mochi::app::puts("IPC_LOAN_ERROR: ");
    mochi::app::puts(why);
    mochi::app::puts("\n");
    mochi::app::exit(MX_ERR_FAULT);
}
void check(bool ok, const char* why) {
    if (!ok)
        fail(why);
}
void generate(libk::bytes bytes, u64 seq) {
    for (u32 i = 0; i < bytes.size; ++i)
        bytes.data[i] = pattern(seq, i);
}
void validate(libk::const_bytes bytes, u64 seq) {
    check(bytes.size == kBytes, "payload length");
    for (u32 i = 0; i < kBytes; ++i)
        if (bytes.data[i] != pattern(seq, i))
            fail("payload contents");
}
mochi::FrameInfo receive(mochi::FramedChannel& channel, void* bytes, u32 capacity) {
    mochi::FrameInfo info{};
    check(channel.receive(info, bytes, capacity) == MX_OK, "receive");
    return info;
}
void send(mochi::FramedChannel& channel, u32 type, u64 seq, const void* bytes, u32 size,
          mochi::FrameFlush flush = mochi::FrameFlush::OnWait) {
    check(channel.send(type, seq, bytes, size, flush) == MX_OK, "send");
}
void server(mochi::FramedChannel& channel, mochi::LoanReader& reader, Scratch& scratch) {
    Settings settings{};
    u64 sequence = 0;
    u32 returned = 0;
    bool configured = false;
    for (;;) {
        const auto info = receive(channel, scratch.bytes, sizeof(scratch.bytes));
        if (info.type == Stop) {
            check(!info.size && !returned && reader.drained(), "stop ownership");
            send(channel, Stopped, 0, nullptr, 0);
            check(channel.finish_reply() == MX_OK, "final reply");
            return;
        }
        if (info.type == Setup) {
            check(info.size == sizeof(settings) && !returned && reader.drained(),
                  "setup size/state");
            libk::copy_exact(libk::as_bytes(&settings, sizeof(settings)),
                             libk::as_bytes(scratch.bytes, info.size));
            check(settings.loan <= 1 && settings.stream <= 1, "setup mode");
            configured = true;
            sequence = 0;
            send(channel, Reply, 0, nullptr, 0);
            continue;
        }
        check(configured && (info.type == Data || info.type == Last) && info.sequence == ++sequence,
              "data sequence/type");
        if (settings.loan) {
            check(info.size == sizeof(LoanDescriptor) && returned < kWindow,
                  "descriptor size/window");
            LoanDescriptor descriptor{};
            libk::copy_exact(libk::as_bytes(&descriptor, sizeof(descriptor)),
                             libk::as_bytes(scratch.bytes, info.size));
            mochi::ReadLoan loan;
            check(reader.accept(descriptor, loan) == MX_OK, "accept loan");
            validate(loan.bytes(), sequence);
            check(loan.release(scratch.returned[returned++]) == MX_OK, "return loan");
            if (!settings.stream || info.type == Last) {
                send(channel, Reply, sequence, scratch.returned, returned * sizeof(LoanDescriptor));
                returned = 0;
            }
        } else {
            validate(libk::as_bytes(scratch.bytes, info.size), sequence);
            if (!settings.stream)
                send(channel, Reply, sequence, scratch.bytes, kBytes);
            else if (info.type == Last)
                send(channel, Reply, sequence, nullptr, 0);
        }
    }
}
void transactions(mochi::FramedChannel& channel, mochi::LoanWriter& writer, Scratch& scratch,
                  Settings settings, u32 count, u64& sequence) {
    for (u32 sent = 0; sent < count;) {
        const u32 group = settings.stream ? (count - sent < kWindow ? count - sent : kWindow) : 1;
        for (u32 i = 0; i < group; ++i) {
            ++sequence;
            const auto type = i + 1 == group ? Last : Data;
            const auto flush = settings.stream && type == Data ? mochi::FrameFlush::Buffered
                                                               : mochi::FrameFlush::OnWait;
            if (settings.loan) {
                mochi::WriteLoan loan;
                check(writer.acquire(kBytes, loan) == MX_OK, "loan backpressure");
                generate(loan.bytes(), sequence);
                check(loan.publish(scratch.sent[i]) == MX_OK, "publish loan");
                send(channel, type, sequence, &scratch.sent[i], sizeof(LoanDescriptor), flush);
            } else {
                generate(libk::as_bytes(scratch.bytes), sequence);
                send(channel, type, sequence, scratch.bytes, kBytes, flush);
            }
        }
        const auto reply = receive(channel, scratch.bytes, sizeof(scratch.bytes));
        check(reply.type == Reply && reply.sequence == sequence, "reply sequence/type");
        if (settings.loan) {
            check(reply.size == group * sizeof(LoanDescriptor), "returned descriptor count");
            libk::copy_exact(libk::as_bytes(scratch.returned, reply.size),
                             libk::as_bytes(scratch.bytes, reply.size));
            for (u32 i = 0; i < group; ++i) {
                const auto& d = scratch.returned[i];
                const auto& original = scratch.sent[i];
                check(d.pool == original.pool && d.slot == original.slot &&
                          d.generation == original.generation && d.size == original.size,
                      "returned descriptor identity");
                if (settings.stream)
                    check(writer.reclaim(d) == MX_OK, "reclaim stream");
                else {
                    mochi::WriteLoan returned;
                    check(writer.reclaim(d, &returned) == MX_OK, "reclaim RPC");
                    validate(returned.bytes(), sequence);
                }
            }
            check(writer.drained(), "window ownership");
        } else if (settings.stream)
            check(!reply.size, "stream ACK size");
        else
            validate(libk::as_bytes(scratch.bytes, reply.size), sequence);
        sent += group;
    }
}
void client(mochi::FramedChannel& channel, mochi::LoanWriter& writer, Scratch& scratch,
            bool shared) {
    libk::str::Buffer header(scratch.line);
    header.format(
        "IPC_LOAN {\"record\":\"header\",\"schema\":\"ame.ipc-loans/v1\",\"payload_bytes\":4096,"
        "\"iterations\":%u,\"warmup\":%u,\"rounds\":%u,\"window\":%u,\"buffers\":32,"
        "\"server_cpu\":%u,\"shared_system\":%s}\n",
        kIterations, kWarmup, kRounds, kWindow, AME_IPC_BENCH_SERVER_CPU,
        shared ? "true" : "false");
    mochi::app::write(header.data, header.length);
    for (u32 loan = 0; loan < 2; ++loan)
        for (u32 stream = 0; stream < 2; ++stream) {
            Settings settings{loan, stream};
            send(channel, Setup, 0, &settings, sizeof(settings));
            const auto ready = receive(channel, scratch.bytes, sizeof(scratch.bytes));
            check(ready.type == Reply && !ready.size && !ready.sequence, "setup reply");
            u64 sequence = 0;
            transactions(channel, writer, scratch, settings, kWarmup, sequence);
            for (u32 round = 0; round < kRounds; ++round) {
                const auto start = mx_clock_ns();
                transactions(channel, writer, scratch, settings, kIterations, sequence);
                const auto elapsed = mx_clock_ns() - start;
                libk::str::Buffer line(scratch.line);
                line.format("IPC_LOAN {\"record\":\"round\",\"transport\":\"%s\",\"mode\":\"%s\","
                            "\"round\":%u,\"operations\":%u,\"elapsed_ns\":%llu}\n",
                            loan ? "shared_loan" : "copied_ring", stream ? "stream" : "rpc", round,
                            kIterations, static_cast<ull>(elapsed));
                check(!line.truncated && elapsed, "result format/time");
                mochi::app::write(line.data, line.length);
            }
        }
    send(channel, Stop, 0, nullptr, 0);
    const auto stopped = receive(channel, scratch.bytes, sizeof(scratch.bytes));
    check(stopped.type == Stopped && !stopped.size && writer.drained(), "shutdown");
    mochi::app::puts("IPC_LOAN_DONE\n");
}
} // namespace

extern "C" [[noreturn]] void _start() {
    auto& context = mochi::app::start();
    const auto* boot = context.boot;
    if (const auto* role = mochi::boot::find(boot, "role")) {
        const bool serving = role->aux0 == 2;
        check(sys_thread_get_cpu() == (serving ? AME_IPC_BENCH_SERVER_CPU : 0), "worker affinity");
        auto state = mochi::make_state(0x20000000, 65536);
        auto* scratch = state.alloc<Scratch>();
        check(scratch, "scratch");
        const auto endpoint = mochi::app::ChannelPair::map(boot, 0x18000000, 0x18020000, "bench");
        check(endpoint.valid(), "worker endpoint");
        mochi::FramedChannel channel(
            endpoint.tx(), endpoint.rx(),
            mochi::IpcWaitPolicy::between(serving ? AME_IPC_BENCH_SERVER_CPU : 0,
                                          serving ? 0 : AME_IPC_BENCH_SERVER_CPU),
            serving ? mochi::IpcRole::Server : mochi::IpcRole::Caller,
            mochi::FrameProtocol::CompleteRecords);
        if (serving) {
            auto reader = mochi::app::SharedLoanPool::map_reader(boot, 0x19000000);
            check(reader.valid(), "read-only pool mapping");
            server(channel, reader, *scratch);
        } else {
            auto writer = mochi::app::SharedLoanPool::map_writer(boot, 0x19000000);
            check(writer.valid(), "writable pool mapping");
            client(channel, writer, *scratch, mochi::boot::find(boot, "shared") != nullptr);
        }
        mochi::app::exit(0);
    }
    const bool app = mochi::boot::find(boot, "app.image.size") != nullptr;
    const uint8_t* image = nullptr;
    u64 size = 0;
    if (app) {
        const auto self = context.self_image();
        image = self.data;
        size = self.size;
    } else {
        check(mx_map_page(mochi::boot::require_handle(boot, "boot.initrd"), 0x21000000,
                          RIGHT_READ) == MX_OK,
              "archive map");
        midix::format::Bundle bundle{};
        bundle.data = reinterpret_cast<const uint8_t*>(0x21000000);
        bundle.size = mochi::boot::find(boot, "boot.initrd")->aux0;
        check(bundle.size >= sizeof(bundle.header), "archive size");
        libk::copy_exact(libk::as_bytes(&bundle.header, sizeof(bundle.header)),
                         libk::as_bytes(bundle.data, sizeof(bundle.header)));
        MxBundleObject object{};
        check(midix::format::find_object(bundle, "bench.ipc.loan", object), "archive object");
        image = bundle.data + object.offset;
        size = object.size;
    }
    check(image && size && context.cpu_count() > AME_IPC_BENCH_SERVER_CPU,
          "image/CPU configuration");
    static mochi::app::Process children[2];
    const auto wake = app ? sys_doorbell_create() : 0;
    check(wake >= 0, "output wake");
    mochi::app::SpawnOptions options{};
    options.name = "bench.ipc.loan";
    options.self_image = false;
    options.principal = MX_POLICY_PRINCIPAL_NATIVE_TRUSTED;
    options.capture_stdout = app;
    options.output_wake = wake;
    for (u32 i = 0; i < 2; ++i) {
        check(children[i].configure(boot, nullptr, i) == MX_OK, "configure child");
        check(children[i].spawn({image, size}, options) == MX_OK, "spawn child");
    }
    mochi::app::ChannelPair channel;
    mochi::app::SharedLoanPool pool;
    check(channel.create(children[1], mochi::FrameGeometry::for_records(kBytes, 7)) == MX_OK,
          "create channel");
    check(pool.create(kBytes, kBuffers) == MX_OK, "create pool");
    for (u32 i = 0; i < 2; ++i) {
        check(channel.share(children[i], i == 1, "bench") == MX_OK &&
                  pool.share(children[i], i == 0) == MX_OK,
              "share channel/pool");
        check(children[i].value("role", i + 1) == MX_OK, "share role");
        if (app)
            check(children[i].value("shared", 1) == MX_OK, "share scope");
    }
    check(children[1].resume(u64{1} << AME_IPC_BENCH_SERVER_CPU) == MX_OK &&
              children[0].resume(1) == MX_OK,
          "resume workers");
    if (app) {
        mx_i32_t status = 0;
        check(mochi::app::wait_all(children, 2, wake, &status) == MX_OK, "wait workers");
        mochi::app::exit(status);
    }
    mochi::park();
}
