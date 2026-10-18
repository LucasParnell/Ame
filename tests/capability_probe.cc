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

    p = new Payload{456};
    root = object::create(p, destroy);
    readers.clear();
    for (unsigned i=0;i<8;++i) readers.emplace_back([&] {
        for (unsigned n=0;n<10000;++n) {
            object::Ref<Payload> cached(p,root);
            object::Ref<Payload> copy(cached);
            assert(copy->value==456);
        }
    });
    for (auto& reader:readers) reader.join();
    assert(destroyed==3);
    object::release(root);
    assert(destroyed==4 && object::live_count()==0);

    // Static targets remain valid when their last registry reference disappears.
    // Race acquisition against final unlink/free: a zero-count retiring record
    // may fail acquisition, but must never be resurrected or freed twice.
    Payload permanent{789};
    readers.clear();
    for (unsigned i=0;i<8;++i) readers.emplace_back([&] {
        for (unsigned n=0;n<10000;++n) {
            object::Ref<Payload> borrowed(&permanent);
            if (borrowed) assert(borrowed->value==789);
        }
    });
    for (auto& reader:readers) reader.join();
    assert(destroyed==4 && object::live_count()==0);

    // Rotate through a full table with sparse holes on both sides of next_free.
    // Metadata must agree with live entries after allocation, targeted revoke,
    // duplication, clear, and generation reuse.
    auto* cached=object::create(&permanent,nullptr);
    CapHandle handles[kCapTableSize]{};
    for (unsigned i=0;i<kCapTableSize;++i)
        handles[i]=cap::alloc(owner,CAP_ARENA,&permanent,RIGHT_READ|RIGHT_DUP,cached);
    assert(owner.used_count==kCapTableSize && !cap::reserve_slots(owner,1));
    for (unsigned i:{0u,63u,64u,511u,1023u}) cap::revoke(owner,handles[i]);
    assert(owner.used_count==kCapTableSize-5 && cap::reserve_slots(owner,5));
    assert(!cap::reserve_slots(owner,6));
    for (unsigned i=0;i<5;++i) assert(cap::dup(owner,handles[1],RIGHT_READ));
    assert(owner.used_count==kCapTableSize);
    assert(!cap::dup(owner,handles[1],RIGHT_READ));
    for (unsigned i:{0u,63u,64u,511u,1023u}) assert(!cap::lookup_any(owner,handles[i]));
    cap::revoke_target(owner,&permanent);
    assert(!owner.used_count && cap::reserve_slots(owner,kCapTableSize));
    for (auto word:owner.occupied) assert(!word);
    object::release(cached);
    assert(object::live_count()==0);

    // Final callbacks run outside the table guard, even during batched clear.
    struct Reentrant { CapTable* table; CapHandle allocated; } reentrant{&owner,0};
    auto* callback=object::create(&reentrant,[](void* target) {
        auto* r=static_cast<Reentrant*>(target);
        r->allocated=cap::alloc(*r->table,CAP_ARENA,&destroyed,RIGHT_READ);
    });
    assert(cap::alloc(owner,CAP_ARENA,&reentrant,RIGHT_READ,callback));
    object::release(callback);
    cap::clear(owner);
    cap::clear(owner); // a callback may populate a slot already passed by clear
    assert(!owner.used_count && object::live_count()==0 && reentrant.allocated);
    std::puts("capability lifetime, concurrent revoke and exhaustion passed");
}
