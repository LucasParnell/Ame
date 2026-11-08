#include "anko/amm.h"
#include "anko/adma.h"
#include "anko/cap.h"
#include "anko/smp.h"
#include "anko/spinlock.h"
#include "libk/string.h"

namespace amm {
namespace {
constexpr usize kPages = kMaxUserArenaPages;
constexpr u32 kMaxAliases = 64;
constexpr u32 kMaxArenas = 256;
struct Page {
    PhysAddr pa; u64 owner; u32 references;
    Page* retired_next = nullptr;
    bool published = false;
};
struct Mapping {
    Mapping* next;
    object::Record* lifetime;
    VmSpace space;
    VirtAddr va;
    u32 rights;
    u64* slots[kPages];
    bool enabled[kPages];
};
struct CopyRound {
    Page* sources[kPages];
    object::Record* lifetime;
    adma::Copy transfer;
    usize next, count;
    bool running;
};
kernel::SpinLock lock{};
Backing* arenas;
u32 arena_count;
u64 activity_epoch;
using Guard = kernel::LockGuard;
Page* retired_pages;
Page* retired_tail;
Page* ready_pages;
u32 reclaim_needed;
bool in_grace;
void free_page_record(Page* page) {
    mm::free_page_for_identity(page->owner, page->pa);
    mm::deallocate<Page>(page);
}
void drain_retired() {
    if (retired_pages) {
        retired_tail->retired_next = ready_pages;
        ready_pages = retired_pages;
        retired_pages = retired_tail = nullptr;
    }
}

struct Rendezvous {
    bool ready;
    Rendezvous() : ready(smp::rendezvous()) { if (ready) in_grace = true; }
    ~Rendezvous() {
        if (ready) { drain_retired(); in_grace = false; smp::resume(); }
    }
};

u64 identity(const Arena& a) { return (u64{a.owner_generation} << 32) | a.owner_pid; }
bool rounded(u64 bytes, u64* result, bool allow_zero = false) {
    if ((!bytes && !allow_zero) || bytes > kPages * PAGE_SIZE) return false;
    *result = (bytes + PAGE_SIZE - 1) & ~(u64{PAGE_SIZE - 1});
    return true;
}
Page* allocate(u64 owner) {
    auto* page = static_cast<Page*>(mm::allocate<Page>());
    if (!page) return nullptr;
    PhysAddr pa = mm::alloc_page_for_identity(owner);
    // These pages already crossed a grace period. Reclaim on allocation
    // pressure without stopping CPUs or copying any live arena contents.
    while (!pa && ready_pages) {
        auto* ready = ready_pages; ready_pages = ready->retired_next;
        free_page_record(ready);
        pa = mm::alloc_page_for_identity(owner);
    }
    if (!pa) { mm::deallocate<Page>(page); return nullptr; }
    libk::memset(reinterpret_cast<void*>(pa), 0, PAGE_SIZE);
    *page = {pa, owner, 1};
    return page;
}
void release(Page* page) {
    if (page && !--page->references) {
        if (!page->published) free_page_record(page);
        else {
            if (in_grace) { page->retired_next = ready_pages; ready_pages = page; }
            else {
                page->retired_next = retired_pages; retired_pages = page;
                if (!retired_tail) retired_tail = page;
            }
            __atomic_store_n(&reclaim_needed, 1u, __ATOMIC_RELEASE);
        }
    }
}
} // namespace

struct Backing {
    Backing* next;
    Arena* arena;
    Mapping* maps;
    object::Record* pin_lifetime;
    CopyRound* round;
    Page* pages[kPages];
    Page* pending[kPages];
    bool dirty[kPages];
    bool accessed[kPages];
    // Temperature measures first access/write faults in a tracking round;
    // it is a conservative estimate, not a hardware access-frequency count.
    u32 heat[kPages];
    u64 capacity, cage_base, cage_size, type_id;
    u64 generation, retired_generation, copied_bytes, zero_copy_changes, touched;
    u32 state, pins, external_pins, aliases, policy;
    bool sampling;
};

namespace {
// Address/type assignments survive arena destruction and physical decommit.
// Only address-space retirement releases these tombstones.
struct Cage { Cage* next; u64 owner, root, base, size, type; };
Cage* cages;
bool cage_history_allowed(u64 owner, VmSpace space, u64 base, u64 size, u64 type) {
    for (auto* c = cages; c; c = c->next) {
        if (!(owner && c->owner == owner) && !(space.pgd_phys && c->root == space.pgd_phys)) continue;
        if (type && c->type == type) {
            if (c->base != base || c->size != size) return false;
        } else if (base < c->base + c->size && c->base < base + size) return false;
    }
    return true;
}
bool remember_cage(u64 owner, VmSpace space, u64 base, u64 size, u64 type) {
    if (!type) return true;
    usize count = 0;
    for (auto* c = cages; c; c = c->next) {
        if (c->owner != owner || c->root != space.pgd_phys) continue;
        if (c->base == base && c->size == size && c->type == type) return true;
        ++count;
    }
    if (count == 256) return false;
    auto* c = mm::allocate<Cage>();
    if (!c) return false;
    *c = {cages, owner, space.pgd_phys, base, size, type}; cages = c; return true;
}
void forget_cages(u64 root, u64 owner = 0) {
    for (auto** link = &cages; *link;) {
        auto* c = *link;
        if ((root && c->root == root) || (owner && c->owner == owner)) {
            *link = c->next; mm::deallocate<Cage>(c);
        } else link = &c->next;
    }
}
bool cage_valid(const Backing& b, VirtAddr va) {
    return va >= b.cage_base && va - b.cage_base <= b.cage_size &&
           b.capacity <= b.cage_size - (va - b.cage_base) && !(va & (PAGE_SIZE - 1));
}
bool type_cage_allowed(u64 owner, u64 base, u64 size, u64 type, VmSpace space = {}) {
    if (!type) return true; // untyped bounds do not reserve a type cage
    if (!cage_history_allowed(owner, space, base, size, type)) return false;
    for (auto* b = arenas; b; b = b->next) {
        bool present = identity(*b->arena) == owner;
        for (auto* m = b->maps; space.pgd_phys && m; m = m->next)
            present |= m->space.pgd_phys == space.pgd_phys;
        if (!present) continue;
        if (!b->type_id) {
            for (auto* m = b->maps; space.pgd_phys && m; m = m->next)
                if (m->space.pgd_phys == space.pgd_phys && base < m->va + b->capacity &&
                    m->va < base + size) return false;
            continue;
        }
        if (type && type == b->type_id &&
            (base != b->cage_base || size != b->cage_size)) return false;
        if (type != b->type_id && base < b->cage_base + b->cage_size && b->cage_base < base + size)
            return false;
    }
    return true;
}
bool mapping_type_allowed(const Backing& candidate, VmSpace space, u64 owner, VirtAddr va) {
    if (!cage_history_allowed(owner, space, candidate.type_id ? candidate.cage_base : va,
                              candidate.type_id ? candidate.cage_size : candidate.capacity, candidate.type_id)) return false;
    for (auto* b = arenas; b; b = b->next) {
        bool present = owner && identity(*b->arena) == owner;
        for (auto* m = b->maps; m; m = m->next) present |= m->space.pgd_phys == space.pgd_phys;
        if (!present) continue;
        if (!candidate.type_id) {
            if (b->type_id && va < b->cage_base + b->cage_size &&
                b->cage_base < va + candidate.capacity) return false;
            continue;
        }
        if (!b->type_id) {
            for (auto* m = b->maps; m; m = m->next)
                if (m->space.pgd_phys == space.pgd_phys && candidate.cage_base < m->va + b->capacity &&
                    m->va < candidate.cage_base + candidate.cage_size) return false;
            continue;
        }
        if (candidate.type_id && candidate.type_id == b->type_id &&
            (candidate.cage_base != b->cage_base || candidate.cage_size != b->cage_size)) return false;
        if (candidate.type_id != b->type_id && candidate.cage_base < b->cage_base + b->cage_size &&
            b->cage_base < candidate.cage_base + candidate.cage_size) return false;
    }
    return true;
}
bool overlaps(VmSpace space, VirtAddr va, u64 size, Mapping* except = nullptr) {
    if (!size || va > ~u64{0} - size) return true;
    for (auto* b = arenas; b; b = b->next)
        for (auto* m = b->maps; m; m = m->next)
            if (m != except && m->space.pgd_phys == space.pgd_phys &&
                va < m->va + b->capacity && m->va < va + size) return true;
    return false;
}
Mapping* lookup(VmSpace space, VirtAddr va, Backing** backing = nullptr) {
    for (auto* b = arenas; b; b = b->next)
        for (auto* m = b->maps; m; m = m->next)
            if (m->space.pgd_phys == space.pgd_phys && va >= m->va && va - m->va < b->capacity) {
                if (backing) *backing = b;
                return m;
            }
    return nullptr;
}
void break_maps(Backing& b) {
    for (auto* m = b.maps; m; m = m->next)
        mm::vm_break_slots(m->space, m->slots, b.capacity / PAGE_SIZE);
}
void make_map(Backing& b, Mapping& m, usize first = 0) {
    for (usize i = first; i < b.arena->size / PAGE_SIZE; ++i) {
        if (!m.enabled[i]) continue;
        u32 rights = m.rights;
        if (b.pages[i]->references > 1 ||
            ((b.sampling || b.state == MX_ARENA_PRECOPY_STATE || b.state == MX_ARENA_DIRTY) &&
             !b.dirty[i]))
            rights &= ~RIGHT_WRITE;
        mm::vm_make_slot(m.slots[i], b.pages[i]->pa, rights, !b.sampling || b.accessed[i]);
        b.pages[i]->published = true;
    }
    mm::vm_sync_space(m.space);
}
void make_maps(Backing& b) {
    for (auto* m = b.maps; m; m = m->next) make_map(b, *m);
}
void touch(Backing& b) { b.touched = ++activity_epoch; }
void note_access(Backing& b, usize index) {
    if (!b.sampling || b.accessed[index]) return;
    b.accessed[index] = true;
    b.heat[index] = b.heat[index] > ~u32{0} - 8 ? ~u32{0} : b.heat[index] + 8;
    for (auto* m = b.maps; m; m = m->next)
        if (m->enabled[index]) mm::vm_note_access(m->space, m->slots[index]);
    touch(b);
}
void prune(VmSpace space, VirtAddr va, u64 size) {
    for (u64 offset = 0; offset < size; offset += PAGE_SIZE)
        mm::vm_prune_empty(space, va + offset);
}
SysError editable(Backing* b) {
    if (!b) return E_BADTYPE;
    return b->pins || b->external_pins || b->state != MX_ARENA_STABLE ? E_BUSY : E_OK;
}
void discard_pending(Backing& b) {
    adma::fence();
    for (usize i = 0; i < b.capacity / PAGE_SIZE; ++i) {
        release(b.pending[i]); b.pending[i] = nullptr; b.dirty[i] = false;
    }
}
// A round owns source references independently of the arena's current pages.
// Writers COW those immutable sources while ADMA runs without the manager lock.
object::Record* finish_round(Backing& b, bool failed) {
    auto* round = b.round;
    for (usize i = 0; i < round->count; ++i) {
        if (failed && i >= round->next && round->sources[i]) b.dirty[i] = true;
        release(round->sources[i]);
    }
    auto* lifetime = round->lifetime;
    b.round = nullptr;
    mm::deallocate<CopyRound>(round);
    if (failed) b.state = MX_ARENA_DIRTY;
    return lifetime;
}
SysError advance_round(Arena& a, usize budget, bool abort = false) {
    CopyRound* round;
    Backing* b;
    {
        Guard guard(lock);
        b = a.managed; round = b ? b->round : nullptr;
        if (!round) return E_OK;
        if (round->running) return E_BUSY;
        round->running = true;
    }
    SysError result = E_OK;
    usize copied = 0;
    if (abort) result = adma::cancel(round->transfer);
    else {
        for (; round->next < round->count;) {
            const usize i = round->next;
            if (!round->sources[i]) { ++round->next; continue; }
            if (copied == budget) { result = E_BUSY; break; }
            result = round->transfer.active ? adma::poll(round->transfer)
                : adma::start(round->transfer, b->pending[i]->pa, round->sources[i]->pa);
            if (result != E_OK) break;
            ++copied; ++round->next;
        }
    }
    object::Record* drop = nullptr;
    {
        Guard guard(lock);
        b->copied_bytes += copied * PAGE_SIZE;
        round->running = false;
        if (result != E_BUSY)
            drop = finish_round(*b, abort || result != E_OK);
        touch(*b);
    }
    object::release(drop);
    return result;
}
// Called under the manager lock. No user can change a COW page without taking
// this lock; a frozen rendezvous excludes direct and kernel accesses to it.
bool write_fault(Backing& b, Mapping& m, usize index) {
    if (!(m.rights & RIGHT_WRITE) || index >= b.arena->size / PAGE_SIZE || !m.enabled[index])
        return false;
    note_access(b, index);
    Page* replacement = nullptr;
    if (b.pages[index]->references > 1) {
        replacement = allocate(identity(*b.arena));
        if (!replacement) return false;
        adma::copy_page(replacement->pa, b.pages[index]->pa);
        adma::fence();
    }
    const bool migrating = b.state == MX_ARENA_PRECOPY_STATE || b.state == MX_ARENA_DIRTY;
    const bool tracking = b.sampling || migrating;
    PhysAddr current;
    if (!replacement && (!tracking || b.dirty[index]) &&
        mm::user_va_to_pa(m.space, m.va + index * PAGE_SIZE, RIGHT_WRITE, &current)) return true;
    // COW sources remain owned by another arena. Attribute-only dirty tracking
    // needs broadcast TLBI, but no CPU rendezvous: simultaneous write faults
    // must never wait for one another to acknowledge an IPI with IRQs masked.
    for (auto* alias = b.maps; alias; alias = alias->next)
        if (alias->enabled[index]) mm::vm_break_slots(alias->space, &alias->slots[index], 1);
    if (replacement) {
        auto* old = b.pages[index]; b.pages[index] = replacement;
        b.retired_generation = b.generation++;
        release(old); // another arena retains this immutable COW source
    }
    if (tracking) b.dirty[index] = true;
    if (migrating) b.state = MX_ARENA_DIRTY;
    if (!b.sampling && b.heat[index] != ~u32{0}) ++b.heat[index];
    touch(b);
    for (auto* alias = b.maps; alias; alias = alias->next) {
        if (!alias->enabled[index]) continue;
        u32 rights = alias->rights;
        if (b.pages[index]->references > 1 || (tracking && !b.dirty[index])) rights &= ~RIGHT_WRITE;
        mm::vm_make_slot(alias->slots[index], b.pages[index]->pa, rights, !b.sampling || b.accessed[index]);
        b.pages[index]->published = true; mm::vm_sync_space(alias->space);
    }
    return true;
}
} // namespace

SysError create(u32 pid, u32 generation, const MxArenaCreate& request, Arena** out, VmSpace owner_space) {
    *out = nullptr;
    u64 size, capacity;
    if (!rounded(request.size, &size, true) || !rounded(request.capacity, &capacity) || size > capacity ||
        request.reserved || (request.policy & ~MX_ARENA_COMPACT_IDLE) ||
        request.cage_base < 0x10000000 || request.cage_base >= 0x40000000 ||
        request.cage_size > 0x40000000 - request.cage_base || request.cage_size < capacity ||
        ((request.cage_base | request.cage_size) & (PAGE_SIZE - 1))) return E_INVALID;
    Guard guard(lock);
    if (arena_count == kMaxArenas) return E_OVERFLOW;
    if (!type_cage_allowed((u64{generation} << 32) | pid, request.cage_base,
                           request.cage_size, request.type_id, owner_space)) return E_POLICY;
    if (request.type_id && owner_space.pgd_phys &&
        !mm::vm_cage_available(owner_space, request.cage_base, request.cage_size)) return E_POLICY;
    auto* a = static_cast<Arena*>(mm::allocate<Arena>());
    auto* b = static_cast<Backing*>(mm::allocate<Backing>());
    if (!a || !b) {
        mm::deallocate<Arena>(a);
        mm::deallocate<Backing>(b);
        return E_NOMEM;
    }
    *a = {}; *b = {};
    a->managed = b; a->size = size; a->owner_pid = pid; a->owner_generation = generation;
    a->rights = RIGHT_READ | RIGHT_WRITE;
    b->arena = a; b->capacity = capacity; b->cage_base = request.cage_base;
    b->cage_size = request.cage_size; b->type_id = request.type_id; b->generation = 1;
    b->policy = request.policy;
    b->sampling = (request.policy & MX_ARENA_COMPACT_IDLE) != 0;
    const auto contiguous = mm::alloc_pages_contiguous_for_identity(identity(*a), capacity / PAGE_SIZE);
    for (usize i = 0; i < capacity / PAGE_SIZE; ++i) {
        if (contiguous) {
            b->pages[i] = static_cast<Page*>(mm::allocate<Page>());
            if (b->pages[i]) {
                *b->pages[i] = {contiguous + i * PAGE_SIZE, identity(*a), 1};
                libk::memset(reinterpret_cast<void*>(b->pages[i]->pa), 0, PAGE_SIZE);
            }
        } else b->pages[i] = allocate(identity(*a));
        if (!b->pages[i]) {
            if (contiguous)
                for (usize j = i; j < capacity / PAGE_SIZE; ++j)
                    mm::free_page_for_identity(identity(*a), contiguous + j * PAGE_SIZE);
            for (usize j = 0; j < i; ++j) release(b->pages[j]);
            mm::deallocate<Backing>(b);
            mm::deallocate<Arena>(a);
            return E_NOMEM;
        }
    }
    if (!remember_cage(identity(*a), owner_space, b->cage_base, b->cage_size, b->type_id)) {
        for (usize i = 0; i < capacity / PAGE_SIZE; ++i) release(b->pages[i]);
        mm::deallocate<Backing>(b); mm::deallocate<Arena>(a); return E_NOMEM;
    }
    b->next = arenas; arenas = b; ++arena_count; touch(*b);
    *out = a;
    return E_OK;
}

SysError adopt(u32 pid, u32 generation, VmSpace space, VirtAddr va, u64 size,
                Arena** out, object::Record** creator) {
    *out = nullptr; *creator = nullptr;
    if (!size || size > kPages * PAGE_SIZE || ((va | size) & (PAGE_SIZE - 1)) ||
        va < 0x10000000 || va >= 0x40000000 || size > 0x40000000 - va) return E_INVALID;
    Guard guard(lock);
    Backing* existing = nullptr;
    if (auto* m = lookup(space, va, &existing)) {
        if (m->va != va || existing->arena->size != size ||
            identity(*existing->arena) != ((u64{generation} << 32) | pid)) return E_PERM;
        *creator = object::acquire_existing(existing->arena); *out = existing->arena;
        return *creator ? E_OK : E_DEAD;
    }
    if (arena_count == kMaxArenas || overlaps(space, va, size)) return E_BUSY;
    auto* a = mm::allocate<Arena>(); auto* b = mm::allocate<Backing>();
    auto* m = mm::allocate<Mapping>();
    auto rollback = [&]() {
        if (b) for (auto* page : b->pages) if (page) mm::deallocate<Page>(page);
        mm::deallocate<Mapping>(m); mm::deallocate<Backing>(b); mm::deallocate<Arena>(a);
    };
    if (b) *b = {};
    if (!a || !b || !m) { rollback(); return E_NOMEM; }
    *a = {}; *m = {};
    a->managed = b; a->owner_pid = pid; a->owner_generation = generation;
    a->rights = RIGHT_READ | RIGHT_WRITE; a->size = size;
    b->arena = a; b->capacity = size; b->cage_base = 0x10000000;
    b->cage_size = 0x30000000; b->generation = 1;
    b->policy = MX_ARENA_COMPACT_IDLE; b->sampling = true;
    m->space = space; m->va = va; m->rights = RIGHT_READ | RIGHT_WRITE;
    for (usize i = 0; i < size / PAGE_SIZE; ++i) {
        b->pages[i] = mm::allocate<Page>();
        if (!b->pages[i]) { rollback(); return E_NOMEM; }
    }
    Rendezvous stop;
    if (!stop.ready) { rollback(); return E_BUSY; }
    for (usize i = 0; i < size / PAGE_SIZE; ++i) {
        PhysAddr pa;
        m->slots[i] = mm::vm_owned_slot(space, va + i * PAGE_SIZE, &pa);
        if (!m->slots[i]) { rollback(); return E_PERM; }
        *b->pages[i] = {pa, identity(*a), 1}; m->enabled[i] = true;
    }
    // VM ownership transfers exactly once, after every leaf and all metadata
    // have been validated. A failed import never frees a VM-owned frame.
    auto* record = object::create(a, destroy);
    if (!record) { rollback(); return E_NOMEM; }
    object::retain(record); m->lifetime = record;
    mm::vm_break_slots(space, m->slots, size / PAGE_SIZE);
    b->maps = m; b->aliases = 1; b->next = arenas; arenas = b; ++arena_count;
    make_maps(*b); touch(*b);
    *out = a; *creator = record;
    return E_OK;
}

void destroy(void* target) {
    auto* a = static_cast<Arena*>(target);
    Guard guard(lock);
    auto* b = a->managed;
    // Mapping references prevent the destructor while an alias is attached.
    Backing** link = &arenas;
    while (*link && *link != b) link = &(*link)->next;
    if (*link) { *link = b->next; --arena_count; }
    discard_pending(*b);
    b->state = MX_ARENA_RETIRED;
    for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i) release(b->pages[i]);
    mm::deallocate<Backing>(b);
    mm::deallocate<Arena>(a);
}

