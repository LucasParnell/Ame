// Exercise observer rights and revocation using production capability tables.
#include "anko/platform_control.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
namespace mm {
void* slab_alloc(usize size, usize align) {
    if (align < sizeof(void*)) align = sizeof(void*);
    void* pointer = nullptr;
    return posix_memalign(&pointer, align, size) ? nullptr : pointer;
}
void slab_free(void* pointer, usize, usize) { std::free(pointer); }
}
namespace libk {
void* memset(void* target, int value, usize size) { return std::memset(target, value, size); }
}
[[noreturn]] void panic(const char*) { std::abort(); }
namespace platform {
static Control health{Domain::Health, 0, 0}, system{Domain::System, 0, 0};
Control* board_control(Domain domain) { return domain == Domain::Health ? &health : &system; }
}
int main() {
    CapTable table{}, stranger{};
    auto* health = platform::board_control(platform::Domain::Health);
    const auto observer = cap::alloc(table, CAP_PLATFORM, health, RIGHT_READ);
    const auto write_only = cap::alloc(table, CAP_PLATFORM, health, RIGHT_WRITE);
    const auto wrong_type = cap::alloc(table, CAP_ARENA, health, RIGHT_READ);
    const auto wrong_domain = cap::alloc(table, CAP_PLATFORM,
        platform::board_control(platform::Domain::System), RIGHT_READ);
    assert(platform::can_observe_health(table, observer));
    assert(!platform::can_observe_health(stranger, observer));
    assert(!platform::can_observe_health(table, 0));
    assert(!platform::can_observe_health(table, write_only));
    assert(!platform::can_observe_health(table, wrong_type));
    assert(!platform::can_observe_health(table, wrong_domain));
    auto entry = cap::lookup(table, observer, CAP_PLATFORM);
    assert(entry && !(entry->rights_mask & (RIGHT_WRITE | RIGHT_DUP)));
    cap::revoke(table, observer);
    assert(!platform::can_observe_health(table, observer));
    std::puts("Health observer authority probe ok");
}
