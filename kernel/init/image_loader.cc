// Native MXI executable loader. ELF is accepted only by the host converter.
#include "anko/init.h"
#include "anko/integrity.h"
#include "midix/format/image.h"
#include "anko/log.h"
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

VirtAddr load_native_image(VmSpace& space, const void* image_data, usize image_size) {
    midix::format::ImagePlan plan;
    const auto* bytes = static_cast<const u8*>(image_data);
    if (const char* error = midix::format::parse_image(bytes, image_size, plan)) {
        ANKO_LOGF_ERROR("kernel/loader", "image validation: %s", error);
        return 0;
    }
    for (u32 i = 0; i < plan.count; ++i) {
        if (!load_plan(space, bytes, plan.segments[i])) {
            unwind_loaded(space, plan.segments, i + 1);
            return 0;
        }
    }
    return plan.entry;
}

VirtAddr load_image(VmSpace& space, const void* image_data, usize image_size) {
    if (!image_data || image_size < 4) {
        return 0;
    }
    const u32 magic = *static_cast<const u32*>(image_data);
    if (magic == MX_IMAGE_MAGIC) {
        return load_native_image(space, image_data, image_size);
    }
    return 0;
}

} // namespace kernel::init
