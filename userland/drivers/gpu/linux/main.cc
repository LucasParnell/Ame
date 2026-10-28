// userland/drivers/gpu/linux/main.cc — drv.gpu.linux client and server halves.
//
// The client half is linked into io.framebuffer; the server half is linked into
// dango.manager (see gpu_linux.h for why there is no separate process). Both
// halves share this one translation unit so the request/reply shapes and the
// validation guards exist once.

#include "gpu_linux.h"

namespace drv::gpu::linux {
namespace {

// Clamp an untrusted damage rectangle to the window. Returns a zero-area
// rectangle when the request lies entirely outside.
MxGpuDamageRect clamp_damage(const MxGpuDamageRect& rect, uint32_t width, uint32_t height) {
    MxGpuDamageRect out{};
    if (rect.x >= width || rect.y >= height) return out;
    if (rect.width == 0 || rect.height == 0) return out;
    const uint32_t x1 = rect.x + (rect.width < width - rect.x ? rect.width : width - rect.x);
    const uint32_t y1 = rect.y + (rect.height < height - rect.y ? rect.height : height - rect.y);
    out.x = rect.x;
    out.y = rect.y;
    out.width = x1 - rect.x;
    out.height = y1 - rect.y;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
void Client::attach(const Endpoint& endpoint, const MxGpuServiceRegisterStagingRequest& window) {
    endpoint_ = endpoint;
    window_ = window;
    buffer_id_ = 0;
    register_sent_ = false;
    dropped_presents_ = 0;
    state_ = endpoint_.valid() ? ClientState::Idle : ClientState::Failed;
}

bool Client::push(uint32_t type, const void* payload, uint32_t length) {
    return endpoint_.valid() &&
           ring_try_push(endpoint_.request_ring, endpoint_.order, type, payload, length,
                         endpoint_.request_doorbell);
}

void Client::poll() {
    if (state_ == ClientState::Failed || !endpoint_.valid()) return;

    // Absorb replies first so a state change (INFO_REPLY) is visible below.
    RingSlot slot{};
    while (ring_pop(endpoint_.reply_ring, endpoint_.order, &slot)) {
        switch (slot.type) {
        case GPU_SERVICE_INFO_REPLY: {
            MxGpuServiceInfoReply reply{};
            if (!mochi::message_get(slot, &reply) || reply.status != MX_OK ||
                (reply.flags & MX_GPU_SERVICE_FLAG_LIVE) == 0) return fail();
            // The service must scan out exactly the window this client renders
            // for; a mismatch means the two ends disagree about the binding.
            if (reply.width != window_.width || reply.height != window_.height ||
                reply.stride != window_.stride ||
                reply.pixel_format != window_.pixel_format) {
                return fail();
            }
            if (state_ == ClientState::AwaitInfo) state_ = ClientState::AwaitRegister;
            break;
        }
        case GPU_SERVICE_REGISTER_STAGING_REPLY: {
            MxGpuServiceRegisterStagingReply reply{};
            if (!mochi::message_get(slot, &reply) || reply.status != MX_OK || reply.buffer_id == 0) {
                return fail();
            }
            if (state_ == ClientState::AwaitRegister) {
                buffer_id_ = reply.buffer_id;
                state_ = ClientState::Live;
            }
            break;
        }
        case GPU_SERVICE_PRESENT_COMPLETE: {
            MxGpuServicePresentComplete reply{};
            if (!mochi::message_get(slot, &reply) || reply.status != MX_OK) return fail();
            break;
        }
        case GPU_SERVICE_ERROR:
            return fail();
        default:
            break; // unknown reply: ignore, never trust the payload
        }
    }

    // Send phase: one request at a time, retried on the next poll when the
    // request ring is momentarily full.
    if (state_ == ClientState::Idle) {
        if (push(GPU_SERVICE_INFO, nullptr, 0)) state_ = ClientState::AwaitInfo;
    } else if (state_ == ClientState::AwaitRegister && !register_sent_) {
        if (push(GPU_SERVICE_REGISTER_STAGING, &window_, sizeof(window_))) register_sent_ = true;
    }
}

bool Client::submit_present(const MxGpuDamageRect* damage, bool full_frame) {
    if (state_ != ClientState::Live) return false;

    MxGpuServicePresentRequest request{};
    request.buffer_id = buffer_id_;
    request.flags = full_frame ? MX_GPU_SERVICE_PRESENT_FLAG_FULL_FRAME
                               : MX_GPU_SERVICE_PRESENT_FLAG_NONE;
    request.damage_count = (damage && !full_frame) ? 1u : 0u;
    if (request.damage_count == 1u) request.damage = *damage;

    if (push(GPU_SERVICE_PRESENT, &request, sizeof(request))) {
        dropped_presents_ = 0;
        return true;
    }
    // Full request ring: absorb completions and retry once. A present that
    // still cannot be queued is not fatal -- the caller keeps its damage, so
    // the next present carries the same rectangle.
    poll();
    if (state_ != ClientState::Live) return false;
    if (push(GPU_SERVICE_PRESENT, &request, sizeof(request))) {
        dropped_presents_ = 0;
        return true;
    }
    // Bounded: a service that never drains its reply ring must degrade the
    // client to its local path instead of swallowing presents forever.
    if (++dropped_presents_ >= kMaxConsecutiveDroppedPresents) {
        fail();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
void Server::init(const Endpoint& endpoint, const ServerConfig& config) {
    endpoint_ = endpoint;
    config_ = config;
    buffer_id_ = 0;
    boot_generation_ = 0;
    frame_index_ = 0;
    consecutive_failures_ = 0;
}

void Server::set_buffer(uint64_t buffer_id, uint64_t boot_generation) {
    buffer_id_ = buffer_id;
    boot_generation_ = boot_generation;
}

bool Server::reply(uint32_t type, const void* payload, uint32_t length) {
    return endpoint_.valid() &&
           ring_try_push(endpoint_.reply_ring, endpoint_.order, type, payload, length,
                         endpoint_.reply_doorbell);
}

void Server::error(uint32_t opcode, mx_status_t status, uint64_t detail) {
    MxGpuServiceError err{};
    err.status = status;
    err.opcode = opcode;
    err.detail = detail;
    (void)reply(GPU_SERVICE_ERROR, &err, sizeof(err));
}

bool Server::serve_once(PresentFn present, void* ctx) {
    if (!endpoint_.valid()) return false;

    RingSlot slot{};
    if (!ring_wait_pop(endpoint_.request_ring, endpoint_.order, &slot,
                       endpoint_.request_doorbell)) {
        return false;
    }

    switch (slot.type) {
    case GPU_SERVICE_INFO: {
        MxGpuServiceInfoReply info{};
        info.status = MX_OK;
        info.flags = MX_GPU_SERVICE_FLAG_LIVE;
        info.boot_generation = boot_generation_;
        info.width = config_.width;
        info.height = config_.height;
        info.stride = config_.stride;
        info.pixel_format = config_.pixel_format;
        (void)reply(GPU_SERVICE_INFO_REPLY, &info, sizeof(info));
        break;
    }
    case GPU_SERVICE_REGISTER_STAGING: {
        MxGpuServiceRegisterStagingRequest request{};
        if (!mochi::message_get(slot, &request)) {
            error(GPU_SERVICE_REGISTER_STAGING, MX_ERR_BAD_PROTOCOL, slot.length);
            break;
        }
        // Guard: a client that was built against a different appliance binding
        // is refused rather than silently mis-presented.
        if (request.staging_pa != config_.staging_pa ||
            request.staging_size != config_.staging_size ||
            request.width != config_.width || request.height != config_.height ||
            request.stride != config_.stride ||
            request.pixel_format != config_.pixel_format) {
            error(GPU_SERVICE_REGISTER_STAGING, MX_ERR_INVALID, request.staging_pa);
            break;
        }
        if (!buffer_ready()) {
            error(GPU_SERVICE_REGISTER_STAGING, MX_ERR_SERVICE_UNAVAILABLE, 0);
            break;
        }
        MxGpuServiceRegisterStagingReply registered{};
        registered.status = MX_OK;
        registered.buffer_id = buffer_id_;
        registered.flags = request.flags;
        (void)reply(GPU_SERVICE_REGISTER_STAGING_REPLY, &registered, sizeof(registered));
        break;
    }
    case GPU_SERVICE_PRESENT: {
        MxGpuServicePresentRequest request{};
        if (!mochi::message_get(slot, &request)) {
            error(GPU_SERVICE_PRESENT, MX_ERR_BAD_PROTOCOL, slot.length);
            break;
        }
        if (!buffer_ready() || request.buffer_id != buffer_id_) {
            error(GPU_SERVICE_PRESENT, MX_ERR_INVALID, request.buffer_id);
            break;
        }
        const bool full_frame = (request.flags & MX_GPU_SERVICE_PRESENT_FLAG_FULL_FRAME) != 0;
        MxGpuDamageRect damage{};
        if (full_frame) {
            // Whole-window present: the client's first frame after attaching.
            damage.width = config_.width;
            damage.height = config_.height;
        } else if (request.damage_count == 1) {
            damage = clamp_damage(request.damage, config_.width, config_.height);
        } else if (request.damage_count != 0) {
            error(GPU_SERVICE_PRESENT, MX_ERR_INVALID, request.damage_count);
            break;
        }
        // damage_count == 0 without FULL_FRAME means nothing changed; that still
        // gets an ack below so every queued present has a completion.
        if (damage.width != 0 && damage.height != 0) {
            if (!present || !present(ctx, damage, request.flags)) {
                if (++consecutive_failures_ >= kMaxConsecutivePresentFailures) {
                    // Bounded: stop reporting success so the client degrades.
                    error(GPU_SERVICE_PRESENT, MX_ERR_DEAD, frame_index_);
                } else {
                    error(GPU_SERVICE_PRESENT, MX_ERR_SERVICE_UNAVAILABLE, frame_index_);
                }
                break;
            }
            consecutive_failures_ = 0;
            ++frame_index_;
        }

        // Nothing to draw still gets an ack, so the client's present loop keeps
        // a definite completion for every request it queued.
        MxGpuServicePresentComplete complete{};
        complete.status = MX_OK;
        complete.buffer_id = buffer_id_;
        complete.frame_index = frame_index_;
        complete.flags = request.flags;
        (void)reply(GPU_SERVICE_PRESENT_COMPLETE, &complete, sizeof(complete));
        break;
    }
    default:
        error(slot.type, MX_ERR_BAD_PROTOCOL, slot.length);
        break;
    }
    return true;
}

} // namespace drv::gpu::linux