SysError map(Arena& a, object::Record* lifetime, VmSpace space, VirtAddr va, u32 rights, u64 owner) {
    Guard guard(lock);
    auto* b = a.managed;
    if (!b || !lifetime || !space.pgd_phys || !rights ||
        (rights & ~(RIGHT_READ | RIGHT_WRITE)) || !cage_valid(*b, va)) return E_INVALID;
    if (b->aliases == kMaxAliases) return E_OVERFLOW;
    if (!mapping_type_allowed(*b, space, owner, va)) return E_POLICY;
    if (b->type_id && !mm::vm_cage_available(space, b->cage_base, b->cage_size)) return E_POLICY;
    if (overlaps(space, va, b->capacity)) return E_BUSY;
    auto* m = static_cast<Mapping*>(mm::allocate<Mapping>());
    if (!m) return E_NOMEM;
    *m = {}; m->space = space; m->va = va; m->rights = rights; m->lifetime = lifetime;
    for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i) {
        m->slots[i] = mm::vm_prepare_slot(space, va + i * PAGE_SIZE);
        m->enabled[i] = true;
        if (!m->slots[i]) {
            mm::vm_drop_slots(space, m->slots, i);
            prune(space, va, (i + 1) * PAGE_SIZE);
            mm::deallocate<Mapping>(m); return E_BUSY;
        }
    }
    if (!remember_cage(owner, space, b->cage_base, b->cage_size, b->type_id)) {
        mm::vm_drop_slots(space, m->slots, b->capacity / PAGE_SIZE);
        prune(space, va, b->capacity); mm::deallocate<Mapping>(m); return E_NOMEM;
    }
    // Fresh invalid leaves have no old readers or translations to retire.
    // Publish under the metadata lock and synchronize the ASID once below.
    object::retain(lifetime);
    m->next = b->maps; b->maps = m; ++b->aliases;
    make_map(*b, *m); touch(*b);
    return E_OK;
}

