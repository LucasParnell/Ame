#include "anko/vm.h"
#include "anko/log.h"
#include "libk/string.h"
#include "libk/printf.h"

namespace vm {

namespace {

u32 g_next_mem_id = 1;

class KernelAddressSpaceGuard {
public:
    explicit KernelAddressSpaceGuard(PhysAddr restore_root)
        : restore_root_(restore_root) {
        mm::vm_activate(0);
    }

    ~KernelAddressSpaceGuard() {
        mm::vm_activate(restore_root_);
    }

    KernelAddressSpaceGuard(const KernelAddressSpaceGuard&) = delete;
    KernelAddressSpaceGuard& operator=(const KernelAddressSpaceGuard&) = delete;

private:
    PhysAddr restore_root_;
};

bool alloc_backing_pages(usize pages, PhysAddr** out_pages) {
    if (!out_pages || pages == 0) return false;

    usize bytes = 0;
    if (__builtin_mul_overflow(pages, static_cast<usize>(sizeof(PhysAddr)), &bytes)) {
        return false;
    }

    PhysAddr* backing = static_cast<PhysAddr*>(mm::slab_alloc(bytes, alignof(PhysAddr)));
    if (!backing) {
        ANKO_LOG_ERROR("kernel/vm", "vm_memory backing array alloc failed");
        return false;
    }
    libk::memset(backing, 0, bytes);

    for (usize i = 0; i < pages; ++i) {
        PhysAddr pa = mm::alloc_page();
        if (!pa) {
            ANKO_LOGF_ERROR("kernel/vm", "vm_memory alloc_page failed at %llu/%llu",
                            static_cast<unsigned long long>(i),
                            static_cast<unsigned long long>(pages));
            for (usize j = 0; j < i; ++j) mm::free_page(backing[j]);
            mm::slab_free(backing, bytes, alignof(PhysAddr));
            return false;
        }
        backing[i] = pa;

        // Keep the first Linux bring-up deterministic. This costs time for
        // 128 MiB RAM, but with QEMU_MEM=512M it should complete and gives a
        // clean zeroed guest RAM image.
        libk::memset(reinterpret_cast<void*>(pa), 0, PAGE_SIZE);
    }

    *out_pages = backing;
    return true;
}

void free_backing_pages(PhysAddr* pages, usize page_count) {
    if (!pages) return;
    for (usize i = 0; i < page_count; ++i) if (pages[i]) mm::free_page(pages[i]);
    mm::slab_free(pages, page_count * sizeof(PhysAddr), alignof(PhysAddr));
}

bool mapping_overlaps(const VmMemory& mem, u64 guest_pa, u64 size) {
    if (!mem.mapped_stage2 || mem.mapped_size == 0) return false;
    u64 end = 0;
    u64 other_end = 0;
    if (__builtin_add_overflow(guest_pa, size, &end)) return true;
    if (__builtin_add_overflow(mem.guest_pa_base, mem.mapped_size, &other_end)) return true;
    return guest_pa < other_end && end > mem.guest_pa_base;
}

bool guest_range_valid(u64 guest_pa, u64 size) {
    if (size == 0) return false;
    if ((guest_pa & (PAGE_SIZE - 1)) != 0 || (size & (PAGE_SIZE - 1)) != 0) return false;
    u64 end = 0;
    if (__builtin_add_overflow(guest_pa, size, &end)) return false;
    if (end <= guest_pa) return false;
    return guest_pa >= kGuestPhysMin && end <= kGuestPhysMax;
}

} // namespace

CapHandle create_memory(Process& caller, CapHandle vm_cap, u64 size, u64 flags,
                        SysError* out_error) {
    SysError rc = E_OK;
    auto v = vm_from_cap(caller, vm_cap, RIGHT_VM_CONTROL, &rc);
    if (!v) {
        ANKO_LOGF_ERROR("kernel/vm", "vm_memory_create vm cap fail rc=%d",
                        static_cast<int>(rc));
        if (out_error) *out_error = rc;
        return kNullCap;
    }
    if (v->memory_count >= kMaxVmMemoryObjects) {
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }
    if (size == 0 || size > 256ull * 1024 * 1024 || (size & (PAGE_SIZE - 1)) != 0) {
        if (out_error) *out_error = E_INVALID;
        note_error();
        return kNullCap;
    }

    VmMemory* mem = static_cast<VmMemory*>(mm::slab_alloc(sizeof(VmMemory), alignof(VmMemory)));
    if (!mem) {
        if (out_error) *out_error = E_NOMEM;
        note_error();
        return kNullCap;
    }
    libk::memset(mem, 0, sizeof(*mem));

    const usize pages = static_cast<usize>(size / PAGE_SIZE);
    PhysAddr* backing = nullptr;
    bool backing_ok = false;
    {
        KernelAddressSpaceGuard kernel_space(caller.ttbr0);
        backing_ok = alloc_backing_pages(pages, &backing);
    }
    if (!backing_ok) {
        mm::slab_free(mem, sizeof(VmMemory), alignof(VmMemory));
        ANKO_LOG_ERROR("kernel/vm", "vm_memory_create backing allocation failed");
        if (out_error) *out_error = E_NOMEM;
        note_error();
        return kNullCap;
    }

    mem->mem_id = g_next_mem_id++;
    mem->owner_vm_id = v->vm_id;
    mem->host_phys_base = backing[0];
    mem->host_va = static_cast<VirtAddr>(backing[0]);
    mem->pages = backing;
    mem->page_count = pages;
    mem->size = size;
    mem->flags = static_cast<u32>(flags);

    mem->owner_lifetime = v->lifetime; object::retain(mem->owner_lifetime);
    mem->lifetime = object::create(mem, dispose_memory);
    if (!mem->lifetime) { dispose_memory(mem); if (out_error) *out_error = E_NOMEM; return kNullCap; }
    const CapHandle cap_h = cap::alloc(*caller.cap_table, CAP_VM_MEMORY, mem,
                                       RIGHT_WRITE |
                                           RIGHT_VM_MEMORY_MAP |
                                           RIGHT_VM_MEMORY_INSPECT |
                                           RIGHT_DUP);
    if (cap_h == kNullCap) {
        ANKO_LOG_ERROR("kernel/vm", "vm_memory_create cap alloc failed");
        object::release(mem->lifetime);
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    v->memory_objects[v->memory_count++] = mem;
    metric_inc(VM_DEBUG_MEMORY_CREATED);
    refresh_vm_state(v);
    if (out_error) *out_error = E_OK;
    return cap_h;
}

SysError map_memory(Process& caller, CapHandle vm_cap, CapHandle memory_cap,
                    u64 guest_pa, u64 size, u32 rights) {
    SysError rc = E_OK;
    auto v = vm_from_cap(caller, vm_cap, RIGHT_VM_MAP, &rc);
    if (!v) return rc;

    auto mem = memory_from_cap(caller, memory_cap, RIGHT_VM_MEMORY_MAP, &rc);
    if (!mem) return rc;
    if (mem->owner_vm_id != v->vm_id) {
        note_error();
        return E_PERM;
    }
    if (!guest_range_valid(guest_pa, size)) {
        note_error();
        return E_INVALID;
    }
    if (size > mem->size || mem->mapped_stage2) {
        note_error();
        return E_INVALID;
    }
    if ((rights & ~(RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC)) != 0 || rights == 0) {
        note_error();
        return E_INVALID;
    }

    for (u32 i = 0; i < v->memory_count; ++i) {
        VmMemory* existing = v->memory_objects[i];
        if (!existing || existing == mem || existing->dead) continue;
        if (mapping_overlaps(*existing, guest_pa, size)) {
            note_error();
            return E_BUSY;
        }
    }

    const usize pages = static_cast<usize>(size / PAGE_SIZE);
    if (pages > mem->page_count || !mem->pages) {
        note_error();
        return E_INVALID;
    }
    if (v->stage2.mapped_pages + pages > kMaxStage2Mappings) {
        note_error();
        return E_OVERFLOW;
    }
    rc = stage2_validate_no_overlap(&v->stage2, guest_pa, size);
    if (rc != E_OK) {
        note_error();
        return rc;
    }

    usize mapped = 0;
    for (; mapped < pages; ++mapped) {
        rc = stage2_map_page(&v->stage2,
                             guest_pa + static_cast<u64>(mapped * PAGE_SIZE),
                             mem->pages[mapped],
                             rights);
        if (rc != E_OK) break;
    }
    if (rc != E_OK) {
        for (usize i = 0; i < mapped; ++i) {
            (void)stage2_unmap_page(&v->stage2, guest_pa + static_cast<u64>(i * PAGE_SIZE));
        }
        note_error();
        return rc;
    }

    mem->guest_pa_base = guest_pa;
    mem->mapped_size = size;
    mem->rights = rights;
    mem->mapped_stage2 = true;
    v->guest_ram_bytes += size;
    ++v->mapped_regions;
    metric_inc(VM_DEBUG_MEMORY_MAPPED);
    refresh_vm_state(v);
    return E_OK;
}

SysError unmap_memory(Process& caller, CapHandle vm_cap, u64 guest_pa, u64 size) {
    SysError rc = E_OK;
    auto v = vm_from_cap(caller, vm_cap, RIGHT_VM_MAP, &rc);
    if (!v) return rc;
    if (!guest_range_valid(guest_pa, size)) {
        note_error();
        return E_INVALID;
    }

    VmMemory* mem = nullptr;
    for (u32 i = 0; i < v->memory_count; ++i) {
        VmMemory* candidate = v->memory_objects[i];
        if (!candidate || candidate->dead) continue;
        if (candidate->mapped_stage2 &&
            candidate->guest_pa_base == guest_pa &&
            candidate->mapped_size == size) {
            mem = candidate;
            break;
        }
    }
    if (!mem) {
        note_error();
        return E_NOTFOUND;
    }

    const usize pages = static_cast<usize>(size / PAGE_SIZE);
    for (usize i = 0; i < pages; ++i) {
        (void)stage2_unmap_page(&v->stage2, guest_pa + static_cast<u64>(i * PAGE_SIZE));
    }
    mem->mapped_stage2 = false;
    mem->guest_pa_base = 0;
    mem->mapped_size = 0;
    mem->rights = 0;
    if (v->guest_ram_bytes >= size) v->guest_ram_bytes -= size;
    if (v->mapped_regions > 0) --v->mapped_regions;
    metric_inc(VM_DEBUG_MEMORY_UNMAPPED);
    refresh_vm_state(v);
    return E_OK;
}

SysError inspect_memory(Process& caller, CapHandle memory_cap, VmMemoryInfo* out) {
    if (!out) return E_INVALID;
    SysError rc = E_OK;
    auto mem = memory_from_cap(caller, memory_cap, RIGHT_VM_MEMORY_INSPECT, &rc);
    if (!mem) return rc;

    VmMemoryInfo info{};
    info.mem_id = mem->mem_id;
    info.owner_vm_id = mem->owner_vm_id;
    info.rights = mem->rights;
    info.flags = mem->flags;
    info.host_phys_base = mem->host_phys_base;
    info.host_va = mem->host_va;
    info.size = mem->size;
    info.guest_pa_base = mem->guest_pa_base;
    info.mapped_size = mem->mapped_size;
    info.mapped_stage2 = mem->mapped_stage2 ? 1 : 0;
    *out = info;
    return E_OK;
}

SysError write_memory(Process& caller, CapHandle memory_cap, u64 guest_offset,
                      const void* src, u64 len) {
    if (!src && len != 0) return E_INVALID;
    SysError rc = E_OK;
    auto mem = memory_from_cap(caller, memory_cap, RIGHT_WRITE, &rc);
    if (!mem) return rc;
    if (len == 0) return E_OK;
    u64 end = 0;
    if (__builtin_add_overflow(guest_offset, len, &end)) {
        note_error();
        return E_INVALID;
    }
    if (guest_offset >= mem->size || end > mem->size || !mem->pages) {
        note_error();
        return E_INVALID;
    }

    VmSpace caller_space{};
    caller_space.pgd_phys = caller.ttbr0;
    caller_space.pid = caller.pid;
    const VirtAddr src_base = reinterpret_cast<VirtAddr>(src);

    u64 copied = 0;
    {
        KernelAddressSpaceGuard kernel_space(caller.ttbr0);
        while (copied < len) {
            const u64 off = guest_offset + copied;
            const usize page_index = static_cast<usize>(off / PAGE_SIZE);
            const usize page_off = static_cast<usize>(off & (PAGE_SIZE - 1));
            const VirtAddr src_va = src_base + copied;
            const usize src_page_off = static_cast<usize>(src_va & (PAGE_SIZE - 1));
            usize chunk = PAGE_SIZE - page_off;
            const usize src_chunk = PAGE_SIZE - src_page_off;
            if (chunk > src_chunk) chunk = src_chunk;
            const u64 remaining = len - copied;
            if (chunk > remaining) chunk = static_cast<usize>(remaining);
            if (page_index >= mem->page_count || !mem->pages[page_index]) {
                note_error();
                return E_INVALID;
            }

            PhysAddr src_pa = 0;
            if (!mm::user_va_to_pa(caller_space, src_va, RIGHT_READ, &src_pa)) {
                note_error();
                return E_FAULT;
            }

            const PhysAddr dst_pa = mem->pages[page_index] + page_off;
            libk::memcpy(reinterpret_cast<void*>(dst_pa),
                         reinterpret_cast<const void*>(src_pa),
                         chunk);
            // Guest RAM can be executable through stage-2 immediately after this
            // write. Make freshly-copied kernel/trampoline bytes visible to the
            // instruction side before EL2 enters the guest.
            mm::sync_icache_range(dst_pa, chunk);
            copied += chunk;
        }
    }
    return E_OK;
}

void destroy_memory_object(VmMemory* mem) {
    if (!mem || mem->dead) return;
    free_backing_pages(mem->pages, static_cast<usize>(mem->page_count));
    mem->pages = nullptr;
    mem->page_count = 0;
    mem->host_phys_base = 0;
    mem->host_va = 0;
    mem->dead = true;
}

void dispose_memory(void* target) {
    auto* mem = static_cast<VmMemory*>(target);
    auto* owner = mem->owner_lifetime;
    destroy_memory_object(mem);
    mm::slab_free(mem, sizeof(VmMemory), alignof(VmMemory));
    object::release(owner);
}

} // namespace vm
