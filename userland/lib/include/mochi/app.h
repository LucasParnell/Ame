#pragma once
// Startup contract shared by filesystem and streamed native app launchers.
#include "mochi/boot.h"
#include "mochi/stdout.h"
#include "midix/abi/protocols/app_stream.h"
#include "libk/bytes.h"
#include "midix/format/app_package.h"

namespace mochi::app {
inline bool add(MxBootInfo* boot, MxBootEntryType type, const char* name, mx_handle_t handle = 0,
                mx_u64_t value = 0) {
    if (!boot || !name || !name[0] ||
        libk::str::length_n(name, MX_BOOT_NAME_MAX) == MX_BOOT_NAME_MAX ||
        boot->entry_count >= MX_BOOT_MAX_ENTRIES)
        return false;
    // Builders publish magic only at resume, so boot::find() is intentionally
    // unavailable here. Reject duplicate names in the unpublished page too.
    for (mx_u32_t i = 0; i < boot->entry_count; ++i)
        if (libk::str::equal(boot->entries[i].name, name))
            return false;
    auto& entry = boot->entries[boot->entry_count++];
    entry = {};
    entry.type = type;
    entry.handle = handle;
    entry.aux0 = value;
    libk::str::copy_into(entry.name, sizeof(entry.name), name);
    return true;
}
inline void image_name(char (&name)[32], mx_u32_t index) {
    libk::str::copy_into(name, sizeof(name), "app.image.00");
    name[10] = static_cast<char>('0' + index / 10);
    name[11] = static_cast<char>('0' + index % 10);
}

// Copy the executable into read-only child-owned snapshots. Upload/shell
// buffers can then be wiped and reused without changing the child's image.
inline bool publish_image(MxBootInfo* boot, mx_handle_t child, const mx_u8_t* data, mx_u32_t size,
                          mx_vaddr_t scratch = va::app::kImageScratch) {
    using namespace midix::apps;
    if (!size || size > kMaxImage ||
        boot->entry_count + (size + kArenaBytes - 1) / kArenaBytes + 1 > MX_BOOT_MAX_ENTRIES)
        return false;
    for (mx_u32_t offset = 0; offset < size; offset += kArenaBytes) {
        const auto count = size - offset < kArenaBytes ? size - offset : kArenaBytes;
        const auto source = sys_arena_create(count, 0);
        if (source <= 0)
            return false;
        const bool mapped = mx_map_page(source, scratch, RIGHT_READ | RIGHT_WRITE) == MX_OK;
        mx_i64_t shared = MX_ERR_NOMEM;
        if (mapped) {
            libk::copy_exact(libk::as_bytes(reinterpret_cast<void*>(scratch), count),
                             libk::as_bytes(data + offset, count));
            shared = sys_cap_grant(child, source, RIGHT_READ | RIGHT_MAP);
            for (mx_u32_t page = 0; page < count; page += 4096)
                sys_unmap_page(scratch + page);
        }
        sys_cap_revoke(source);
        if (shared <= 0)
            return false;
        char name[32];
        image_name(name, offset / kArenaBytes);
        add(boot, MX_BOOT_ENTRY_ARENA, name, shared, count);
    }
    return add(boot, MX_BOOT_ENTRY_VALUE, "app.image.size", 0, size);
}

inline const mx_u8_t* map_image(const MxBootInfo* boot, mx_vaddr_t address, mx_u64_t& size) {
    using namespace midix::apps;
    const auto* total = boot::find(boot, "app.image.size");
    size = total ? total->aux0 : 0;
    if (!size || size > kMaxImage)
        return nullptr;
    mx_u32_t offset = 0;
    for (; offset < size; offset += kArenaBytes) {
        char name[32];
        image_name(name, offset / kArenaBytes);
        const auto* entry = boot::find(boot, name);
        const auto count = size - offset < kArenaBytes ? size - offset : kArenaBytes;
        MxCapInfo info{};
        if (!entry || entry->aux0 != count || mx_cap_info(entry->handle, &info) != MX_OK ||
            info.type != MX_CAP_ARENA || info.size != ((count + 4095) & ~mx_u64_t{4095}) ||
            (info.rights & RIGHT_WRITE) ||
            mx_map_page(entry->handle, address + offset, RIGHT_READ) != MX_OK)
            break;
    }
    if (offset >= size)
        return reinterpret_cast<const mx_u8_t*>(address);
    for (mx_u32_t page = 0; page < offset; page += 4096)
        sys_unmap_page(address + page);
    return nullptr;
}

// Application entry API. start() is idempotent: callers can share library
// startup helpers without mapping BootInfo or stdout a second time.
struct Image {
    const mx_u8_t* data = nullptr;
    mx_u64_t size = 0;
    explicit operator bool() const {
        return data && size;
    }
};
struct Context {
    const MxBootInfo* boot = nullptr;
    Image image{};
    Image snapshot{};
    midix::package::View package{};
    mx_u64_t value(const char* name, mx_u64_t fallback = 0) const {
        const auto* entry = boot::find(boot, name);
        return entry && entry->type == MX_BOOT_ENTRY_VALUE ? entry->aux0 : fallback;
    }
    mx_handle_t supervisor() const {
        return boot::handle(boot, "supervisor");
    }
    mx_u32_t cpu_count() const {
        return static_cast<mx_u32_t>(value("cpu.count", 1));
    }
    mx_u64_t cpu_mask() const {
        return value("cpu.mask", 1);
    }
    Image self_image() {
        if (image.data) return image;
        if (!snapshot.data)
            snapshot.data = map_image(boot, va::app::kSelfImage, snapshot.size);
        if (midix::package::has_magic(snapshot.data, snapshot.size)) {
            if (!midix::package::parse(snapshot.data, snapshot.size, package)) return {};
            const auto executable = package.executable();
            image = {executable.data, executable.size};
        } else image = snapshot;
        return image;
    }
    // Bytes remain valid for this process lifetime; no filesystem authority is
    // needed to read resources in its own immutable package snapshot.
    midix::package::Bytes resource(const char* name) {
        (void)self_image();
        return package.find(name);
    }
};
inline Context& start() {
    static Context context{};
    if (!context.boot) {
        context.boot = boot::map();
        if (boot::handle(context.boot, "log.ring") &&
            !appio::attach(context.boot, va::app::kStdout))
            boot::invalid_boot();
        boot::ready(context.boot);
    }
    return context;
}
inline mx_u32_t write(const void* data, mx_u32_t length) {
    return appio::write(data, length);
}
inline void puts(const char* text) {
    appio::puts(text);
}
using appio::put_dec;
using appio::put_hex;
using appio::putc;
using appio::try_write;
[[noreturn]] inline void exit(mx_i32_t status) {
    mx_exit(status);
    for (;;)
        mx_yield();
}
} // namespace mochi::app
