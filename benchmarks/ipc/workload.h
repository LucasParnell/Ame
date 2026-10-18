#pragma once
// IPC workload contract v1. Keep this loop identical across OS adapters.
// Setup, affinity changes, allocation, diagnostics and output are not timed.
#include <cstdint>
#include <cstddef>
#include "libk/bytes.h"
#include "libk/str.h"
#include "libk/text.h"
#ifndef IPC_BENCH_CONTRACT_SHA256
#define IPC_BENCH_CONTRACT_SHA256 "unrecorded"
#endif

namespace ipc_bench {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;
constexpr u32 kVersion = 1, kMaxPayload = 4096, kMaxSamples = 16384;
constexpr u32 kPayloads[] = {0, 8, 48, 256, 4096};
constexpr u64 kUnavailable = ~u64{0};

struct Config {
    u32 iterations = 1024, warmup = 256, samples = 4096, rounds = 5;
    u32 window = 32, payload_mask = 31;
    bool include_spin = false;
    bool valid() const {
        return iterations >= 16 && iterations <= 1048576 && warmup && warmup <= 1048576 &&
               samples >= 16 && samples <= kMaxSamples && rounds >= 3 && rounds <= 31 &&
               window && window <= 64 && payload_mask && !(payload_mask & ~31u);
    }
};
enum Kind : u32 { Configure = 1, Ready, Echo, EchoReply, Stream, StreamAck, Ack, Finish, Closed };
enum Mode : u32 { Rpc = 0, Throughput = 1 };
struct Case {
    u32 id, mode, bytes, different_cpu, spin, server_cpu, window;
};
// Linux transports carry this 16-byte header explicitly. Ame uses native slot
// metadata plus FIRST/LAST flags for fragmentation. Logical content is equal.
struct Message {
    u64 sequence;
    u32 size, kind;
    std::uint8_t payload[kMaxPayload];
};
static_assert(__builtin_offsetof(Message, payload) == 16 && sizeof(Message) == 4112);

enum Counter : u32 {
    ContextSwitches, VoluntarySwitches, InvoluntarySwitches,
    IpcSyscalls, DoorbellSleeps, Notifications, Yields, RemoteWakes, IpisSent, TimerPreempts, LocalWakes, CounterCount
};
struct Counters { u64 values[CounterCount]; };
struct State {
    Message outgoing{}, incoming{};
    u64 samples[kMaxSamples]{};
    char line[2048]{};
};

inline std::uint8_t pattern(u64 sequence, u32 offset) {
    return static_cast<std::uint8_t>((sequence * 131 + offset * 17) ^ (offset >> 3));
}
inline void prepare(Message& message, u32 kind, u64 sequence, u32 bytes) {
    message.sequence = sequence; message.kind = kind; message.size = bytes;
    for (u32 i = 0; i < bytes; ++i) message.payload[i] = pattern(sequence, i);
}
inline void control(Message& message, u32 kind, u64 sequence = 0) {
    message.kind = kind; message.sequence = sequence; message.size = 0;
}
inline bool valid_payload(const Message& message, u64 sequence, u32 bytes) {
    if (message.sequence != sequence || message.size != bytes || bytes > kMaxPayload) return false;
    for (u32 i = 0; i < bytes; ++i)
        if (message.payload[i] != pattern(sequence, i)) return false;
    return true;
}
template <class Backend> void require(Backend& backend, bool value, const char* why) {
    if (!value) backend.fail(why);
}
template <class Backend> void emit(Backend& backend, libk::str::Buffer& line) {
    require(backend, !line.truncated, "result line truncated");
    line.append('\n');
    require(backend, !line.truncated, "result line truncated");
    backend.write(line.data, line.length);
}

// Emit raw samples after the timed work. Quantiles and uncertainty are computed
// by the runner, using one definition for every adapter, without guest sorting.
template <class Backend>
void emit_samples(Backend& backend, State& state, const char* record, u32 id, u32 count) {
    for (u32 offset = 0; offset < count; offset += 64) {
        libk::str::Buffer line(state.line);
        line.format("IPC_BENCH {\"record\":\"%s\",\"case_id\":%u,\"offset\":%u,\"values_ns\":[",
                    record, id, offset);
        const u32 end = count - offset < 64 ? count : offset + 64;
        for (u32 i = offset; i < end; ++i)
            line.format("%s%llu", i == offset ? "" : ",", static_cast<ull>(state.samples[i]));
        line.append("]}");
        emit(backend, line);
    }
}

template <class Backend> void rpc(Backend& backend, State& state, const Case& item, u64& sequence) {
    prepare(state.outgoing, Echo, ++sequence, item.bytes);
    backend.send(state.outgoing);
    backend.receive(state.incoming);
    require(backend, state.incoming.kind == EchoReply &&
            valid_payload(state.incoming, sequence, item.bytes), "echo payload/sequence mismatch");
}

// One-way delivery with an ACK after each window (including the last partial
// window). The receiver consumes/validates every byte. ACKs carry no payload.
template <class Backend>
void transactions(Backend& backend, State& state, const Case& item, u32 count, u64& sequence) {
    if (item.mode == Rpc) {
        for (u32 i = 0; i < count; ++i) rpc(backend, state, item, sequence);
        return;
    }
    for (u32 sent = 0; sent < count;) {
        const u32 group = count - sent < item.window ? count - sent : item.window;
        for (u32 i = 0; i < group; ++i) {
            prepare(state.outgoing, i + 1 == group ? StreamAck : Stream, ++sequence, item.bytes);
            backend.send(state.outgoing);
        }
        backend.receive(state.incoming);
        require(backend, state.incoming.kind == Ack && !state.incoming.size &&
                state.incoming.sequence == sequence, "stream completion mismatch");
        sent += group;
    }
}

template <class Backend> void server(Backend& backend, State& state) {
    Case item{};
    u64 sequence = 0;
    bool configured = false;
    for (;;) {
        backend.receive(state.incoming);
        const auto& message = state.incoming;
        if (message.kind == Configure) {
            require(backend, message.size == sizeof(Case), "configuration size");
            (void)libk::copy_exact(libk::as_bytes(&item, sizeof(item)),
                                  libk::as_bytes(message.payload, sizeof(item)));
            require(backend, item.bytes <= kMaxPayload && item.window && item.window <= 64 &&
                    item.mode <= Throughput && item.spin <= 1 && item.different_cpu <= 1 &&
                    (!item.spin || item.different_cpu), "invalid case");
            backend.polling(false);
            backend.pin(item.server_cpu);
            const u32 cpu = backend.cpu();
            require(backend, cpu == item.server_cpu, "server CPU affinity");
            control(state.outgoing, Ready, item.id);
            state.outgoing.size = sizeof(cpu);
            (void)libk::copy_exact(libk::as_bytes(state.outgoing.payload, sizeof(cpu)), libk::as_bytes(&cpu, sizeof(cpu)));
            backend.send(state.outgoing);
            backend.polling(item.spin != 0);
            sequence = 0; configured = true;
            continue;
        }
        if (message.kind == Finish) {
            control(state.outgoing, Closed);
            backend.send(state.outgoing);
            return;
        }
        require(backend, configured && valid_payload(message, ++sequence, item.bytes),
                "request payload/sequence mismatch");
        if (item.mode == Rpc) {
            require(backend, message.kind == Echo, "echo opcode");
            state.incoming.kind = EchoReply;
            backend.send(state.incoming);
        } else {
            require(backend, message.kind == Stream || message.kind == StreamAck, "stream opcode");
            if (message.kind == StreamAck) {
                control(state.outgoing, Ack, sequence);
                backend.send(state.outgoing);
            }
        }
    }
}

template <class Backend> void client(Backend& backend, State& state, const Config& config) {
    require(backend, config.valid(), "benchmark configuration bounds");
    backend.pin(backend.client_cpu());
    {
        libk::str::Buffer line(state.line);
        line.format("IPC_BENCH {\"record\":\"header\",\"version\":%u,\"transport\":\"%s\","
                    "\"contract_sha256\":\"%s\","
                    "\"placement_scope\":\"%s\",\"diagnostic_scope\":\"%s\",\"iterations\":%u,\"warmup\":%u,\"samples\":%u,"
                    "\"rounds\":%u,\"window\":%u,\"payload_mask\":%u,\"include_spin\":%s,"
                    "\"client_cpu\":%u,\"client_process\":%llu,\"server_process\":%llu}",
                    kVersion, backend.transport(), IPC_BENCH_CONTRACT_SHA256, backend.placement_scope(), backend.diagnostic_scope(), config.iterations,
                    config.warmup, config.samples, config.rounds, config.window, config.payload_mask,
                    config.include_spin ? "true" : "false", backend.client_cpu(),
                    static_cast<ull>(backend.process()), static_cast<ull>(backend.peer_process()));
        emit(backend, line);
    }
    for (u32 i = 0; i < 1024; ++i) {
        const u64 start = backend.now(), end = backend.now();
        require(backend, end >= start, "nonmonotonic clock");
        state.samples[i] = end - start;
    }
    emit_samples(backend, state, "clock_samples", 0, 1024);

    u32 id = 0;
    for (u32 different = 0; different < 2; ++different)
    for (u32 spin = 0; spin < 2; ++spin) {
        if (spin && (!different || !config.include_spin)) continue;
        for (u32 mode = 0; mode < 2; ++mode)
        for (u32 p = 0; p < 5; ++p) {
            if (!(config.payload_mask & (1u << p))) continue;
            Case item{++id, mode, kPayloads[p], different, spin, backend.server_cpu(different), config.window};
            if (!backend.supports_placement(different) || (different && item.server_cpu == backend.client_cpu()) || (spin && !backend.supports_polling())) {
                libk::str::Buffer line(state.line);
                line.format("IPC_BENCH {\"record\":\"skip\",\"case_id\":%u,\"reason\":\"placement_or_policy_unavailable\"}", id);
                emit(backend, line);
                continue;
            }
            backend.polling(false);
            control(state.outgoing, Configure, item.id);
            state.outgoing.size = sizeof(item);
            (void)libk::copy_exact(libk::as_bytes(state.outgoing.payload, sizeof(item)), libk::as_bytes(&item, sizeof(item)));
            backend.send(state.outgoing); backend.receive(state.incoming);
            u32 observed_cpu = ~0u;
            (void)libk::copy_exact(libk::as_bytes(&observed_cpu, sizeof(observed_cpu)),
                                  libk::as_bytes(state.incoming.payload, sizeof(observed_cpu)));
            require(backend, state.incoming.kind == Ready && state.incoming.sequence == id &&
                    state.incoming.size == sizeof(observed_cpu) && observed_cpu == item.server_cpu,
                    "server readiness/placement");
            backend.polling(spin != 0);
            u64 sequence = 0;
            transactions(backend, state, item, config.warmup, sequence);
            {
                libk::str::Buffer line(state.line);
                line.format("IPC_BENCH {\"record\":\"case\",\"case_id\":%u,\"mode\":\"%s\","
                            "\"placement\":\"%s\",\"wait_policy\":\"%s\",\"payload_bytes\":%u,\"server_cpu\":%u}",
                            id, mode == Rpc ? "rpc" : "stream", different ? "different_cpu" : "same_cpu",
                            spin ? "spin" : "blocking", item.bytes, observed_cpu);
                emit(backend, line);
            }
            for (u32 round = 0; round < config.rounds; ++round) {
                const auto before = backend.counters();
                const u64 start = backend.now();
                transactions(backend, state, item, config.iterations, sequence);
                const u64 end = backend.now();
                const auto after = backend.counters();
                require(backend, end > start && backend.cpu() == backend.client_cpu(), "timing/client placement");
                libk::str::Buffer line(state.line);
                line.format("IPC_BENCH {\"record\":\"round\",\"case_id\":%u,\"round\":%u,\"operations\":%u,"
                            "\"elapsed_ns\":%llu,\"counters\":[", id, round, config.iterations, static_cast<ull>(end - start));
                for (u32 counter = 0; counter < CounterCount; ++counter) {
                    if (counter) line.append(',');
                    if (before.values[counter] == kUnavailable || after.values[counter] == kUnavailable) line.append("null");
                    else {
                        require(backend, after.values[counter] >= before.values[counter], "counter regressed");
                        line.format("%llu", static_cast<ull>(after.values[counter] - before.values[counter]));
                    }
                }
                line.append("]}"); emit(backend, line);
            }
            // A separate instrumented distribution; two clock syscalls per RPC
            // are included, never silently subtracted or called one-way latency.
            if (mode == Rpc) {
                for (u32 i = 0; i < config.samples; ++i) {
                    const u64 start = backend.now();
                    rpc(backend, state, item, sequence);
                    const u64 end = backend.now();
                    require(backend, end >= start, "latency clock regressed");
                    state.samples[i] = end - start;
                }
                emit_samples(backend, state, "latency_samples", id, config.samples);
            }
        }
    }
    backend.polling(false);
    control(state.outgoing, Finish);
    backend.send(state.outgoing); backend.receive(state.incoming);
    require(backend, state.incoming.kind == Closed, "peer shutdown acknowledgement");
    backend.write("IPC_BENCH_DONE\n", 15);
}
} // namespace ipc_bench
