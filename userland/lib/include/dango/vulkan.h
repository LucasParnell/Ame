#pragma once
// dango/vulkan.h — the native Mochi Vulkan session client and the `ame_remote`
// ICD marshalling (Ame spec §14.6/§14.7, Phase N/O).
//
// This is the client half of the remote Vulkan session transport. A native
// service (or, in the host regression, a test) drives it to:
//
//   * open one session over a shared ring pair,
//   * allocate host-visible memory out of the session arena,
//   * *record* a bounded command stream into the session's command region —
//     this is the `ame_remote` ICD's marshalling of a recorded command buffer,
//   * submit it (one RPC crosses an unbounded number of commands) and wait on
//     a client-owned sync token,
//   * drive a surface/swapchain whose present is a bounded copy into the Dango
//     shared buffer (spec §15.3–§15.5).
//
// The client is transport-abstract: it never names a RingPage, a syscall or a
// mochi cap, only the `MxVkFrame` envelope and the abstract `Transport` below.
// That keeps the marshalling in one place and lets the same translation unit be
// driven by the in-kernel mochi ring (mochi_transport.h) or by an in-memory
// test transport. Nothing here allocates on the request path: every table is
// fixed-capacity, sized from the shared ABI quotas.

#include "midix/abi/protocols/vulkan_session.h"

namespace dango::vk {

// Bounds one request/reply exchange: the client may skip this many stray frames
// (a completion or a reply for another in-flight call) before it gives up, so a
// wedged service can never spin the caller. Fixed, so the wait path allocates
// nothing.
constexpr mx_u32_t kRequestSpinLimit = MX_VK_SESSION_SLOTS;

// One shared ring pair, seen by the client as push/pop of an MxVkFrame. `wait`
// blocks (bounded by the transport) until a reply might be available; an
// in-process test transport simply returns true. `map_arena` returns the
// client's writable view of a session arena, or null when the platform cannot
// map it yet (V0 has no native shared-memory attach; the manager brokers
// sessions with no arena and submits are then refused).
struct Transport {
    void* ctx = nullptr;
    bool (*push)(void* ctx, const MxVkFrame& slot) = nullptr;
    bool (*pop)(void* ctx, MxVkFrame* out) = nullptr;
    bool (*wait)(void* ctx) = nullptr;
    mx_u8_t* (*map_arena)(void* ctx, mx_u64_t session_id, mx_u32_t arena_bytes) = nullptr;
};

// A host-visible allocation inside the session arena. `ptr` is the client's
// writable view (arena base + arena_offset), or null when the arena is unmapped.
struct Memory {
    mx_u64_t id = 0;
    mx_u64_t arena_offset = 0;
    mx_u32_t size = 0;
    mx_u8_t* ptr = nullptr;
    bool valid() const { return id != 0; }
};

// The `ame_remote` ICD marshalling: append bounded command records into the
// session command region. Every record is an MxVkCommandHeader followed by its
// body, aligned to MX_VK_COMMAND_ALIGNMENT. No allocation, no overflow: a
// record that does not fit leaves the recorder unchanged and returns false.
class CommandRecorder {
public:
    void begin(mx_u8_t* region, mx_u32_t capacity) {
        region_ = region;
        capacity_ = capacity;
        size_ = 0;
        overflow_ = false;
    }
    void reset() { size_ = 0; overflow_ = false; }
    mx_u32_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    bool overflowed() const { return overflow_; }

    bool fill_buffer(const Memory& dst, mx_u64_t offset, mx_u64_t size, mx_u32_t value);
    bool copy_buffer(const Memory& src, mx_u64_t src_offset, const Memory& dst, mx_u64_t dst_offset,
                     mx_u64_t size);
    bool dispatch(mx_u32_t group_x, mx_u32_t group_y, mx_u32_t group_z);
    bool bind_vertex_buffer(const Memory& buffer, mx_u64_t offset);
    bool bind_render_target(const Memory& target, mx_u32_t width, mx_u32_t height,
                            mx_u32_t stride);
    bool draw(mx_u32_t vertex_count, mx_u32_t instance_count, mx_u64_t color_xrgb8888);
    bool pipeline_barrier();

private:
    mx_u8_t* reserve(mx_u32_t opcode, mx_u32_t record_bytes);

