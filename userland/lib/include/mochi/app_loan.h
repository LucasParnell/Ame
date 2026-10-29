#pragma once
#include "mochi/app_process.h"
#include "mochi/loan.h"

namespace mochi::app {
// Parent-owned arena. Stop both peers and drop their mappings before close or
// reuse; capability revocation alone does not retract existing mappings.
class SharedLoanPool {
    mx_handle_t arena_ = 0;
    uint32_t count_ = 0, stride_ = 0;
    static bool name(char (&out)[MX_BOOT_NAME_MAX], const char* prefix, const char* suffix) {
        if (!prefix || !*prefix)
            return false;
        libk::str::Buffer text(out);
        text.append(prefix);
        text.append(suffix);
        return !text.truncated;
    }

  public:
    SharedLoanPool() = default;
    SharedLoanPool(const SharedLoanPool&) = delete;
    SharedLoanPool& operator=(const SharedLoanPool&) = delete;
    ~SharedLoanPool() {
        close();
    }
    void close() {
        if (arena_)
            sys_cap_revoke(arena_);
        arena_ = count_ = stride_ = 0;
    }
    mx_status_t create(uint32_t buffer_bytes = 4096, uint32_t count = 32) {
        if (arena_ || !count || count > 64 || !buffer_bytes || buffer_bytes > 65536)
            return MX_ERR_INVALID;
        const uint32_t stride = (buffer_bytes + 63u) & ~63u;
        const auto cap = sys_arena_create(uint64_t{stride} * count, 0);
        if (cap <= 0)
            return static_cast<mx_status_t>(cap);
        arena_ = static_cast<mx_handle_t>(cap);
        count_ = count;
        stride_ = stride;
        return MX_OK;
    }
    mx_status_t share(Process& child, bool writer, const char* prefix = "loan") const {
        if (!arena_)
            return MX_ERR_INVALID;
        char names[4][MX_BOOT_NAME_MAX]{};
        constexpr const char* suffixes[] = {".arena", ".count", ".stride", ".cookie"};
        for (uint32_t i = 0; i < 4; ++i)
            if (!name(names[i], prefix, suffixes[i]))
                return MX_ERR_INVALID;
        auto rc = child.share_arena(names[0], arena_,
                                    RIGHT_READ | RIGHT_MAP | (writer ? RIGHT_WRITE : 0));
        if (rc == MX_OK)
            rc = child.value(names[1], count_);
        if (rc == MX_OK)
            rc = child.value(names[2], stride_);
        if (rc == MX_OK)
            rc = child.value(names[3], arena_);
        return rc;
    }
    // All arena pages are mapped by MAP_PAGE. The caller owns unmapping the
    // page-rounded memory.bytes() extent after all loan objects are finished.
    static LoanMemory map(const MxBootInfo* boot, mx_vaddr_t address, bool writer,
                          const char* prefix = "loan") {
        char names[4][MX_BOOT_NAME_MAX]{};
        constexpr const char* suffixes[] = {".arena", ".count", ".stride", ".cookie"};
        for (uint32_t i = 0; i < 4; ++i)
            if (!name(names[i], prefix, suffixes[i]))
                return {};
        const auto* arena = boot::find(boot, names[0]);
        const auto* count = boot::find(boot, names[1]);
        const auto* stride = boot::find(boot, names[2]);
        const auto* cookie = boot::find(boot, names[3]);
        if (!address || (address & 4095) || !arena || arena->type != MX_BOOT_ENTRY_ARENA ||
            !count || !stride || !cookie || count->aux0 > 64 || stride->aux0 > 65536)
            return {};
        LoanMemory memory{reinterpret_cast<void*>(address), static_cast<uint32_t>(count->aux0),
                          static_cast<uint32_t>(stride->aux0), cookie->aux0};
        const auto bytes = (memory.bytes() + 4095) & ~uint64_t{4095};
        MxCapInfo info{};
        if (!memory.valid() || address > ~uint64_t{0} - bytes ||
            mx_cap_info(arena->handle, &info) != MX_OK || info.type != MX_CAP_ARENA ||
            info.size != bytes)
            return {};
        if (mx_map_page(arena->handle, address, RIGHT_READ | (writer ? RIGHT_WRITE : 0)) != MX_OK)
            return {};
        return memory;
    }
    static LoanWriter map_writer(const MxBootInfo* boot, mx_vaddr_t address,
                                 const char* prefix = "loan") {
        const auto memory = map(boot, address, true, prefix);
        return LoanWriter(const_cast<void*>(memory.data), memory.count, memory.stride,
                          memory.cookie);
    }
    static LoanReader map_reader(const MxBootInfo* boot, mx_vaddr_t address,
                                 const char* prefix = "loan") {
        return LoanReader(map(boot, address, false, prefix));
    }
};
} // namespace mochi::app