bool reserved(VmSpace space, VirtAddr va, u64 size) {
    Guard guard(lock); return overlaps(space, va, size);
}

static bool map_wired_locked(u64 owner, VmSpace space, VirtAddr va, PhysAddr pa, u64 size,
                              u32 rights, bool device, bool owned_page) {
    if (!size || va > ~u64{0} - size) return false;
    if (!cage_history_allowed(owner, space, va, size, 0)) return false;
    for (auto* b = arenas; b; b = b->next) {
        if (!b->type_id || va >= b->cage_base + b->cage_size || b->cage_base >= va + size) continue;
        bool present = identity(*b->arena) == owner;
        for (auto* m = b->maps; m; m = m->next) present |= m->space.pgd_phys == space.pgd_phys;
        if (present) return false;
    }
    if (owned_page)
        return size == PAGE_SIZE && !device && mm::vm_map_owned(space, va, pa, rights);
    return device ? mm::vm_map_device_range(space, va, pa, size, rights)
                  : mm::vm_map_range(space, va, pa, size, rights);
}

bool map_wired(u64 owner, VmSpace space, VirtAddr va, PhysAddr pa, u64 size, u32 rights,
                bool device, bool owned_page) {
    Guard guard(lock);
    return map_wired_locked(owner, space, va, pa, size, rights, device, owned_page);
}

