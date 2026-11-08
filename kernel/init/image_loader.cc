// Native MXI executable loader. ELF is accepted only by the host converter.
#include "anko/init.h"
#include "anko/integrity.h"
#include "midix/format/image.h"
#include "anko/log.h"
#include "anko/random.h"
#include "libk/string.h"

namespace kernel::init {

namespace {

using midix::format::SegmentPlan;

void unwind_loaded(VmSpace& space, const SegmentPlan* plans, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        mm::vm_unmap_owned_range(space, plans[i].va_start, plans[i].map_size);
    }
}

bool load_plan(VmSpace& space, const u8* image_data, const SegmentPlan& plan) {
    for (u64 off = 0; off < plan.map_size; off += PAGE_SIZE) {
        PhysAddr page = mm::alloc_page_for(space.pid);
        if (!page) {
            mm::vm_unmap_owned_range(space, plan.va_start, off);
            return false;
        }
        libk::memset(reinterpret_cast<void*>(page), 0, PAGE_SIZE);

        const u64 page_seg_start = off;
        const u64 page_seg_end = off + PAGE_SIZE;
        const u64 file_start = page_seg_start > plan.page_off ? page_seg_start - plan.page_off : 0;
        if (file_start < plan.file_size) {
            u64 file_end = page_seg_end > plan.page_off ? page_seg_end - plan.page_off : 0;
            if (file_end > plan.file_size) {
                file_end = plan.file_size;
            }
            if (file_end > file_start) {
                const u64 dst_off = (plan.page_off + file_start) - page_seg_start;
                const auto* src = image_data + plan.file_offset + file_start;
                libk::memcpy(reinterpret_cast<u8*>(page) + dst_off, src,
                             static_cast<usize>(file_end - file_start));
            }
        }

        if (plan.rights & RIGHT_EXEC) {
            mm::sync_icache_range(page, PAGE_SIZE);
        }

        if (!mm::vm_map_owned(space, plan.va_start + off, page, plan.rights)) {
            mm::free_page_for(space.pid, page);
            mm::vm_unmap_owned_range(space, plan.va_start, off);
            return false;
        }
    }
    return true;
}

} // namespace

VirtAddr load_native_image(VmSpace& space, const void* image_data, usize image_size, MxArenaCreate* arena) {
    midix::format::ImagePlan plan;
    const auto* bytes = static_cast<const u8*>(image_data);
    if (const char* error = midix::format::parse_image(bytes, image_size, plan)) {
        ANKO_LOGF_ERROR("kernel/loader", "image validation: %s", error);
        return 0;
    }
    u64 loaded_base = plan.layout.minimum;
    if (plan.aslr && security::random_ready()) {
        const u64 extent = (plan.layout.maximum - plan.layout.minimum + 4095) & ~u64{4095};
        constexpr u64 low = MX_IMAGE_ASLR_START, high = MX_IMAGE_ASLR_END;
        if (extent > high - low) return 0;
        const u64 destination = low + security::random_below((high - low - extent) / PAGE_SIZE + 1) * PAGE_SIZE;
        loaded_base = destination;
        for (u32 i = 0; i < plan.count; ++i)
            if (!midix::format::rebase_image_address(plan.segments[i].va_start, plan.layout.minimum,
                                                    loaded_base, &plan.segments[i].va_start)) return 0;
        if (!midix::format::rebase_image_address(plan.entry, plan.layout.minimum, loaded_base, &plan.entry)) return 0;
    }
    if (security::random_ready())
        plan.arena.cage_base = 0x3c000000 + security::random_below((0x02000000 - plan.arena.cage_size) / PAGE_SIZE + 1) * PAGE_SIZE;
    for (u32 i = 0; i < plan.count; ++i) {
        if (!load_plan(space, bytes, plan.segments[i])) {
            unwind_loaded(space, plan.segments, i + 1);
            return 0;
        }
    }
    // The relocation table is part of the image; it must fit inside the image
    // before a single record is read, or the reads below walk off the buffer and
    // the resulting values become patch addresses.
    if (plan.layout.count) {
        u64 table_end = 0;
        if (__builtin_mul_overflow(static_cast<u64>(plan.layout.count),
                                   static_cast<u64>(sizeof(MxImageRelocation)), &table_end) ||
            __builtin_add_overflow(table_end, plan.relocation_offset, &table_end) ||
            table_end > image_size) {
            unwind_loaded(space, plan.segments, plan.count); return 0;
        }
    }
    for (u32 i = 0; i < plan.layout.count; ++i) {
        MxImageRelocation r;
        libk::memcpy(&r, bytes + plan.relocation_offset + i * sizeof(r), sizeof(r));
        u64 address = 0;
        if (!midix::format::rebase_image_address(r.address, plan.layout.minimum, loaded_base, &address)) {
            unwind_loaded(space, plan.segments, plan.count); return 0;
        }
        PhysAddr physical;
        if (!mm::user_va_to_pa(space, address, RIGHT_READ, &physical)) {
            unwind_loaded(space, plan.segments, plan.count); return 0;
        }
        // Only the encoded widths the converter emits, and the patch has to stay
        // inside the page just resolved: the source is an 8-byte stack value, so
        // a larger count would read the kernel stack into physical memory.
        if ((r.width != 1 && r.width != 2 && r.width != 4 && r.width != 8) ||
            (address & (PAGE_SIZE - 1)) + r.width > PAGE_SIZE) {
            unwind_loaded(space, plan.segments, plan.count); return 0;
        }
        u64 value = 0;
        if (!midix::format::rebase_image_address(r.value, plan.layout.minimum, loaded_base, &value)) {
            unwind_loaded(space, plan.segments, plan.count); return 0;
        }
        if (r.width == 4 && value > 0xffffffff) { unwind_loaded(space, plan.segments, plan.count); return 0; }
        libk::memcpy(reinterpret_cast<void*>(physical), &value, r.width);
        mm::sync_icache_range(physical, r.width);
    }
    if (arena) *arena = plan.arena;
    return plan.entry;
}

VirtAddr load_image(VmSpace& space, const void* image_data, usize image_size, MxArenaCreate* arena) {
    if (!image_data || image_size < 4) {
        return 0;
    }
    const u32 magic = *static_cast<const u32*>(image_data);
    if (magic == MX_IMAGE_MAGIC) {
        return load_native_image(space, image_data, image_size, arena);
    }
    return 0;
}

} // namespace kernel::init
