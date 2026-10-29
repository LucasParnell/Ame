#include <cassert>
#include <string>
#include "mochi/stdout.h"

struct Storage {
    MxRingPage page;
    MxRingSlot slots[2];
} storage{};
static std::string received;
static void drain() {
    MxRingSlot slot{};
    while (ring_pop(&storage.page, 1, &slot))
        received.append(reinterpret_cast<const char*>(slot.payload), slot.length);
}
int main() {
    mochi::appio::sink() = {{&storage.page, 1, 1}, true};
    const std::string data(4096, 'x');
    host_on_yield = [] {
        if (host_yields > 1100)
            drain();
    };
    assert(mochi::appio::write(data.data(), data.size()) == data.size());
    drain();
    assert(host_yields > 1024 && received == data);
    // The optional writer still reports a short prefix without waiting.
    const auto before = host_yields;
    assert(mochi::appio::try_write(data.data(), data.size()) == 96);
    assert(host_yields == before);
}
