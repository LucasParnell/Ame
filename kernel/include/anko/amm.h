#pragma once
#include "anko/mm.h"
#include "anko/syscall.h"
#include "midix/abi/arena.h"

namespace amm {
struct ExternalPin {
    Arena* arena = nullptr;
    object::Record* lifetime = nullptr;
    u64 generation = 0;
    PhysAddr base = 0;
    u64 size = 0;
    SysError (*fence)(void*) = nullptr;
    void* context = nullptr;
    bool dma = false, coherent = true;
};
SysError pin_external(Arena&, ExternalPin&, SysError (*fence)(void*), void* context);
SysError prepare_dma(ExternalPin&, bool coherent);
SysError unpin_external(ExternalPin&); // failed fence retains the lease and pages
// APIs serialize metadata. A rendezvous parks remote CPUs before PTE/backing
// replacement, so aliases and kernel user accesses observe one generation.
SysError create(u32 pid, u32 owner_generation, const MxArenaCreate&, Arena** out,
                VmSpace owner_space = {});
SysError adopt(u32 pid, u32 generation, VmSpace, VirtAddr, u64 size,
                Arena**, object::Record** creator);
void destroy(void*);
SysError map(Arena&, object::Record*, VmSpace, VirtAddr, u32 rights, u64 owner = 0);
bool reserved(VmSpace, VirtAddr, u64 size);
SysError unmap_page(VmSpace, VirtAddr);
SysError unmap_wired(VmSpace, VirtAddr, ArenaTable* = nullptr);
inline bool unmap(VmSpace s, VirtAddr va) { return unmap_page(s, va) == E_OK; }
void detach_space(VmSpace);
void release_owner_pins(u64 owner_identity);
SysError resize(Arena&, u64 size);
SysError reserve(Arena&, u64 capacity);
SysError trim(Arena&, u64 capacity = 0);
SysError unmap(Arena&, VmSpace, VirtAddr);
SysError realloc(Arena&, VmSpace, VirtAddr old_va, VirtAddr new_va, u64 size);
SysError reorder(Arena&, const u32* order, usize count);
SysError clone(Arena&, u32 pid, u32 owner_generation, Arena** out, VmSpace owner_space = {});
bool map_wired(u64 owner, VmSpace, VirtAddr, PhysAddr, u64 size, u32 rights,
                bool device, bool owned = false);
// Publish the wired mapping, its VA claim and its lifetime pin together.
// unmap_wired must receive the same table when retiring these mappings.
SysError map_shared_wired(u64 owner, VmSpace, ArenaTable&, Arena&, object::Record*,
                          VirtAddr, u32 rights);
SysError precopy(Arena&, bool require_contiguous = false, usize page_budget = kMaxUserArenaPages);
SysError commit(Arena&);
SysError cancel(Arena&);
SysError pin(Arena&, u64 expected_generation);
SysError unpin(Arena&, u64 expected_generation);
SysError policy(Arena&, u32 flags);
MxArenaInfo inspect(Arena&);
MxArenaCreate configuration(Arena&);
bool fault(VmSpace, VirtAddr, bool write);
bool prepare_write(VmSpace, VirtAddr, u64 size);
bool prepare_read(VmSpace, VirtAddr, u64 size);
// One cold arena per idle call; no work when the manager lock is occupied.
void idle_step();
SysError reclaim(usize page_budget = kMaxUserArenaPages);
}
