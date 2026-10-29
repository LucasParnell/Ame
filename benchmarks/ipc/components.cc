// Component diagnosis, deliberately separate from the two-process IPC contract.
// The same scalar library/workload functions run as an app or a native probe.
#include "workload.h"
#include "mochi/framed_ring.h"
#ifdef AME_IPC_COMPONENT_HOST
#include <chrono>
#include <cstdio>
#else
#include "mochi/app.h"
#include "mochi/state.h"
#endif
#ifndef AME_IPC_COMPONENT_ITERATIONS
#define AME_IPC_COMPONENT_ITERATIONS 8192
#endif

namespace {
using namespace ipc_bench;
struct ComponentState {
    State workload;
    alignas(64) RingPage page{};
    RingSlot slots[256]{};
    volatile u64 checksum = 0;
};
u64 now() {
#ifdef AME_IPC_COMPONENT_HOST
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#else
    return mx_clock_ns();
#endif
}
void output(const char* text, unsigned size) {
#ifdef AME_IPC_COMPONENT_HOST
    std::fwrite(text, 1, size, stdout);
#else
    mochi::app::write(text, size);
#endif
}
void barrier(const void* data) {
    asm volatile("" : : "r"(data) : "memory");
}
template <class Operation>
void measure(ComponentState& state, const char* component, u32 bytes, Operation operation) {
    constexpr u32 iterations = AME_IPC_COMPONENT_ITERATIONS;
    // Startup and code/data warming stay outside the measured interval.
    for (u32 i = 0; i < 64; ++i)
        operation(i);
    const auto start = now();
    for (u32 i = 0; i < iterations; ++i)
        operation(i);
    const auto end = now();
    libk::str::Buffer line(state.workload.line);
    line.format("IPC_COMPONENT {\"component\":\"%s\",\"payload_bytes\":%u,\"iterations\":%u,"
                "\"elapsed_ns\":%llu,\"checksum\":%llu}\n",
                component, bytes, iterations, static_cast<ull>(end - start),
                static_cast<ull>(state.checksum));
    output(line.data, line.length);
}
void run(ComponentState& state) {
    state.page.space.features = MX_RING_PACKED_FRAMES;
    mochi::Ring ring{&state.page, 8};
    ring.peer_polls = true;
    auto& source = state.workload.outgoing;
    auto& destination = state.workload.incoming;
    for (auto bytes : kPayloads) {
        measure(state, "generate", bytes, [&](u32 sequence) {
            prepare(source, Echo, sequence, bytes);
            barrier(&source);
        });
        prepare(source, Echo, 17, bytes);
        measure(state, "validate", bytes, [&](u32) {
            barrier(&source);
            state.checksum = state.checksum + valid_payload(source, 17, bytes);
        });
        measure(state, "copy", bytes, [&](u32) {
            (void)libk::copy_exact(libk::as_bytes(destination.payload, bytes),
                                   libk::as_bytes(source.payload, bytes));
            barrier(&destination);
        });
        measure(state, "framed_copy_roundtrip", bytes, [&](u32 sequence) {
            mochi::FrameWriter writer(Echo, sequence, source.payload, bytes);
            mochi::FrameReader reader(destination.payload, sizeof(destination.payload));
            const bool copied = writer.push(ring) == mochi::FrameProgress::Complete &&
                                reader.pop(ring) == mochi::FrameProgress::Complete;
            state.checksum = state.checksum + copied;
            barrier(&destination);
        });
    }
    constexpr char done[] = "IPC_COMPONENT_DONE\n";
    output(done, sizeof(done) - 1);
}
} // namespace
#ifdef AME_IPC_COMPONENT_HOST
int main() {
    static ComponentState state;
    run(state);
}
#else
extern "C" [[noreturn]] void _start() {
    (void)mochi::app::start();
    auto arena = mochi::make_state(0x20000000, 256 * 1024);
    auto* state = arena.alloc<ComponentState>(1);
    if (!state)
        mochi::app::exit(MX_ERR_NOMEM);
    run(*state);
    mochi::app::exit(0);
}
#endif
