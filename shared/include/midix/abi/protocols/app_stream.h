#pragma once
#include "midix/abi/types.h"

// AMK1 framing is shared with USB key injection; application message types
// occupy a separate range. Frames are LE and retain the existing IEEE CRC32.
namespace midix::apps {
constexpr mx_u16_t kHello = 0x100, kBegin = 0x101, kData = 0x102,
                   kRun = 0x103, kPoll = 0x104, kCancel = 0x105;
constexpr mx_u32_t kMaxImage = 8 * 1024 * 1024;
constexpr mx_u32_t kArenaBytes = 256 * 1024;
constexpr mx_u64_t kUploadTimeoutNs = 10'000'000'000ull;
constexpr mx_u32_t kDefaultRunMs = 30'000, kMaxRunMs = 300'000;
constexpr mx_i32_t kNoExit = (-2147483647 - 1);
enum State : mx_u32_t { Closed, Ready, Receiving, Running, Stopping };
constexpr mx_u32_t kStart = 0x41505001, kStop = 0x41505002,
                   kStatus = 0x41505003, kReply = 0x41505004;
struct Control { mx_u64_t token; };
struct Reply {
    mx_u64_t token, session;
    mx_i32_t status, exit_code;
    mx_u32_t state, received;
};
static_assert(sizeof(Reply) == 32);
inline bool wire_type(mx_u16_t type) { return type >= kHello && type <= kCancel; }
} // namespace midix::apps