SysError map_shared_wired(u64 owner, VmSpace space, ArenaTable& table, Arena& arena,
                           object::Record* lifetime, VirtAddr va, u32 rights) {
    if (!lifetime || arena.managed || !arena.phys_base) return E_INVALID;
    Guard guard(lock);
    bool claimed = false;
    if (!mm::arena_claim_va(table, arena, va, arena.size, &claimed)) return E_OVERFLOW;
    if (!map_wired_locked(owner, space, va, arena.phys_base, arena.size, rights, false, false)) {
        if (claimed) mm::arena_release_va(table, arena, va);
        return E_INVALID;
    }
    // A racing unmap cannot remove the claim between PTE publication and the
    // pin. It takes the same manager lock through unmap_wired below.
    mm::arena_retain_mapping(table, va, lifetime);
    return E_OK;
}

SysError unmap_wired(VmSpace space, VirtAddr va, ArenaTable* table) {
    object::Record* drop = nullptr;
    {
        Guard guard(lock);
        Rendezvous stop; if (!stop.ready) return E_BUSY;
        if (!mm::vm_unmap_user(space, va)) return E_PERM;
        if (table) drop = mm::arena_unmap_page_detach(*table, va);
    }
    // Ring/arena destruction may release process references and reenter AMM.
    // All translations and metadata are gone before running those callbacks.
    object::release(drop);
    return E_OK;
}

