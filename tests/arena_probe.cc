// tests/arena_probe.cc — host test for the arena allocator (mochi/arena.h).
//
// The arena is the libc-allocation replacement: it must never grow, never
// overrun, and refuse a request it cannot satisfy. ASan/UBSan catch a bad bump.
#include "mochi/arena.h"

static int failures = 0;
static void check(bool cond) { if (!cond) ++failures; }

struct Pair {
    unsigned int x;
    unsigned int y;
};

int main() {
    alignas(16) unsigned char region[256];
    mochi::Arena a(region, sizeof(region));

    check(a.valid());
    check(a.used() == 0 && a.remaining() == 256);

    // First allocation starts at offset 0; an aligned request bumps to the
    // alignment and never overlaps the previous block.
    void* p = a.alloc(10, 1);
    check(p == region);
    void* q = a.alloc(10, 16);
    check(reinterpret_cast<unsigned long>(q) % 16 == 0);
    check(static_cast<unsigned char*>(q) >= static_cast<unsigned char*>(p) + 10);

    // Non-power-of-two alignment is refused.
    a.reset();
    check(a.alloc(4, 3) == nullptr);

    // Exhaustion returns nullptr rather than overrunning.
    a.reset();
    check(a.alloc(257, 1) == nullptr);
    void* fit = a.alloc(256, 1);
    check(fit == region && a.remaining() == 0);
    check(a.alloc(1, 1) == nullptr);

    // Typed allocation, and a count whose size multiplication would overflow.
    a.reset();
    Pair* pr = a.alloc<Pair>();
    check(pr != nullptr && a.used() >= sizeof(Pair));
    check(a.alloc<Pair>(~static_cast<mx_size_t>(0)) == nullptr);
    check(a.alloc<Pair>(0) == nullptr);

    // An invalid (unattached) arena hands out nothing.
    mochi::Arena empty;
    check(!empty.valid() && empty.alloc(1, 1) == nullptr && empty.remaining() == 0);

    check(a.high_water() > 0 && a.high_water() <= a.size());
    mochi::Arena unaligned(region + 1, sizeof(region) - 1);
    auto* aligned = unaligned.alloc<Pair>(1, 1);
    check(aligned && reinterpret_cast<mx_u64_t>(aligned) % alignof(Pair) == 0);
    const auto before = unaligned.used();
    check(unaligned.alloc(300, 16) == nullptr && unaligned.used() == before);
    libk::fill(libk::as_bytes(region), 0xff);
    a.reset();
    check(a.alloc_zeroed(sizeof(region)) == region && libk::is_zero(libk::as_bytes(region)));
    mochi::Arena no_storage(nullptr, 64);
    check(no_storage.remaining() == 0);
    mochi::Arena wrap(reinterpret_cast<void*>(~mx_u64_t{0} - 7), 32);
    check(wrap.alloc(16, 1) == nullptr && wrap.used() == 0);
    mochi::Arena reserved(region, 64, sizeof(region));
    auto* tail = static_cast<unsigned char*>(reserved.alloc(16, 1));
    tail[0] = 73;
    check(reserved.realloc(tail, 16, 32, 1) == tail && reserved.used() == 32 && tail[0] == 73);
    check(reserved.resize(128) && reserved.capacity() == sizeof(region));
    check(!reserved.resize(16) && !reserved.resize(257));
    auto* other = reserved.alloc(16, 1);
    auto* replacement = static_cast<unsigned char*>(reserved.realloc(tail, 32, 48, 1));
    check(replacement && replacement != tail && replacement[0] == 73 && other != nullptr);
    const auto cursor = reserved.used();
    check(reserved.realloc(replacement, 48, 257, 1) == nullptr && reserved.used() == cursor);
    check(reserved.realloc(replacement, 48, 0, 1) == nullptr && reserved.used() == cursor - 48);
    return failures;
}
