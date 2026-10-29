#pragma once
#include "midix/abi/service.h"

constexpr mx_u32_t MX_SESSION_MAGIC = 0x53455331;
constexpr mx_u32_t MX_MAX_SESSIONS = 8;
constexpr mx_u32_t MX_MAX_EXPORTS = 32;
constexpr mx_u32_t MX_SESSION_FIDO2_PRIVATE = 1u << 0; // init-issued FS namespace authority

// Init publishes generation last. The server resets both rings and releases
// handles from the preceding generation, then acknowledges before client resume.
struct MxSessionSlot {
    mx_u64_t generation;
    mx_u64_t acknowledged;
    mx_handle_t client;
    mx_handle_t request_ring;
    mx_handle_t reply_ring;
    mx_handle_t request_doorbell;
    mx_handle_t reply_doorbell;
    mx_u32_t order;
    mx_u32_t reserved; // MX_SESSION_* policy flags, written only by init
    mx_handle_t write_arena;      // server-local, optional 64 KiB FS upload arena
    mx_handle_t client_write_arena; // client-local token, never used as server authority
};
struct MxServiceExport {
    char name[32];
    mx_handle_t process;
    mx_u32_t protocol;
    mx_u32_t rights;
};
struct MxSessionDirectory {
    mx_u32_t magic;
    mx_u32_t count;
    MxSessionSlot slots[MX_MAX_SESSIONS];
    MxServiceExport exports[MX_MAX_EXPORTS];
};
static_assert(sizeof(MxSessionDirectory) <= 4096);