SysError unmap_page(VmSpace space, VirtAddr va) {
    object::Record* drop = nullptr;
    {
        Guard guard(lock);
        Backing* b = nullptr;
        auto* m = lookup(space, va, &b);
        if (!m) return E_NOTFOUND;
        Rendezvous stop; if (!stop.ready) return E_BUSY;
        const usize i = (va - m->va) / PAGE_SIZE;
        mm::vm_break_slots(m->space, &m->slots[i], 1);
        m->enabled[i] = false;
        bool any = false;
        for (usize j = 0; j < b->capacity / PAGE_SIZE; ++j) any |= m->enabled[j];
        // A range can be explicitly detached by unmapping the entire reserved
        // capacity, including its currently uncommitted tail.
        if (!any) {
            auto** link = &b->maps;
            while (*link != m) link = &(*link)->next;
            *link = m->next; --b->aliases; drop = m->lifetime;
            mm::vm_drop_slots(m->space, m->slots, b->capacity / PAGE_SIZE);
            prune(m->space, m->va, b->capacity);
            mm::deallocate<Mapping>(m);
        }
    }
    object::release(drop);
    return E_OK;
}

SysError unmap(Arena& a, VmSpace space, VirtAddr va) {
    object::Record* drop;
    {
        Guard guard(lock);
        Backing* b = nullptr;
        auto* m = lookup(space, va, &b);
        if (!m || m->va != va || b != a.managed) return E_INVALID;
        Rendezvous stop; if (!stop.ready) return E_BUSY;
        mm::vm_break_slots(m->space, m->slots, b->capacity / PAGE_SIZE);
        mm::vm_drop_slots(m->space, m->slots, b->capacity / PAGE_SIZE);
        auto** link = &b->maps;
        while (*link != m) link = &(*link)->next;
        *link = m->next; --b->aliases;
        prune(m->space, m->va, b->capacity);
        drop = m->lifetime;
        mm::deallocate<Mapping>(m);
    }
    object::release(drop); return E_OK;
}

void detach_space(VmSpace space) {
    { Guard guard(lock); forget_cages(space.pgd_phys); }
    // Release references after unlinking, outside the manager lock. Destruction
    // can reenter the manager when this was the object's final reference.
    for (;;) {
        object::Record* drop = nullptr;
        {
            Guard guard(lock);
            for (auto* b = arenas; b && !drop; b = b->next) {
                for (auto** link = &b->maps; *link; link = &(*link)->next) {
                    auto* m = *link;
                    if (m->space.pgd_phys != space.pgd_phys) continue;
                    mm::vm_break_slots(m->space, m->slots, b->capacity / PAGE_SIZE);
                    mm::vm_drop_slots(m->space, m->slots, b->capacity / PAGE_SIZE);
                    prune(m->space, m->va, b->capacity);
                    *link = m->next; --b->aliases; drop = m->lifetime;
                    mm::deallocate<Mapping>(m); break;
                }
            }
        }
        if (!drop) return;
        object::release(drop);
    }
}

void release_owner_pins(u64 owner) {
    { Guard guard(lock); forget_cages(0, owner); }
    for (;;) {
        object::Record* drop = nullptr;
        {
            Guard guard(lock);
            for (auto* b = arenas; b; b = b->next) {
                if (identity(*b->arena) != owner || !b->pins) continue;
                adma::fence(); b->pins = 0; drop = b->pin_lifetime;
                b->pin_lifetime = nullptr; break;
            }
        }
        if (!drop) return;
        object::release(drop);
    }
}

