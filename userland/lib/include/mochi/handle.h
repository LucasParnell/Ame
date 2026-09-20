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
