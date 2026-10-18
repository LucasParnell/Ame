// Native Linux reference adapters. Both peers are separate processes, pinned
// by the common workload before warmup. No affinity/clock setup is timed.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <signal.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "workload.h"

struct Linux {
    int rx, tx;
    bool packet, parent;
    unsigned client_id, server_id;
    pid_t peer;

    [[noreturn]] void fail(const char* why) const {
        std::fprintf(stderr, "IPC_BENCH_ERROR: %s: %s\n", why, std::strerror(errno));
        if (parent && peer > 0) kill(peer, SIGTERM);
        std::exit(2);
    }
    const char* transport() const { return packet ? "linux_unix_seqpacket" : "linux_pipe_pair"; }
    const char* diagnostic_scope() const { return "client_process"; }
    unsigned client_cpu() const { return client_id; }
    unsigned server_cpu(bool different) const { return different ? server_id : client_id; }
    const char* placement_scope() const { return "both"; }
    bool supports_placement(bool) const { return true; }
    bool supports_polling() const { return false; }
    void polling(bool spin) { if (spin) fail("Linux reference adapters use blocking I/O"); }
    ipc_bench::u64 process() const { return static_cast<ipc_bench::u64>(getpid()); }
    ipc_bench::u64 peer_process() const { return static_cast<ipc_bench::u64>(peer); }
    void pin(unsigned cpu) {
        if (cpu >= CPU_SETSIZE) fail("CPU id outside cpu_set_t");
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set)) fail("sched_setaffinity");
    }
    unsigned cpu() {
        const int value = sched_getcpu();
        if (value < 0) fail("sched_getcpu");
        return static_cast<unsigned>(value);
    }
    ipc_bench::u64 now() {
        timespec time{};
        // A real syscall, as on Ame. Do not silently give Linux a vDSO clock
        // while instrumenting Ame with two kernel entries per latency sample.
        if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC_RAW, &time)) fail("clock_gettime syscall");
        return static_cast<ipc_bench::u64>(time.tv_sec) * 1000000000ull + time.tv_nsec;
    }
    ipc_bench::Counters counters() {
        ipc_bench::Counters out{};
        for (auto& value : out.values) value = ipc_bench::kUnavailable;
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage)) fail("getrusage");
        out.values[ipc_bench::VoluntarySwitches] = usage.ru_nvcsw;
        out.values[ipc_bench::InvoluntarySwitches] = usage.ru_nivcsw;
        out.values[ipc_bench::ContextSwitches] = usage.ru_nvcsw + usage.ru_nivcsw;
        return out;
    }
    void write(const char* data, size_t size) {
        if (std::fwrite(data, 1, size, stdout) != size || std::fflush(stdout)) fail("result output");
    }
    void exact(void* data, size_t size, bool sending) {
        auto* bytes = static_cast<unsigned char*>(data);
        while (size) {
            const ssize_t count = sending ? ::write(tx, bytes, size) : ::read(rx, bytes, size);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) fail(sending ? "pipe write" : "pipe EOF/read");
            bytes += count; size -= static_cast<size_t>(count);
        }
    }
    void send(const ipc_bench::Message& message) {
        if (message.size > ipc_bench::kMaxPayload) fail("outgoing length");
        const size_t bytes = offsetof(ipc_bench::Message, payload) + message.size;
        if (!packet) {
            // Single writer per direction; records may exceed PIPE_BUF, so
            // partial writes/reads are handled rather than assuming atomicity.
            exact(const_cast<ipc_bench::Message*>(&message), bytes, true);
            return;
        }
        ssize_t count;
        do { count = ::send(tx, &message, bytes, MSG_NOSIGNAL); } while (count < 0 && errno == EINTR);
        if (count != static_cast<ssize_t>(bytes)) fail("seqpacket send");
    }
    void receive(ipc_bench::Message& message) {
        if (!packet) {
            exact(&message, offsetof(ipc_bench::Message, payload), false);
            if (message.size > ipc_bench::kMaxPayload) fail("pipe message length");
            exact(message.payload, message.size, false);
            return;
        }
        ssize_t count;
        do { count = ::recv(rx, &message, sizeof(message), MSG_TRUNC); } while (count < 0 && errno == EINTR);
        if (count < static_cast<ssize_t>(offsetof(ipc_bench::Message, payload)) ||
            count > static_cast<ssize_t>(sizeof(message)) || message.size > ipc_bench::kMaxPayload ||
            static_cast<size_t>(count) != offsetof(ipc_bench::Message, payload) + message.size)
            fail("seqpacket receive/EOF/length");
    }
};

static unsigned number(const char* text) {
    char* end = nullptr; errno = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno || !*text || *end || value > 0xfffffffful) {
        std::fprintf(stderr, "invalid integer: %s\n", text); std::exit(2);
    }
    return static_cast<unsigned>(value);
}

int main(int argc, char** argv) {
    if (argc != 12 || (std::strcmp(argv[1], "seqpacket") && std::strcmp(argv[1], "pipe"))) {
        std::fprintf(stderr, "use scripts/test/ipc-bench.py linux (see --help)\n");
        return 2;
    }
    const bool packet = !std::strcmp(argv[1], "seqpacket");
    const unsigned client = number(argv[2]), server = number(argv[3]);
    ipc_bench::Config config{number(argv[4]), number(argv[5]), number(argv[6]), number(argv[7]),
                             number(argv[8]), number(argv[9]), number(argv[10]) != 0};
    // Last argument is the wire-contract version; stale runners fail closed.
    if (!config.valid() || number(argv[11]) != ipc_bench::kVersion) return 2;
    signal(SIGPIPE, SIG_IGN);
    auto* state = new (std::nothrow) ipc_bench::State{};
    if (!state) return 2;
    int forward[2], reverse[2] = {-1, -1};
    if ((packet && socketpair(AF_UNIX, SOCK_SEQPACKET, 0, forward)) ||
        (!packet && (pipe(forward) || pipe(reverse)))) return 2;
    const pid_t self = getpid(), child = fork();
    if (child < 0) return 2;
    if (!child) {
        if (packet) close(forward[0]);
        else { close(forward[1]); close(reverse[0]); }
        Linux backend{forward[packet ? 1 : 0], packet ? forward[1] : reverse[1], packet,
                      false, client, server, self};
        ipc_bench::server(backend, *state);
        _exit(0);
    }
    if (packet) close(forward[1]);
    else { close(forward[0]); close(reverse[1]); }
    Linux backend{packet ? forward[0] : reverse[0], packet ? forward[0] : forward[1], packet,
                  true, client, server, child};
    ipc_bench::client(backend, *state, config);
    close(backend.rx);
    if (backend.tx != backend.rx) close(backend.tx);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) backend.fail("waitpid");
    delete state;
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : 2;
}