namespace {
struct MappingPlan {
    MappingPlan* next;
    Mapping* original;
    Mapping prepared;
    bool fresh[kPages];
    VirtAddr previous_va;
};
// All storage and page-table allocation precedes publication. The transaction
// preserves every original page, alias and size on any preparation failure.
SysError change(Arena& a, u64 size, u64 capacity, Mapping* moving = nullptr, VirtAddr new_va = 0) {
    auto& b = *a.managed;
    if (capacity > b.cage_size || size > capacity) return E_INVALID;
    const usize previous = b.capacity / PAGE_SIZE, count = capacity / PAGE_SIZE;
    const bool relocate = moving && moving->va != new_va;
    if (capacity == b.capacity && !relocate) {
        if (size == a.size) return E_OK;
        if (size > a.size) {
            const usize first = a.size / PAGE_SIZE; a.size = size;
            ++b.generation; ++b.zero_copy_changes;
            for (auto* m = b.maps; m; m = m->next) make_map(b, *m, first);
            touch(b); return E_OK;
        }
        Rendezvous stop;
        if (!stop.ready) return E_BUSY;
        break_maps(b); a.size = size; ++b.generation; ++b.zero_copy_changes;
        make_maps(b); touch(b); return E_OK;
    }
    for (auto* m = b.maps; m; m = m->next) {
        const auto va = m == moving ? new_va : m->va;
        if ((va & (PAGE_SIZE - 1)) || va < b.cage_base ||
            va - b.cage_base > b.cage_size || capacity > b.cage_size - (va - b.cage_base))
            return E_INVALID;
        if (overlaps(m->space, va, capacity, m)) return E_BUSY;
        for (auto* n = m->next; n; n = n->next) {
            const auto other = n == moving ? new_va : n->va;
            if (m->space.pgd_phys == n->space.pgd_phys &&
                va < other + capacity && other < va + capacity) return E_BUSY;
        }
    }
    Page* added[kPages]{};
    MappingPlan* plans = nullptr;
    auto cleanup = [&](bool rollback) {
        for (auto* plan = plans; plan; plan = plan->next)
            if (rollback)
                for (usize i = 0; i < count; ++i)
                    if (plan->fresh[i])
                        mm::vm_drop_slots(plan->prepared.space, &plan->prepared.slots[i], 1);
        while (plans) {
            auto* plan = plans; plans = plan->next;
            prune(plan->prepared.space, rollback ? plan->prepared.va : plan->previous_va,
                  rollback ? capacity : previous * PAGE_SIZE);
            mm::deallocate<MappingPlan>(plan);
        }
        if (rollback) for (usize i = previous; i < count; ++i) release(added[i]);
    };
    for (usize i = previous; i < count; ++i) {
        added[i] = allocate(identity(a));
        if (!added[i]) { cleanup(true); return E_NOMEM; }
    }
    for (auto* m = b.maps; m; m = m->next) {
        auto* plan = static_cast<MappingPlan*>(mm::allocate<MappingPlan>());
        if (!plan) { cleanup(true); return E_NOMEM; }
        *plan = {}; plan->original = m; plan->prepared = *m; plan->previous_va = m->va;
        plan->prepared.va = m == moving ? new_va : m->va;
        for (auto& slot : plan->prepared.slots) slot = nullptr;
        plan->next = plans; plans = plan;
        for (usize i = 0; i < count; ++i) {
            const auto va = plan->prepared.va + i * PAGE_SIZE;
            if (va >= m->va && va - m->va < b.capacity)
                plan->prepared.slots[i] = m->slots[(va - m->va) / PAGE_SIZE];
            else {
                plan->prepared.slots[i] = mm::vm_prepare_slot(m->space, va);
                plan->fresh[i] = plan->prepared.slots[i] != nullptr;
                if (!plan->prepared.slots[i]) { cleanup(true); return E_BUSY; }
            }
            plan->prepared.enabled[i] = i < previous ? m->enabled[i] : true;
        }
        for (usize i = count; i < kPages; ++i) plan->prepared.enabled[i] = false;
    }
    if (!relocate && count >= previous && size >= a.size) {
        const usize first = a.size / PAGE_SIZE;
        for (usize i = previous; i < count; ++i) {
            b.pages[i] = added[i]; b.dirty[i] = b.accessed[i] = false; b.heat[i] = 0;
        }
        for (auto* plan = plans; plan; plan = plan->next) *plan->original = plan->prepared;
        b.capacity = capacity; a.size = size; ++b.generation; ++b.zero_copy_changes;
        for (auto* m = b.maps; m; m = m->next) make_map(b, *m, first);
        touch(b); cleanup(false); return E_OK;
    }
    Rendezvous stop;
    if (!stop.ready) { cleanup(true); return E_BUSY; }
    break_maps(b);
    for (auto* plan = plans; plan; plan = plan->next)
        mm::vm_drop_slots(plan->original->space, plan->original->slots, previous);
    for (auto* plan = plans; plan; plan = plan->next) {
        *plan->original = plan->prepared;
        mm::vm_break_slots(plan->original->space, plan->original->slots, count);
    }
    for (usize i = previous; i < count; ++i) {
        b.pages[i] = added[i]; b.dirty[i] = b.accessed[i] = false; b.heat[i] = 0;
    }
    for (usize i = count; i < previous; ++i) {
        release(b.pages[i]); b.pages[i] = nullptr;
        b.dirty[i] = b.accessed[i] = false; b.heat[i] = 0;
    }
    b.capacity = capacity; a.size = size; ++b.generation; ++b.zero_copy_changes;
    make_maps(b); touch(b); cleanup(false);
    return E_OK;
}
}
SysError resize(Arena& a, u64 bytes) {
    Guard guard(lock);
    if (auto error = editable(a.managed); error != E_OK) return error;
    u64 size;
    if (!rounded(bytes, &size, true)) return E_INVALID;
    return change(a, size, size > a.managed->capacity ? size : a.managed->capacity);
}
SysError reserve(Arena& a, u64 bytes) {
    Guard guard(lock);
    if (auto error = editable(a.managed); error != E_OK) return error;
    u64 capacity;
    if (!rounded(bytes, &capacity) || capacity < a.managed->capacity) return E_INVALID;
    return change(a, a.size, capacity);
}
SysError trim(Arena& a, u64 bytes) {
    Guard guard(lock);
    if (auto error = editable(a.managed); error != E_OK) return error;
    u64 capacity;
    if (!rounded(bytes ? bytes : (a.size ? a.size : PAGE_SIZE), &capacity) ||
        capacity < a.size || capacity > a.managed->capacity) return E_INVALID;
    return change(a, a.size, capacity);
}
SysError realloc(Arena& a, VmSpace space, VirtAddr old_va, VirtAddr new_va, u64 bytes) {
    Guard guard(lock);
    if (auto error = editable(a.managed); error != E_OK) return error;
    Backing* backing = nullptr;
    auto* m = lookup(space, old_va, &backing);
    u64 size;
    if (!m || m->va != old_va || backing != a.managed || !rounded(bytes, &size, true)) return E_INVALID;
    return change(a, size, size > backing->capacity ? size : backing->capacity, m, new_va);
}

SysError reorder(Arena& a, const u32* order, usize count) {
    Guard guard(lock);
    auto* b = a.managed;
    if (auto error = editable(b); error != E_OK) return error;
    if (!order || count != a.size / PAGE_SIZE) return E_INVALID;
    bool seen[kPages]{};
    Page* sorted[kPages]{};
    for (usize i = 0; i < count; ++i) {
        if (order[i] >= count || seen[order[i]]) return E_INVALID;
        seen[order[i]] = true; sorted[i] = b->pages[order[i]];
    }
    Rendezvous stop;
    if (!stop.ready) return E_BUSY;
    break_maps(*b);
    for (usize i = 0; i < count; ++i) {
        b->pages[i] = sorted[i]; b->dirty[i] = b->accessed[i] = false; b->heat[i] = 0;
    }
    ++b->generation; ++b->zero_copy_changes; make_maps(*b); touch(*b);
    return E_OK;
}

SysError clone(Arena& a, u32 pid, u32 generation, Arena** out, VmSpace owner_space) {
    *out = nullptr;
    Guard guard(lock);
    auto* b = a.managed;
    if (auto error = editable(b); error != E_OK) return error;
    if (arena_count == kMaxArenas) return E_OVERFLOW;
    if (!type_cage_allowed((u64{generation} << 32) | pid, b->cage_base,
                           b->cage_size, b->type_id, owner_space)) return E_POLICY;
    if (b->type_id && owner_space.pgd_phys &&
        !mm::vm_cage_available(owner_space, b->cage_base, b->cage_size)) return E_POLICY;
    auto* copy = static_cast<Arena*>(mm::allocate<Arena>());
    auto* next = static_cast<Backing*>(mm::allocate<Backing>());
    if (!copy || !next) {
        mm::deallocate<Arena>(copy);
        mm::deallocate<Backing>(next); return E_NOMEM;
    }
    Rendezvous stop;
    if (!stop.ready) {
        mm::deallocate<Arena>(copy);
        mm::deallocate<Backing>(next); return E_BUSY;
    }
    if (!remember_cage((u64{generation} << 32) | pid, owner_space,
                       b->cage_base, b->cage_size, b->type_id)) {
        mm::deallocate<Arena>(copy); mm::deallocate<Backing>(next); return E_NOMEM;
    }
    *copy = a; *next = {};
    copy->managed = next; copy->owner_pid = pid; copy->owner_generation = generation;
    next->arena = copy; next->capacity = b->capacity; next->cage_base = b->cage_base;
    next->cage_size = b->cage_size; next->type_id = b->type_id; next->generation = 1;
    next->policy = b->policy;
    next->sampling = b->sampling;
    break_maps(*b);
    for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i) {
        next->pages[i] = b->pages[i]; ++next->pages[i]->references;
    }
    ++b->zero_copy_changes; make_maps(*b);
    next->next = arenas; arenas = next; ++arena_count; touch(*next);
    *out = copy;
    return E_OK;
}

