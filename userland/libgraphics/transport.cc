#include "ame/graphics.h"
#include "mochi/driver.h"
namespace ame::graphics {
static mochi::SpscEndpoint endpoint;
static mx_u64_t sequence;
static ame_gfx_reply last;
static mx_u8_t* commands;
bool connect(const MxBootInfo* boot, const char* name, mx_u64_t mapping) {
    if (!endpoint.valid()) {
        endpoint = mochi::driver::link(boot, name, mapping);
    }
    if (endpoint.valid() && !commands) {
        const auto cap = mochi::boot::handle(boot, "gfx.commands");
        MxCapInfo info{};
        if (cap && mx_cap_info(cap, &info) == MX_OK && info.size == AME_GFX_COMMAND_BYTES &&
            mx_map_page(cap, 0x36000000, MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
            commands = reinterpret_cast<mx_u8_t*>(0x36000000);
        }
    }
    return endpoint.valid();
}
mx_u32_t batch_limit() {
    return commands ? AME_GFX_BATCH_BYTES : 1536;
}
struct Pending {
    mx_u64_t id;
    bool done;
    ame_gfx_reply reply;
} pending[AME_GFX_COMMAND_SLOTS];
static bool poisoned;
bool wait(mx_u64_t ticket, ame_gfx_reply& reply) {
    if (poisoned) {
        return false;
    }
    auto& target = pending[ticket % AME_GFX_COMMAND_SLOTS];
    if (target.id != ticket) {
        return false;
    }
    const auto deadline = mx_clock_ns() + 40000000000ull;
    while (!target.done) {
        MxRingSlot slot{};
        if (!mochi::driver::receive(endpoint, slot, deadline)) {
            poisoned = true;
            return false;
        }
        ame_gfx_reply done{};
        if (!mochi::message_get(slot, &done, AME_GFX_REPLY)) {
            continue;
        }
        auto& entry = pending[done.id % AME_GFX_COMMAND_SLOTS];
        if (entry.id == done.id) {
            entry.reply = done;
            entry.done = true;
        }
    }
    reply = target.reply;
    last = reply;
    target = {};
    return reply.status == 0;
}
bool wait_all() {
    bool ok = !poisoned;
    for (unsigned n = 0; n < AME_GFX_COMMAND_SLOTS; ++n) {
        mx_u64_t first = ~mx_u64_t{0};
        for (const auto& entry : pending) {
            if (entry.id && entry.id < first) {
                first = entry.id;
            }
        }
        if (first == ~mx_u64_t{0}) {
            break;
        }
        ame_gfx_reply reply{};
        if (!wait(first, reply)) {
            ok = false;
        }
    }
    return ok;
}
bool enqueue(const void* data, mx_u32_t size, mx_u32_t api, mx_u32_t flags, mx_u64_t& ticket) {
    if (poisoned || !endpoint.valid() || size > batch_limit() || (size && !data)) {
        return false;
    }
    const mx_u64_t id = sequence + 1, deadline = mx_clock_ns() + 40000000000ull;
    const mx_u32_t index = id % AME_GFX_COMMAND_SLOTS;
    if (pending[index].id || (!commands && (pending[0].id || pending[1].id))) {
        return false;
    }
    if (!id) {
        poisoned = true;
        return false;
    }
    const auto* bytes = static_cast<const mx_u8_t*>(data);
    const ame_gfx_submit request{id, AME_GFX_VERSION, api, size, flags};
    if (commands) {
        for (mx_u32_t i = 0; i < size; ++i) {
            commands[index * AME_GFX_BATCH_BYTES + i] = bytes[i];
        }
        const ame_gfx_shared_submit shared{request, index, 0};
        if (!endpoint.tx().push_bounded(AME_GFX_SHARED, &shared, sizeof(shared), deadline)) {
            poisoned = true;
            return false;
        }
    } else {
        // Inline fallback remains for clients without a provisioned upload arena.
        for (mx_u32_t offset = 0; offset < size;) {
            ame_gfx_chunk chunk{id, offset, size - offset > 32 ? 32 : size - offset, {}};
            for (mx_u32_t i = 0; i < chunk.size; ++i) {
                chunk.data[i] = bytes[offset + i];
            }
            if (!endpoint.tx().push_bounded(AME_GFX_CHUNK, &chunk, sizeof(chunk), deadline)) {
                poisoned = true;
                return false;
            }
            offset += chunk.size;
        }
        if (!endpoint.tx().push_bounded(AME_GFX_SUBMIT, &request, sizeof(request), deadline)) {
            poisoned = true;
            return false;
        }
    }
    sequence = id;
    pending[index] = {id, false, {}};
    ticket = id;
    return true;
}
bool submit(const void* data, mx_u32_t size, mx_u32_t api, mx_u32_t flags, ame_gfx_reply& reply) {
    mx_u64_t ticket = 0;
    return wait_all() && enqueue(data, size, api, flags, ticket) && wait(ticket, reply);
}
const ame_gfx_reply& completion() {
    return last;
}
bool exchange(void* data, mx_u32_t size, mx_u32_t api, mx_u32_t flags, ame_gfx_reply& reply) {
    if (!commands || !wait_all()) return false;
    mx_u64_t ticket=0;
    if (!enqueue(data,size,api,flags,ticket) || !wait(ticket,reply)) return false;
    const auto* response=commands+(ticket%AME_GFX_COMMAND_SLOTS)*AME_GFX_BATCH_BYTES;
    for(mx_u32_t i=0;i<size;i++) static_cast<mx_u8_t*>(data)[i]=response[i];
    return true;
}
} // namespace ame::graphics
