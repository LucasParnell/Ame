// tests/irq_probe.cc — host regression for the IRQ → doorbell binding table.
//
// Compiles kernel/drivers/gic/irq_table.cc against a fake interrupt controller
// and a stub doorbell notifier, then exercises masking-at-entry, bind/ack/
// unbind semantics, level-line pause/re-arm, owner-scoped teardown, and the
// delivery path (including the boot selftest routine).

#include "anko/irq.h"
#include "anko/ipc.h"
#include "anko/object.h"
#include "anko/mm.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// --- kernel memory stubs ----------------------------------------------------
namespace mm {
void* slab_alloc(usize size, usize align) {
    if (align < sizeof(void*)) align = sizeof(void*);
    void* p = nullptr;
    return posix_memalign(&p, align, size) ? nullptr : p;
}
void slab_free(void* p, usize, usize) { std::free(p); }
} // namespace mm

[[noreturn]] void panic(const char* text) {
    std::fprintf(stderr, "probe panic: %s\n", text);
    std::abort();
}

// --- fake GICv2 SPI state ---------------------------------------------------
namespace {
constexpr u32 kMaxIrq = 1024;
bool g_pending[kMaxIrq];
bool g_enabled[kMaxIrq];
bool g_active[kMaxIrq];
u32 g_trigger[kMaxIrq];
} // namespace

namespace drivers::gic {
void spi_configure(u32 irq_id, u32 trigger, u32) {
    if (irq_id < kMaxIrq) g_trigger[irq_id] = trigger;
}
void spi_enable(u32 irq_id) { if (irq_id < kMaxIrq) g_enabled[irq_id] = true; }
void spi_disable(u32 irq_id) { if (irq_id < kMaxIrq) g_enabled[irq_id] = false; }
void spi_set_pending(u32 irq_id) { if (irq_id < kMaxIrq) g_pending[irq_id] = true; }
void spi_clear_pending(u32 irq_id) { if (irq_id < kMaxIrq) g_pending[irq_id] = false; }
u32 iar_id() {
    // Mirrors GICv2: IAR consumes the highest pending enabled interrupt.
    for (u32 id = kMaxIrq - 1; id >= 32; --id) {
        if (g_pending[id] && g_enabled[id]) {
            g_pending[id] = false;
            g_active[id] = true;
            return id;
        }
    }
    return 1023;
}
void eoi(u32 irq_id) { if (irq_id < kMaxIrq) g_active[irq_id] = false; }
} // namespace drivers::gic

// --- doorbell notify stub ---------------------------------------------------
namespace ipc {
SysError notify_object(Doorbell* db) {
    if (!db) return E_NOTFOUND;
    __atomic_add_fetch(&db->counter, 1, __ATOMIC_RELAXED);
    return E_OK;
}
} // namespace ipc

namespace {
int g_destroyed = 0;

void destroy_db(void* target) {
    ++g_destroyed;
    mm::slab_free(target, sizeof(Doorbell), alignof(Doorbell));
}

Doorbell* new_db() {
    auto* db = static_cast<Doorbell*>(mm::slab_alloc(sizeof(Doorbell), alignof(Doorbell)));
    std::memset(db, 0, sizeof(*db));
    return db;
}

u64 counter_of(Doorbell* db) {
    return __atomic_load_n(&db->counter, __ATOMIC_RELAXED);
}
} // namespace

int main() {
    // (1) Masking-at-entry: the index always lands in range; the range check
    // rejects the sentinel, PPIs/SGIs, and out-of-range ids.
    assert(irq::irq_index(0xFFFF'FFFFu) == 255);
    assert(irq::irq_index(32) == 32);
    assert(irq::irq_index(300) == 44);
    assert(!irq::irq_bindable(0xFFFF'FFFFu));
    assert(!irq::irq_bindable(0) && !irq::irq_bindable(30) && !irq::irq_bindable(31));
    assert(irq::irq_bindable(32) && irq::irq_bindable(255) && !irq::irq_bindable(256));

    Doorbell* db = new_db();
    object::Record* rec = object::create(db, destroy_db);
    assert(rec);

    // (2) A sentinel line is rejected and never enters the table.
    IrqLine bad{};
    bad.irq_id = 0xFFFF'FFFFu;
    assert(irq::bind(&bad, db, 1, 0) == E_INVALID);
    assert(irq::binding_count() == 0);

    // (3) Bind/config/enable, idempotent rebind, and E_BUSY on a second doorbell.
    IrqLine line{};
    line.irq_id = 32;
    line.trigger = 1; // edge
    assert(irq::bind(&line, db, 1, 0) == E_OK);
    assert(irq::binding_count() == 1 && g_enabled[32]);
    assert(irq::bind(&line, db, 1, 0) == E_OK && irq::binding_count() == 1);
    Doorbell* db2 = new_db();
    object::Record* rec2 = object::create(db2, destroy_db);
    assert(irq::bind(&line, db2, 1, 0) == E_BUSY);
    assert(irq::bind(&line, db, 1, /*flags=*/1) == E_INVALID); // reserved flags

    // (4) Delivery notifies the bound doorbell; PPIs are never redirected.
    const u64 d0 = irq::delivered_count();
    irq::deliver(32);
    assert(irq::delivered_count() == d0 + 1 && counter_of(db) == 1);
    irq::deliver(30); // timer PPI
    assert(irq::delivered_count() == d0 + 1 && counter_of(db) == 1);

    // (5) Only the binder may tear the line down; teardown disables the SPI and
    // delivery afterwards is a no-op.
    irq::unbind_line(&line, /*owner=*/999);
    assert(irq::binding_count() == 1 && g_enabled[32]);
    irq::unbind_line(&line, /*owner=*/1);
    assert(irq::binding_count() == 0 && !g_enabled[32]);
    const u64 d1 = irq::delivered_count();
    irq::deliver(32);
    assert(irq::delivered_count() == d1);

    // (6) Level lines pause on delivery until acked, then re-arm.
    IrqLine lvl{};
    lvl.irq_id = 40;
    lvl.trigger = 0; // level
    assert(irq::bind(&lvl, db, 5, 0) == E_OK);
    irq::deliver(40);
    assert(!g_enabled[40]); // paused so the source cannot storm the CPU
    assert(irq::ack(&lvl, /*owner=*/5) == E_OK);
    assert(g_enabled[40]); // re-armed after the driver serviced it
    assert(irq::ack(&lvl, /*owner=*/777) == E_PERM); // wrong owner
    irq::unbind_line(&lvl, 5);

    // (7) Owner-scoped teardown removes exactly one process's lines.
    IrqLine a{}, b{}, c{};
    a.irq_id = 50; b.irq_id = 51; c.irq_id = 52;
    assert(irq::bind(&a, db, 7, 0) == E_OK);
    assert(irq::bind(&b, db, 7, 0) == E_OK);
    assert(irq::bind(&c, db, 8, 0) == E_OK);
    assert(irq::binding_count() == 3);
    irq::unbind_owner(7);
    assert(irq::binding_count() == 1);
    assert(!g_enabled[50] && !g_enabled[51] && g_enabled[52]);
    irq::unbind_owner(8);
    assert(irq::binding_count() == 0);

    // (8) The boot selftest drives raise → IAR → deliver → doorbell end to end.
    assert(irq::run_selftest() == 1);
    assert(irq::selftest_status() == 1);

    // Creator references are the last ones; both doorbells are destroyed.
    object::release(rec);
    object::release(rec2);
    assert(g_destroyed == 2);
    assert(object::live_count() == 0);

    std::printf("irq binding table probe ok\n");
    return 0;
}