SysError precopy(Arena& a, bool require_contiguous, usize page_budget) {
    {
    Guard guard(lock);
    auto* b = a.managed;
    if (!b) return E_BADTYPE;
    if (b->round) {
        // The existing round is progressed after releasing the manager lock.
    } else {
    if (b->pins || b->external_pins || (b->state != MX_ARENA_STABLE && b->state != MX_ARENA_DIRTY &&
                    b->state != MX_ARENA_PRECOPY_STATE)) return E_BUSY;
    auto* round = static_cast<CopyRound*>(mm::allocate<CopyRound>());
    if (!round) return E_NOMEM;
    *round = {};
    round->lifetime = object::acquire_existing(&a);
    if (!round->lifetime) {
        mm::deallocate<CopyRound>(round); return E_DEAD;
    }
    // The caller's reference keeps the arena alive during failed preparation.
    auto fail_round = [&](SysError error) {
        object::release(round->lifetime);
        mm::deallocate<CopyRound>(round);
        return error;
    };
    const bool first = b->state == MX_ARENA_STABLE;
    if (first) {
        const usize count = b->capacity / PAGE_SIZE;
        const PhysAddr contiguous = mm::alloc_pages_contiguous_for_identity(identity(a), count);
        if (!contiguous && require_contiguous) return fail_round(E_NOMEM);
        for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i) {
            if (contiguous) {
                b->pending[i] = static_cast<Page*>(mm::allocate<Page>());
                if (b->pending[i])
                    *b->pending[i] = {contiguous + i * PAGE_SIZE, identity(a), 1};
            } else b->pending[i] = allocate(identity(a));
            if (!b->pending[i]) {
                if (contiguous)
                    for (usize j = i; j < count; ++j)
                        mm::free_page_for_identity(identity(a), contiguous + j * PAGE_SIZE);
                discard_pending(*b); return fail_round(E_NOMEM);
            }
        }
    }
    {
        Rendezvous stop;
        if (!stop.ready) {
            if (first) discard_pending(*b);
            return fail_round(E_BUSY);
        }
        break_maps(*b);
        for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i) {
            if (first || b->dirty[i]) {
                round->sources[i] = b->pages[i]; ++b->pages[i]->references;
            }
            b->dirty[i] = false;
        }
        round->count = b->capacity / PAGE_SIZE; b->round = round;
        b->state = MX_ARENA_PRECOPY_STATE; make_maps(*b);
    }
    }
    }
    return advance_round(a, page_budget);
}

SysError commit(Arena& a) {
    // An unfinished copy never races the final reconciliation or retirement.
    const auto progress = advance_round(a, kPages);
    if (progress != E_OK) return progress;
    Guard guard(lock);
    auto* b = a.managed;
    if (!b) return E_BADTYPE;
    if (b->round || b->pins || b->external_pins ||
        (b->state != MX_ARENA_PRECOPY_STATE && b->state != MX_ARENA_DIRTY)) return E_BUSY;
    Rendezvous stop;
    if (!stop.ready) return E_BUSY;
    b->state = MX_ARENA_FROZEN;
    // Rendezvous is a grace period for every CPU reader. Break every alias and
    // complete broadcast TLBI before changing any backing identity.
    break_maps(*b);
    for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i) if (b->dirty[i]) {
        adma::copy_page(b->pending[i]->pa, b->pages[i]->pa); b->copied_bytes += PAGE_SIZE;
    }
    adma::fence();
    b->state = MX_ARENA_COMMITTING;
    Page* retired[kPages]{};
    for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i) {
        retired[i] = b->pages[i]; b->pages[i] = b->pending[i];
        b->pending[i] = nullptr; b->dirty[i] = false;
        b->accessed[i] = false;
    }
    b->retired_generation = b->generation++;
    b->state = MX_ARENA_STABLE;
    make_maps(*b);
    // RETIRED generation has no CPU mappings and the copy fence completed.
    // Pins prohibit reaching this point while an external reader owns pages.
    for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i) release(retired[i]);
    touch(*b);
    return E_OK;
}

SysError cancel(Arena& a) {
    const auto progress = advance_round(a, 0, true);
    if (progress == E_BUSY) return E_BUSY;
    Guard guard(lock);
    auto* b = a.managed;
    if (!b) return E_BADTYPE;
    if (b->state == MX_ARENA_STABLE) return E_OK;
    Rendezvous stop;
    if (!stop.ready) return E_BUSY;
    break_maps(*b); discard_pending(*b); b->state = MX_ARENA_STABLE; make_maps(*b);
    return E_OK;
}

