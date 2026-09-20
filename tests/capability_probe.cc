#include "anko/cap.h"
#include "anko/mm.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

namespace mm {
void* slab_alloc(usize size, usize align) {
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    void* p = nullptr;
    return posix_memalign(&p, align, size) ? nullptr : p;
}
void slab_free(void* p, usize, usize) {
    std::free(p);
}
void free_page(PhysAddr) {
    assert(false);
}
} // namespace mm
[[noreturn]] void panic(const char* text) {
    std::fprintf(stderr, "%s\n", text);
    std::abort();
}
static std::atomic<unsigned> destroyed;
struct Payload {
    unsigned value;
};
static void destroy(void* p) {
    delete static_cast<Payload*>(p);
    ++destroyed;
}

int main() {
    CapTable owner{}, client{};
    auto* p = new Payload{42};
    auto* root = object::create(p, destroy);
    CapHandle h = cap::alloc(owner, CAP_ARENA, p, RIGHT_READ | RIGHT_MAP | RIGHT_DUP);
    object::release(root);
    assert(h);
    {
        auto borrow = cap::lookup(owner, h, CAP_ARENA);
        assert(borrow && static_cast<Payload*>(borrow->target)->value == 42);
        CapHandle shared = cap::alloc(client, CAP_ARENA, borrow->target, RIGHT_READ | RIGHT_MAP);
        cap::clear(owner);
        assert(!cap::lookup_any(owner, h));
        assert(!destroyed);
        cap::clear(client);
        assert(!destroyed); // the in-flight borrow is the final reference
        assert(shared);
    }
    assert(destroyed == 1 && object::live_count() == 0);

    p = new Payload{99};
    root = object::create(p, destroy);
    h = cap::alloc(owner, CAP_ARENA, p, RIGHT_READ | RIGHT_DUP);
    object::release(root);
    std::atomic<bool> go{false};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!go.load()) {
            }
            for (unsigned n = 0; n < 10000; ++n) {
                auto ref = cap::lookup_any(owner, h);
                if (ref) {
                    assert(static_cast<Payload*>(ref->target)->value == 99);
                }
            }
        });
    }
    go = true;
    cap::revoke(owner, h);
    for (auto& reader : readers) {
        reader.join();
    }
    assert(destroyed == 2 && object::live_count() == 0);

    p = new Payload{123};
    root = object::create(p, destroy);
    for (unsigned i = 0; i < kCapTableSize; ++i) {
        assert(cap::alloc(owner, CAP_ARENA, p, RIGHT_READ));
    }
    assert(!cap::alloc(owner, CAP_ARENA, p, RIGHT_READ));
    cap::clear(owner);
    object::release(root);
    assert(destroyed == 3 && object::live_count() == 0);
    std::puts("capability lifetime, concurrent revoke and exhaustion passed");
}
