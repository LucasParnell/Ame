// userland/lib/vulkan/session_client.cc — the native Vulkan session client and
// the `ame_remote` command marshalling (implementation of dango/vulkan.h).
//
// Transport-abstract and allocation-free: it talks only through the Transport
// callbacks and writes command records into a caller-owned byte region. It is
// compiled into the mochi userland (as an object library, so a stray Linux or
// host dependency fails the freestanding build) and, unchanged, into the host
// transport regression.

#include "dango/vulkan.h"

namespace dango::vk {
namespace {

// Write `n` bytes of a zeroed record body without a memcpy dependency (the
// freestanding userland links no libc).
void zero_range(mx_u8_t* dst, mx_u32_t n) {
    for (mx_u32_t i = 0; i < n; ++i) dst[i] = 0;
}

} // namespace

// --- CommandRecorder (the ame_remote marshalling) --------------------------

mx_u8_t* CommandRecorder::reserve(mx_u32_t record_bytes) {
    if (!region_) return nullptr;
    const mx_u32_t aligned =
        (size_ + (MX_VK_COMMAND_ALIGNMENT - 1)) & ~(MX_VK_COMMAND_ALIGNMENT - 1);
    if (record_bytes == 0 || record_bytes > capacity_ || aligned > capacity_ - record_bytes) {
        overflow_ = true;
        return nullptr;
    }
    zero_range(region_ + aligned, record_bytes);
    size_ = aligned + record_bytes;
    return region_ + aligned;
}

bool CommandRecorder::fill_buffer(const Memory& dst, mx_u64_t offset, mx_u64_t size,
                                  mx_u32_t value) {
    if (!dst.valid()) return false;
    auto* record = reinterpret_cast<MxVkCmdFillBuffer*>(reserve(sizeof(MxVkCmdFillBuffer)));
    if (!record) return false;
    record->header.opcode = VK_CMD_FILL_BUFFER;
    record->header.size = sizeof(MxVkCmdFillBuffer);
    record->header.flags = 0;
    record->dest_arena_offset = dst.arena_offset;
    record->offset = offset;
    record->size = size;
    record->value = value;
    return true;
}

bool CommandRecorder::copy_buffer(const Memory& src, mx_u64_t src_offset, const Memory& dst,
                                  mx_u64_t dst_offset, mx_u64_t size) {
    if (!src.valid() || !dst.valid()) return false;
    auto* record = reinterpret_cast<MxVkCmdCopyBuffer*>(reserve(sizeof(MxVkCmdCopyBuffer)));
    if (!record) return false;
    record->header.opcode = VK_CMD_COPY_BUFFER;
    record->header.size = sizeof(MxVkCmdCopyBuffer);
    record->header.flags = 0;
    record->src_arena_offset = src.arena_offset + src_offset;
    record->dst_arena_offset = dst.arena_offset + dst_offset;
    record->size = size;
    return true;
}

bool CommandRecorder::dispatch(mx_u32_t group_x, mx_u32_t group_y, mx_u32_t group_z) {
    auto* record = reinterpret_cast<MxVkCmdDispatch*>(reserve(sizeof(MxVkCmdDispatch)));
    if (!record) return false;
    record->header.opcode = VK_CMD_DISPATCH;
    record->header.size = sizeof(MxVkCmdDispatch);
    record->header.flags = 0;
    record->group_x = group_x;
    record->group_y = group_y;
    record->group_z = group_z;
    return true;
}

bool CommandRecorder::bind_vertex_buffer(const Memory& buffer, mx_u64_t offset) {
    if (!buffer.valid()) return false;
    auto* record =
        reinterpret_cast<MxVkCmdBindVertexBuffer*>(reserve(sizeof(MxVkCmdBindVertexBuffer)));
    if (!record) return false;
    record->header.opcode = VK_CMD_BIND_VERTEX_BUFFER;
    record->header.size = sizeof(MxVkCmdBindVertexBuffer);
    record->header.flags = 0;
    record->arena_offset = buffer.arena_offset + offset;
    record->size_bytes = buffer.size;
    return true;
}

bool CommandRecorder::bind_render_target(const Memory& target, mx_u32_t width, mx_u32_t height,
                                         mx_u32_t stride) {
    if (!target.valid()) return false;
    auto* record =
        reinterpret_cast<MxVkCmdBindRenderTarget*>(reserve(sizeof(MxVkCmdBindRenderTarget)));
    if (!record) return false;
    record->header.opcode = VK_CMD_BIND_RENDER_TARGET;
    record->header.size = sizeof(MxVkCmdBindRenderTarget);
    record->header.flags = 0;
    record->arena_offset = target.arena_offset;
    record->width = width;
    record->height = height;
    record->stride = stride;
    return true;
}

bool CommandRecorder::draw(mx_u32_t vertex_count, mx_u32_t instance_count,
                           mx_u64_t color_xrgb8888) {
    auto* record = reinterpret_cast<MxVkCmdDraw*>(reserve(sizeof(MxVkCmdDraw)));
    if (!record) return false;
    record->header.opcode = VK_CMD_DRAW;
    record->header.size = sizeof(MxVkCmdDraw);
    record->header.flags = 0;
    record->vertex_count = vertex_count;
    record->instance_count = instance_count;
    record->color_xrgb8888 = color_xrgb8888;
    return true;
}

bool CommandRecorder::pipeline_barrier() {
    auto* record = reinterpret_cast<MxVkCommandHeader*>(reserve(sizeof(MxVkCommandHeader)));
    if (!record) return false;
    record->opcode = VK_CMD_PIPELINE_BARRIER;
    record->size = sizeof(MxVkCommandHeader);
    record->flags = 0;
    return true;
}

// --- SessionClient ---------------------------------------------------------

bool SessionClient::call(mx_u32_t opcode, const void* payload, mx_u32_t bytes,
                         mx_u32_t expect_reply, MxVkFrame* out) {
    if (!transport_.push || !transport_.pop) return false;
    MxVkFrame request{};
    mx_vk_slot_encode(&request, opcode, ++seq_, payload, bytes);
    if (!transport_.push(transport_.ctx, request)) return false;
    for (mx_u32_t spins = 0; spins < kRequestSpinLimit; ++spins) {
        MxVkFrame reply{};
        if (transport_.pop(transport_.ctx, &reply)) {
            if (reply.type == expect_reply || reply.type == VK_SESSION_ERROR) {
                *out = reply;
                return true;
            }
            continue; // a frame for another in-flight request: ignore it
        }
        if (transport_.wait && transport_.wait(transport_.ctx)) continue;
        return false;
    }
    return false;
}

bool SessionClient::open(mx_u32_t client_id, mx_u32_t arena_bytes, mx_u32_t max_objects,
                         mx_u32_t max_outstanding) {
    MxVkSessionOpenRequest request{};
    request.major = MX_VK_SESSION_VERSION_MAJOR;
    request.minor = MX_VK_SESSION_VERSION_MINOR;
    request.flags = MX_VK_SESSION_FLAG_NONE;
    request.arena_bytes = arena_bytes;
    request.max_objects = max_objects;
    request.max_outstanding = max_outstanding;
    request.client_id = client_id;
    MxVkSessionOpenReply reply{};
    if (!call_into(VK_SESSION_OPEN, &request, sizeof(request), VK_SESSION_OPEN_REPLY, &reply)) {
        return false;
    }
    if (reply.status != MX_OK || reply.session_id == 0) return false;
    info_ = reply;
    arena_bytes_ = reply.arena_bytes;
    arena_base_ = nullptr;
    if ((reply.flags & MX_VK_SESSION_FLAG_ARENA_READY) && transport_.map_arena) {
        arena_base_ = transport_.map_arena(transport_.ctx, reply.session_id, reply.arena_bytes);
    }
    recorder_.begin(arena_base_, arena_base_ ? MX_VK_COMMAND_REGION_BYTES : 0u);
    open_ok_ = true;
    return true;
}

bool SessionClient::close() {
    if (!open_ok_) return false;
    MxVkSessionCloseRequest request{};
    request.session_id = info_.session_id;
    MxVkSessionCloseReply reply{};
    const bool ok = call_into(VK_SESSION_CLOSE, &request, sizeof(request), VK_SESSION_CLOSE_REPLY,
                              &reply);
    open_ok_ = false;
    arena_base_ = nullptr;
    arena_bytes_ = 0;
    recorder_.begin(nullptr, 0);
    return ok;
}

bool SessionClient::alloc(mx_u64_t size, mx_u32_t alignment, mx_u32_t memory_type, Memory* out) {
    if (!open_ok_ || !out) return false;
    MxVkSessionAllocRequest request{};
    request.session_id = info_.session_id;
    request.size = size;
    request.alignment = alignment;
    request.memory_type = memory_type;
    request.usage = 0;
    request.flags = MX_VK_MEMORY_FLAG_MAPPED;
    MxVkSessionAllocReply reply{};
    if (!call_into(VK_SESSION_ALLOC, &request, sizeof(request), VK_SESSION_ALLOC_REPLY, &reply) ||
        reply.status != MX_OK || reply.memory_id == 0) {
        return false;
    }
    out->id = reply.memory_id;
    out->arena_offset = reply.arena_offset;
    out->size = reply.size;
    out->ptr = arena_at(reply.arena_offset);
    return true;
}

bool SessionClient::free(const Memory& memory) {
    if (!open_ok_ || !memory.valid()) return false;
    MxVkSessionFreeRequest request{};
    request.session_id = info_.session_id;
    request.memory_id = memory.id;
    MxVkSessionFreeReply reply{};
    return call_into(VK_SESSION_FREE, &request, sizeof(request), VK_SESSION_FREE_REPLY, &reply) &&
           reply.status == MX_OK;
}

// Decode a submit/sync reply, or the paired error, into a status. Factored here
// so both paths report the same status a caller can branch on.
namespace {
template <typename Reply>
bool finish_status(const MxVkFrame& frame, mx_u32_t expect_reply, Reply* reply, mx_status_t* status) {
    if (frame.type == VK_SESSION_ERROR) {
        MxVkSessionError err{};
        if (mx_vk_slot_decode(&frame, VK_SESSION_ERROR, (mx_u32_t)sizeof(err), &err) != 0) {
            if (status) *status = MX_ERR_BAD_PROTOCOL;
            return false;
        }
        if (status) *status = err.status;
        return false;
    }
    if (mx_vk_slot_decode(&frame, expect_reply, (mx_u32_t)sizeof(*reply), reply) != 0) {
        if (status) *status = MX_ERR_BAD_PROTOCOL;
        return false;
    }
    if (status) *status = reply->status;
    return reply->status == MX_OK;
}
} // namespace

bool SessionClient::submit(mx_u64_t sync_id, mx_status_t* out_status) {
    if (!open_ok_) {
        if (out_status) *out_status = MX_ERR_DEAD;
        return false;
    }
    if (recorder_.overflowed() || recorder_.empty()) {
        if (out_status) *out_status = MX_ERR_INVALID;
        return false;
    }
    MxVkSessionSubmitRequest request{};
    request.session_id = info_.session_id;
    request.command_offset = 0; // the command region starts at the arena head
    request.sync_id = sync_id;
    request.command_bytes = recorder_.size();
    request.flags = sync_id ? MX_VK_SUBMIT_FLAG_SIGNAL : MX_VK_SUBMIT_FLAG_NONE;
    MxVkFrame frame{};
    if (!call(VK_SESSION_SUBMIT, &request, sizeof(request), VK_SESSION_SUBMIT_REPLY, &frame)) {
        if (out_status) *out_status = MX_ERR_DEAD;
        return false;
    }
    MxVkSessionSubmitReply reply{};
    const bool ok = finish_status(frame, VK_SESSION_SUBMIT_REPLY, &reply, out_status);
    if (ok) recorder_.reset(); // the command region is consumed by this submit
    return ok;
}

bool SessionClient::sync_wait(mx_u64_t sync_id, mx_u64_t timeout_ns, mx_status_t* out_status) {
    if (!open_ok_) {
        if (out_status) *out_status = MX_ERR_DEAD;
        return false;
    }
    MxVkSessionSyncWaitRequest request{};
    request.session_id = info_.session_id;
    request.sync_id = sync_id;
    request.timeout_ns = timeout_ns;
    request.flags = 0;
    MxVkFrame frame{};
    if (!call(VK_SESSION_SYNC_WAIT, &request, sizeof(request), VK_SESSION_SYNC_WAIT_REPLY, &frame)) {
        if (out_status) *out_status = MX_ERR_DEAD;
        return false;
    }
    MxVkSessionSyncWaitReply reply{};
    return finish_status(frame, VK_SESSION_SYNC_WAIT_REPLY, &reply, out_status);
}

bool SessionClient::surface_create(mx_u32_t width, mx_u32_t height, mx_u32_t format,
                                   mx_u64_t* surface_id) {
    if (!open_ok_) return false;
    MxVkSurfaceCreateRequest request{};
    request.session_id = info_.session_id;
    request.width = width;
    request.height = height;
    request.format = format;
    request.flags = 0;
    MxVkSurfaceCreateReply reply{};
    if (!call_into(VK_SESSION_SURFACE_CREATE, &request, sizeof(request),
                   VK_SESSION_SURFACE_CREATE_REPLY, &reply) ||
        reply.status != MX_OK || reply.surface_id == 0) {
        return false;
    }
    if (surface_id) *surface_id = reply.surface_id;
    return true;
}

bool SessionClient::swapchain_create(mx_u64_t surface_id, mx_u32_t image_count, mx_u32_t stride,
                                     mx_u32_t pixel_format, mx_u64_t* swapchain_id,
                                     mx_u32_t* image_bytes) {
    if (!open_ok_) return false;
    MxVkSwapchainCreateRequest request{};
    request.session_id = info_.session_id;
    request.surface_id = surface_id;
    request.image_count = image_count;
    request.stride = stride;
    request.pixel_format = pixel_format;
    request.flags = MX_VK_SWAPCHAIN_FLAG_BOUNDED_COPY;
    MxVkSwapchainCreateReply reply{};
    if (!call_into(VK_SESSION_SWAPCHAIN_CREATE, &request, sizeof(request),
                   VK_SESSION_SWAPCHAIN_CREATE_REPLY, &reply) ||
        reply.status != MX_OK || reply.swapchain_id == 0) {
        return false;
    }
    if (swapchain_id) *swapchain_id = reply.swapchain_id;
    if (image_bytes) *image_bytes = reply.image_bytes;
    return true;
}

bool SessionClient::swapchain_acquire(mx_u64_t swapchain_id, mx_u32_t* image_index, Memory* image,
                                      mx_u32_t* image_bytes) {
    if (!open_ok_) return false;
    MxVkSwapchainAcquireRequest request{};
    request.session_id = info_.session_id;
    request.swapchain_id = swapchain_id;
    request.timeout_ns = 0;
    request.flags = 0;
    MxVkSwapchainAcquireReply reply{};
    if (!call_into(VK_SESSION_SWAPCHAIN_ACQUIRE, &request, sizeof(request),
                   VK_SESSION_SWAPCHAIN_ACQUIRE_REPLY, &reply) ||
        reply.status != MX_OK) {
        return false;
    }
    if (image_index) *image_index = reply.image_index;
    if (image_bytes) *image_bytes = reply.image_bytes;
    if (image) {
        image->id = reply.image_id;
        image->arena_offset = reply.arena_offset;
        image->size = reply.image_bytes;
        image->ptr = arena_at(reply.arena_offset);
    }
    return true;
}

bool SessionClient::swapchain_present(mx_u64_t swapchain_id, mx_u32_t image_index, bool full_frame,
                                      mx_u32_t* copied_bytes) {
    if (!open_ok_) return false;
    MxVkSwapchainPresentRequest request{};
    request.session_id = info_.session_id;
    request.swapchain_id = swapchain_id;
    request.image_index = image_index;
    request.flags = full_frame ? MX_VK_PRESENT_FLAG_FULL_FRAME : MX_VK_PRESENT_FLAG_NONE;
    MxVkFrame frame{};
    if (!call(VK_SESSION_SWAPCHAIN_PRESENT, &request, sizeof(request),
              VK_SESSION_SWAPCHAIN_PRESENT_REPLY, &frame)) {
        return false;
    }
    MxVkSwapchainPresentReply reply{};
    if (!finish_status(frame, VK_SESSION_SWAPCHAIN_PRESENT_REPLY, &reply, nullptr)) return false;
    if (copied_bytes) *copied_bytes = reply.copied_bytes;
    return true;
}

} // namespace dango::vk
