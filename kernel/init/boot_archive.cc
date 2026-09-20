// External signed MXB bundles are the sole boot archive format.
#include "anko/init.h"
#include "anko/log.h"
#include "anko/fdt.h"
#include "midix/format/bundle.h"
#include "manifest_trust.h"
#include "libk/string.h"
namespace kernel::init {
namespace {
midix::format::Bundle g_archive{};
struct InitrdVisitorCtx {
    i64 chosen_depth = -1;
    PhysAddr initrd_start = 0;
    PhysAddr initrd_end = 0;
};

static bool initrd_begin_node(void* v, const char* name, u32 depth) {
    auto& ctx = *static_cast<InitrdVisitorCtx*>(v);
    if (depth == 1 && libk::strcmp(name, "chosen") == 0) {
        ctx.chosen_depth = static_cast<i64>(depth);
        ANKO_LOG_DEBUG("kernel/dtb", "chosen");
    }
    return true;
}

static void initrd_end_node(void* v, u32 depth) {
    auto& ctx = *static_cast<InitrdVisitorCtx*>(v);
    if (static_cast<i64>(depth) == ctx.chosen_depth) {
        ctx.chosen_depth = -1;
    }
}

static void initrd_prop(void* v, const char* name, const u8* data, u32 len, u32 depth) {
    auto& ctx = *static_cast<InitrdVisitorCtx*>(v);
    if (ctx.chosen_depth < 0 || static_cast<i64>(depth) != ctx.chosen_depth) {
        return;
    }
    if (libk::strcmp(name, "linux,initrd-start") == 0 && (len == 4 || len >= 8)) {
        ctx.initrd_start = (len == 4) ? static_cast<PhysAddr>(libk::be32(data)) : libk::be64(data);
        ANKO_LOGF_DEBUG("kernel/dtb", "initrd-start len=%u", len);
    } else if (libk::strcmp(name, "linux,initrd-end") == 0 && (len == 4 || len >= 8)) {
        ctx.initrd_end = (len == 4) ? static_cast<PhysAddr>(libk::be32(data)) : libk::be64(data);
        ANKO_LOGF_DEBUG("kernel/dtb", "initrd-end len=%u", len);
    }
}

} // namespace

bool discover_initrd(PhysAddr dtb_phys, PhysAddr* out_base, usize* out_size) {
    if (!dtb_phys || !out_base || !out_size) {
        return false;
    }

    const auto* dtb = reinterpret_cast<const u8*>(dtb_phys);
    InitrdVisitorCtx ctx;
    if (!fdt::walk(dtb, {&ctx, initrd_begin_node, initrd_end_node, initrd_prop})) {
        ANKO_LOG_WARN("kernel/dtb", "bad or malformed DTB");
        return false;
    }

    if (ctx.initrd_start == 0 || ctx.initrd_end <= ctx.initrd_start) {
        // No DTB initrd is normal for disk/U-Boot boots; callers fall back to
        // loading /ame/boot/system.mxb from the boot disk.
        return false;
    }

    *out_base = ctx.initrd_start;
    *out_size = static_cast<usize>(ctx.initrd_end - ctx.initrd_start);
    return true;
}

bool init_archive(PhysAddr base, usize size) {
    const char* error = midix::format::verify_bundle(
        reinterpret_cast<const u8*>(base), size, kManifestTrustedKeys,
        sizeof(kManifestTrustedKeys) / sizeof(kManifestTrustedKeys[0]), g_archive);
    if (error) {
        ANKO_LOGF_ERROR("kernel/init", "bundle rejected: %s", error);
        return false;
    }
    ANKO_LOGF_INFO("kernel/init", "authenticated MXB bundle objects=%u",
                   g_archive.header.object_count);
    return true;
}
bool archive_get(const char* wanted, const void** data, usize* size) {
    if (!wanted || !data || !size || !g_archive.data) {
        return false;
    }
    for (u32 i = 0; i < g_archive.header.object_count; ++i) {
        MxBundleObject o;
        const char* name;
        if (!midix::format::object(g_archive, i, o, name)) {
            return false;
        }
        if (libk::strcmp(wanted, name) != 0) {
            continue;
        }
        if (i == 0) {
            *data = g_archive.manifest;
            *size = g_archive.manifest_size;
        } else {
            *data = g_archive.data + o.offset;
            *size = o.size;
        }
        return true;
    }
    return false;
}
} // namespace kernel::init