    mx_u8_t* region_ = nullptr;
    mx_u32_t capacity_ = 0;
    mx_u32_t size_ = 0;
    bool overflow_ = false;
};

// The client owns exactly one session. attach() is non-owning; open() runs the
// handshake and pins the session id and arena.
class SessionClient {
public:
    void attach(const Transport& transport) { transport_ = transport; }

    // Open a session. On success `info()` describes the granted quotas and the
    // arena view (when the transport can map one) is armed for the recorder.
    bool open(mx_u32_t client_id, mx_u32_t arena_bytes, mx_u32_t max_objects,
              mx_u32_t max_outstanding);

    // Close the session. Safe to call when not open.
    bool close();

    bool open_ok() const { return open_ok_; }
    const MxVkSessionOpenReply& info() const { return info_; }
    mx_u64_t session_id() const { return info_.session_id; }

    // Host-visible memory.
    bool alloc(mx_u64_t size, mx_u32_t alignment, mx_u32_t memory_type, Memory* out);
    bool free(const Memory& memory);

    CommandRecorder& recorder() { return recorder_; }
    const CommandRecorder& recorder() const { return recorder_; }

    // Signalled result of sync_wait(); MX_OK when the token was reached.
    bool submit(mx_u64_t sync_id, mx_status_t* out_status);
    bool sync_wait(mx_u64_t sync_id, mx_u64_t timeout_ns, mx_status_t* out_status);

    // WSI.
    bool surface_create(mx_u32_t width, mx_u32_t height, mx_u32_t format, mx_u64_t* surface_id);
    bool swapchain_create(mx_u64_t surface_id, mx_u32_t image_count, mx_u32_t stride,
                          mx_u32_t pixel_format, mx_u64_t* swapchain_id, mx_u32_t* image_bytes);
    bool swapchain_acquire(mx_u64_t swapchain_id, mx_u32_t* image_index, Memory* image,
                           mx_u32_t* image_bytes);
    bool swapchain_present(mx_u64_t swapchain_id, mx_u32_t image_index, bool full_frame,
                           mx_u32_t* copied_bytes);

    // Absolute pointer for an arena offset (null when the arena is unmapped).
    mx_u8_t* arena_at(mx_u64_t arena_offset) const {
        return arena_base_ && arena_offset < arena_bytes_ ? arena_base_ + arena_offset : nullptr;
    }

private:
    // Send one request and collect its reply frame. Returns true when a reply of
    // `expect_reply` (or a VK_SESSION_ERROR) arrived. Fills `out`.
    bool call(mx_u32_t opcode, const void* payload, mx_u32_t bytes, mx_u32_t expect_reply,
              MxVkFrame* out);

    // Send a request, decode the reply into `T`, and report the reply status.
    template <typename T>
    bool call_into(mx_u32_t opcode, const void* payload, mx_u32_t bytes, mx_u32_t expect_reply,
                   T* out) {
        MxVkFrame frame{};
        if (!call(opcode, payload, bytes, expect_reply, &frame)) return false;
        if (frame.type == VK_SESSION_ERROR) return false;
        return mx_vk_slot_decode(&frame, expect_reply, (mx_u32_t)sizeof(T), out) == 0;
    }

    Transport transport_{};
    MxVkSessionOpenReply info_{};
    mx_u64_t seq_ = 0; // running correlation token stamped on every request
    bool open_ok_ = false;
    mx_u8_t* arena_base_ = nullptr;
    mx_u32_t arena_bytes_ = 0;
    CommandRecorder recorder_{};
};

} // namespace dango::vk
