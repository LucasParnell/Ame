#include "mochi/heap.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <random>
#include <vector>
struct Provider { unsigned calls; };
static mx_u64_t grow(void* p, mx_u64_t wanted) { ++static_cast<Provider*>(p)->calls; return wanted; }
int main(int argc, char** argv) {
    constexpr unsigned limit = 1048576;
    auto* memory = static_cast<unsigned char*>(std::aligned_alloc(4096, limit)); assert(memory);
    mochi::memory::Heap heap; Provider provider{};
    assert(heap.initialize(memory, 4096, limit, &provider, grow));
    auto* first = heap.allocate(123); assert(first); std::memset(first, 0x72, 123);
    if (argc > 1) {
        if (!std::strcmp(argv[1], "stale")) {
            assert(heap.release(first)); static_cast<unsigned char*>(first)[0] = 1;
            for (int i = 0; i < 32; ++i) { auto* p = heap.allocate(123); assert(p); assert(heap.release(p)); }
        } else if (!std::strcmp(argv[1], "links")) {
            // The remainder after the first 176-byte block is on a free bin.
            auto* header = memory + 176;
            *reinterpret_cast<mx_u32_t*>(header + 28) = 0xfffffff0;
            (void)heap.allocate(3000);
        }
        return 2;
    }
    assert(heap.reallocate(first, 2048) == first);
    for (unsigned i = 0; i < 123; ++i) assert(static_cast<unsigned char*>(first)[i] == 0x72);
    assert(heap.release(first)); assert(!heap.release(first));
    struct Entry { void* p = nullptr; unsigned size = 0, value = 0; };
    std::vector<Entry> entries(256); std::mt19937 random(741);
    for (unsigned i = 0; i < 40000; ++i) {
        auto& entry = entries[random() % entries.size()];
        if (entry.p) {
            auto* bytes = static_cast<unsigned char*>(entry.p);
            for (unsigned j = 0; j < entry.size; ++j) assert(bytes[j] == entry.value);
            if (random() & 1) {
                const auto size = random() % 4096;
                auto* replacement = heap.reallocate(entry.p, size);
                if (replacement || !size) {
                    for (unsigned j = 0; j < size && j < entry.size; ++j)
                        assert(static_cast<unsigned char*>(replacement)[j] == entry.value);
                    entry.p = replacement; entry.size = size;
                    if (replacement) std::memset(replacement, entry.value, size);
                }
            } else { assert(heap.release(entry.p)); entry.p = nullptr; }
        } else {
            entry.size = random() % 2048; entry.value = random() % 256;
            entry.p = (random() & 1) ? heap.allocate(entry.size) : heap.allocate_aligned(256, entry.size);
            if (entry.p) std::memset(entry.p, entry.value, entry.size);
        }
    }
    for (auto& entry : entries) if (entry.p) assert(heap.release(entry.p));
    assert(!heap.live_bytes());
    assert(!heap.allocate_aligned(~mx_u64_t{0} - 15, 12));
    auto* p = heap.allocate(128); assert(p); auto calls = provider.calls;
    for (unsigned i = 0; i < 10000; ++i) { auto* q = heap.allocate(64); assert(q); assert(heap.release(q)); }
    assert(provider.calls == calls); assert(heap.release(p));
    std::free(memory); std::puts("mochi heap: randomized realloc, alignment, quarantine and local fast path passed");
}
