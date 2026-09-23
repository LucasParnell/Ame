#pragma once
#include "mochi/syscall.h"
#include "mochi/status.h"

namespace mochi {

struct UntypedTag {};
struct ProcessTag {};
struct ThreadTag {};
struct RingTag {};
struct DoorbellTag {};
struct ArenaTag {};
struct UartTag {};

template <typename Tag = UntypedTag>
class Handle {
public:
    constexpr Handle() : h_(MX_NULL_HANDLE) {}
    explicit constexpr Handle(mx_handle_t h) : h_(h) {}

    constexpr mx_handle_t raw() const { return h_; }
    constexpr bool valid() const { return h_ != MX_NULL_HANDLE; }
    constexpr explicit operator bool() const { return valid(); }

    // --- capability operations ---------------------------------------------
    // A capability names an object the kernel mediates (a ring, a doorbell, an
    // arena, a device range...). These are the five things a user does with one;
    // the typed tag above is documentation, and the object type is the truth.

    // Object type this capability names (MX_CAP_*), or MX_CAP_NULL if the
    // handle is invalid or the kernel does not know it.
    mx_u16_t type() const {
        mx_i64_t kind = MX_CAP_NULL;
        return (valid() && mx_cap_identify(h_, &kind) == MX_OK)
                   ? static_cast<mx_u16_t>(kind)
                   : static_cast<mx_u16_t>(MX_CAP_NULL);
    }

    // Human name for type(), from the shared ABI table (no switch here).
    const char* type_name() const { return mx_cap_type_name(type()); }

    // Trusted metadata snapshot: base, size, rights, attr, flags, aux0.
    Status info(MxCapInfo* out) const {
        if (!valid() || !out) return {MX_ERR_INVALID};
        return {mx_cap_info(h_, out)};
    }

    // Map the object into this process and hand back a typed pointer. The
    // default `T = void` yields a void*; name the mapped type to skip a cast.
    template <typename T = void>
    mx_status_t map(mx_vaddr_t va, mx_rights_t rights, T** out = nullptr) const {
        if (!valid()) return MX_ERR_INVALID;
        const mx_status_t status = mx_map_page(h_, va, rights);
        if (status == MX_OK && out) *out = reinterpret_cast<T*>(va);
        return status;
    }

    // A second handle to the same object, with `rights` (a subset of this one).
    mx_status_t dup(mx_rights_t rights, Handle* out) const {
        if (!valid()) return MX_ERR_INVALID;
        mx_handle_t copy = MX_NULL_HANDLE;
        const mx_status_t status = mx_cap_dup(h_, rights, &copy);
        if (status == MX_OK && out) *out = Handle(copy);
        return status;
    }

    // Drop authority: revoke every handle derived from this object.
    mx_status_t revoke() const {
        return valid() ? static_cast<mx_status_t>(sys_cap_revoke(h_)) : MX_ERR_INVALID;
    }

private:
    mx_handle_t h_;
};

using AnyHandle = Handle<UntypedTag>;
using ProcessHandle = Handle<ProcessTag>;
using ThreadHandle = Handle<ThreadTag>;
using RingHandle = Handle<RingTag>;
using DoorbellHandle = Handle<DoorbellTag>;
using ArenaHandle = Handle<ArenaTag>;
using UartHandle = Handle<UartTag>;

static inline Status identify(mx_handle_t h, mx_i64_t* out_type) {
    return {mx_cap_identify(h, out_type)};
}

static inline bool is_type(mx_handle_t h, mx_i64_t type) {
    mx_i64_t got = 0;
    return identify(h, &got).ok() && got == type;
}

} // namespace mochi