SysError pin(Arena& a, u64 generation) {
    Guard guard(lock);
    auto* b = a.managed;
    if (!b) return E_BADTYPE;
    if (b->generation != generation || b->state != MX_ARENA_STABLE) return E_BUSY;
    if (b->pins == ~u32{0}) return E_OVERFLOW;
    // A physical pin must never refer to a page that a later COW fault can
    // replace. Migrate an arena to private backing before pinning a clone.
    for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i)
        if (b->pages[i]->references > 1) return E_BUSY;
    if (!b->pins) {
        b->pin_lifetime = object::acquire_existing(&a);
        if (!b->pin_lifetime) return E_DEAD;
    }
    ++b->pins; return E_OK;
}
SysError pin_external(Arena& a, ExternalPin& lease, SysError (*fence)(void*), void* context) {
    Guard guard(lock);
    auto* b = a.managed;
    if (!b || lease.arena || !fence || !a.size) return E_INVALID;
    if (b->state != MX_ARENA_STABLE || b->round) return E_BUSY;
    for (usize i = 0; i < a.size / PAGE_SIZE; ++i)
        if (b->pages[i]->references != 1 || b->pages[i]->pa != b->pages[0]->pa + i * PAGE_SIZE)
            return E_BUSY;
    if (b->external_pins == ~u32{0}) return E_OVERFLOW;
    auto* lifetime = object::acquire_existing(&a);
    if (!lifetime) return E_DEAD;
    lease = {&a, lifetime, b->generation, b->pages[0]->pa, a.size, fence, context, false, true};
    ++b->external_pins;
    return E_OK;
}
SysError prepare_dma(ExternalPin& lease, bool coherent) {
    Guard guard(lock);
    if (!lease.arena || lease.arena->managed->generation != lease.generation) return E_INVALID;
    if (lease.dma && lease.coherent != coherent) return E_INVALID;
    adma::publish_dma(lease.base, lease.size, coherent);
    lease.dma = true; lease.coherent = coherent;
    return E_OK;
}
SysError unpin_external(ExternalPin& lease) {
    if (!lease.arena || !lease.fence) return E_INVALID;
    if (lease.fence(lease.context) != E_OK) return E_BUSY;
    object::Record* drop;
    {
        Guard guard(lock);
        auto& b = *lease.arena->managed;
        if (b.generation != lease.generation || !b.external_pins) return E_INVALID;
        if (lease.dma) adma::complete_dma(lease.base, lease.size, lease.coherent);
        --b.external_pins; drop = lease.lifetime; lease = {};
    }
    object::release(drop); return E_OK;
}
SysError unpin(Arena& a, u64 generation) {
    object::Record* drop = nullptr;
    {
        Guard guard(lock);
        auto* b = a.managed;
        if (!b) return E_BADTYPE;
        if (b->generation != generation || !b->pins) return E_INVALID;
        adma::fence();
        if (!--b->pins) { drop = b->pin_lifetime; b->pin_lifetime = nullptr; }
    }
    object::release(drop); return E_OK;
}
SysError policy(Arena& a, u32 flags) {
    Guard guard(lock);
    if (!a.managed || (flags & ~MX_ARENA_COMPACT_IDLE)) return E_INVALID;
    auto& b = *a.managed;
    Rendezvous stop;
    if (!stop.ready) return E_BUSY;
    break_maps(b);
    b.policy = flags; b.sampling = (flags & MX_ARENA_COMPACT_IDLE) != 0;
    if (b.state == MX_ARENA_STABLE)
        for (usize i = 0; i < kPages; ++i) b.dirty[i] = b.accessed[i] = false;
    make_maps(b); return E_OK;
}
MxArenaInfo inspect(Arena& a) {
    Guard guard(lock);
    auto* b = a.managed;
    if (!b) return {};
    u32 dirty = 0;
    for (usize i = 0; i < b->capacity / PAGE_SIZE; ++i) dirty += b->dirty[i];
    return {a.size, b->capacity, b->generation, b->retired_generation, b->type_id,
            b->copied_bytes, b->zero_copy_changes, b->state, b->pins + b->external_pins, b->aliases, dirty};
}
MxArenaCreate configuration(Arena& a) {
    Guard guard(lock);
    auto& b = *a.managed;
    return {a.size, b.capacity, b.cage_base, b.cage_size, b.type_id, b.policy, 0};
}
bool fault(VmSpace space, VirtAddr va, bool write) {
    Guard guard(lock);
    Backing* b = nullptr;
    auto* m = lookup(space, va, &b);
    if (!m) return false;
    const usize index = (va - m->va) / PAGE_SIZE;
    if (index >= b->arena->size / PAGE_SIZE || !m->enabled[index]) return false;
    note_access(*b, index);
    return write ? write_fault(*b, *m, index) : bool(*m->slots[index]);
}
bool prepare_read(VmSpace space, VirtAddr va, u64 size) {
    if (size && va > ~u64{0} - (size - 1)) return false;
    Guard guard(lock);
    while (size) {
        Backing* b = nullptr;
        if (auto* m = lookup(space, va, &b)) {
            const usize index = (va - m->va) / PAGE_SIZE;
            if (index >= b->arena->size / PAGE_SIZE || !m->enabled[index] || !(m->rights & RIGHT_READ))
                return false;
            note_access(*b, index);
        }
        const auto chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (size <= chunk) break;
        size -= chunk; va += chunk;
    }
    return true;
}
bool prepare_write(VmSpace space, VirtAddr va, u64 size) {
    if (size && va > ~u64{0} - (size - 1)) return false;
    Guard guard(lock);
    while (size) {
        Backing* b = nullptr;
        if (auto* m = lookup(space, va, &b)) {
            if (!write_fault(*b, *m, (va - m->va) / PAGE_SIZE)) return false;
        }
        const u64 chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (size <= chunk) break;
        size -= chunk; va += chunk;
    }
    return true;
}

void idle_step() {
    object::Record* lifetime = nullptr;
    Arena* candidate = nullptr;
    bool sample = false;
    bool copy = false, collect = false;
    {
        kernel::InterruptGuard irq;
        u32 expected = 0;
        if (!__atomic_compare_exchange_n(&lock.value, &expected, 1u, false,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        ++activity_epoch;
        collect = retired_pages || ready_pages;
        Backing* selected = nullptr;
        for (auto* b = arenas; b; b = b->next) {
            // An outstanding generation must complete even after policy is
            // disabled. Oldest-first selection prevents a stalled DMA ticket
            // from starving independent arenas.
            const bool pending = b->round || b->state != MX_ARENA_STABLE;
            if (!pending && (!(b->policy & MX_ARENA_COMPACT_IDLE) || b->pins ||
                b->external_pins || !b->aliases || activity_epoch - b->touched < 128)) continue;
            if (!selected || b->touched < selected->touched) selected = b;
        }
        if (selected) {
            auto& b = *selected;
            candidate = b.arena; lifetime = object::acquire_existing(candidate);
            if (lifetime) {
                copy = b.round;
                if (!copy && b.state == MX_ARENA_STABLE) {
                    bool fragmented = false; u32 heat = 0;
                    for (usize i = 0; i < b.capacity / PAGE_SIZE; ++i) {
                        b.heat[i] >>= 1;
                        if (b.heat[i] > heat) heat = b.heat[i];
                        if (i && b.pages[i]->pa != b.pages[i - 1]->pa + PAGE_SIZE) fragmented = true;
                    }
                    sample = !fragmented || heat > 2;
                }
                touch(b);
            } else candidate = nullptr;
        }
        __atomic_store_n(&lock.value, 0u, __ATOMIC_RELEASE);
    }
    if (!candidate) { if (collect) (void)reclaim(4); return; }
    if (copy) (void)advance_round(*candidate, 4);
    else if (sample) {
        Guard guard(lock);
        auto& b = *candidate->managed;
        Rendezvous stop;
        if (stop.ready && b.state == MX_ARENA_STABLE && !b.pins && !b.external_pins) {
            break_maps(b);
            for (usize i = 0; i < kPages; ++i) b.dirty[i] = b.accessed[i] = false;
            make_maps(b);
        }
    } else if (inspect(*candidate).state == MX_ARENA_STABLE) (void)precopy(*candidate, true, 4);
    else (void)commit(*candidate);
    object::release(lifetime);
}
SysError reclaim(usize page_budget) {
    if (!__atomic_load_n(&reclaim_needed, __ATOMIC_ACQUIRE)) return E_OK;
    {
        Guard guard(lock);
        if (retired_pages) {
            Rendezvous stop;
            if (!stop.ready) return E_BUSY;
        }
    }
    // Zeroing/releasing physical frames runs after CPUs resume. Retirement
    // publication and each pop are serialized; no reader can reacquire a frame
    // whose last reference was removed before the completed grace period.
    while (page_budget--) {
        Page* page;
        {
            Guard guard(lock);
            page = ready_pages;
            if (!page) {
                if (!retired_pages) __atomic_store_n(&reclaim_needed, 0u, __ATOMIC_RELEASE);
                break;
            }
            ready_pages = page->retired_next;
        }
        free_page_record(page);
    }
    return E_OK;
}
} // namespace amm
