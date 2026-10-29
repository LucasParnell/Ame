#pragma once
#include "midix/abi/types.h"
struct MxBootInfo;
#include "midix/abi/protocols/graphics.h"
namespace ame::graphics {
// Explicit boot capabilities; no ambient service lookup or Linux dependencies.
bool connect(const MxBootInfo* boot, const char* endpoint = "diag", mx_u64_t mapping = 0x33000000);
mx_u32_t batch_limit();
// Submission tickets retire command storage; GPU fences are API-specific.
bool enqueue(const void* bytes, mx_u32_t size, mx_u32_t api, mx_u32_t flags, mx_u64_t& ticket);
bool wait(mx_u64_t ticket, ame_gfx_reply& reply);
bool wait_all();
bool submit(const void* bytes, mx_u32_t size, mx_u32_t api, mx_u32_t flags, ame_gfx_reply& reply);
// Synchronous bidirectional RPC; requires the provisioned command arena.
bool exchange(void* bytes, mx_u32_t size, mx_u32_t api, mx_u32_t flags, ame_gfx_reply& reply);
const ame_gfx_reply& completion();
} // namespace ame::graphics
