#pragma once
// userland/drivers/gpu/linux/vulkan_broker.h — per-client Vulkan session
// brokerage for the native GPU service (spec §14.7, §15.3).
//
// The broker owns the *sessions*: each native client that opens the endpoint
// gets its own logical session, with its own arena watermark, its own object
// table and its own outstanding-submit accounting. Every quota is enforced here
// (MX_VK_SESSION_* in midix/abi/protocols/vulkan_session.h), and every object id
// embeds the boot generation and the owning session, so a request that names
// another client's session or object is refused rather than aliased.
//
// The broker performs no I/O and touches no memory the client owns except the
// two ring frames it is handed: it is pure state, which is why the same
// translation unit serves the manager's server loop and the host regression
// that drives it against a reference backend. The *work* (executing a command
// stream, copying a swapchain image into the Dango shared buffer) is delegated
// to a VulkanBackend: the guest `dango-vkd` in production, a reference backend
// in the host test. Nothing here names a Vulkan type, a DRM object or a Linux
// pointer.

#include "midix/abi/protocols/vulkan_session.h"
#include "libk/integer.h"

namespace drv::gpu::linux {

// The work the broker forwards to the appliance. A backend reports whether it
// has a live Vulkan device and an arena for a new session (open), executes a
// bounded command stream (submit), observes completion (sync_wait) and copies a
// swapchain image into the Dango shared buffer (present). All bounds are already
// validated by the broker before a call; a backend still validates its own.
struct VulkanBackend {
    void* ctx = nullptr;
    // Returns true and fills `*flags_out` with MX_VK_SESSION_FLAG_* the session
    // should advertise, or false to refuse the session outright.
    bool (*open)(void* ctx, mx_u64_t session_id, mx_u32_t client_id, mx_u32_t arena_bytes,
                 mx_u32_t* flags_out) = nullptr;
    void (*close)(void* ctx, mx_u64_t session_id) = nullptr;
    // Execute [command_offset, command_offset+command_bytes) of the session
    // arena. Returns MX_OK once accepted, or a negative mx_status_t.
    mx_status_t (*submit)(void* ctx, mx_u64_t session_id, mx_u32_t command_offset,
                          mx_u32_t command_bytes, mx_u64_t sync_id) = nullptr;
    // Wait until `sync_id` is reached. Returns MX_OK signalled, MX_ERR_BUSY when
    // not yet reached within the bounded wait, or another negative status.
    mx_status_t (*sync_wait)(void* ctx, mx_u64_t session_id, mx_u64_t sync_id,
                             mx_u64_t timeout_ns) = nullptr;
    // Bounded copy of one swapchain image into the Dango shared buffer.
    // `*copied_out` is the number of bytes moved (<= image_bytes and <=
    // MX_VK_SHARED_BUFFER_MAX_BYTES). Returns MX_OK or a negative status.
    mx_status_t (*present)(void* ctx, mx_u64_t session_id, mx_u64_t swapchain_id,
                           mx_u32_t image_index, mx_u64_t image_offset, mx_u32_t image_bytes,
                           mx_u32_t* copied_out) = nullptr;
};

class VulkanSessionBroker {
public:
    static constexpr mx_u32_t kMaxSessions = MX_VK_SESSION_MAX_SESSIONS;
    static constexpr mx_u32_t kMaxObjects = MX_VK_SESSION_MAX_OBJECTS;
    static constexpr mx_u32_t kMaxOutstanding = MX_VK_SESSION_MAX_OUTSTANDING;
    static constexpr mx_u32_t kMaxSurfaces = MX_VK_SESSION_MAX_SURFACES;
    static constexpr mx_u32_t kMaxSwapchains = MX_VK_SESSION_MAX_SWAPCHAINS;
    static constexpr mx_u32_t kMaxImages = MX_VK_SWAPCHAIN_MAX_IMAGES;
    static constexpr mx_u32_t kCommandRegionBytes = MX_VK_COMMAND_REGION_BYTES;

    void init(const VulkanBackend& backend, mx_u64_t boot_generation) {
        backend_ = backend;
        boot_generation_ = boot_generation;
        for (mx_u32_t i = 0; i < kMaxSessions; ++i) {
            sessions_[i] = Session{};
            incarnations_[i] = Incarnations{};
        }
    }

    // Handle one request frame from `client_id`, writing exactly one reply frame
    // (whose `seq` echoes the request's). Returns false only when the opcode is
    // not a Vulkan session opcode at all, so the caller can answer with its own
    // protocol error; any error inside this protocol is reported as a
    // VK_SESSION_ERROR reply and still returns true.
    bool handle(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        if (!reply || !mx_vk_is_opcode(request.type)) return false;
        switch (request.type) {
        case VK_SESSION_OPEN: return on_open(client_id, request, reply);
        case VK_SESSION_CLOSE: return on_close(client_id, request, reply);
        case VK_SESSION_ALLOC: return on_alloc(client_id, request, reply);
        case VK_SESSION_FREE: return on_free(client_id, request, reply);
        case VK_SESSION_SUBMIT: return on_submit(client_id, request, reply);
        case VK_SESSION_SYNC_WAIT: return on_sync_wait(client_id, request, reply);
        case VK_SESSION_SURFACE_CREATE: return on_surface_create(client_id, request, reply);
        case VK_SESSION_SURFACE_DESTROY: return on_surface_destroy(client_id, request, reply);
        case VK_SESSION_SWAPCHAIN_CREATE: return on_swapchain_create(client_id, request, reply);
        case VK_SESSION_SWAPCHAIN_ACQUIRE: return on_swapchain_acquire(client_id, request, reply);
        case VK_SESSION_SWAPCHAIN_PRESENT: return on_swapchain_present(client_id, request, reply);
        default:
            // A reply/error opcode sent as a request, or an unknown opcode in
            // the range: refuse it rather than guess.
            return error(reply, request, MX_ERR_BAD_PROTOCOL, 0, 0);
        }
    }

    // Introspection for tests and counters (never used on the request path).
    mx_u32_t session_count() const {
        mx_u32_t n = 0;
        for (mx_u32_t i = 0; i < kMaxSessions; ++i) n += sessions_[i].used ? 1u : 0u;
        return n;
    }
    bool session_stats(mx_u32_t index, mx_u32_t* objects, mx_u32_t* outstanding,
                       mx_u32_t* watermark) const {
        if (index >= kMaxSessions || !sessions_[index].used) return false;
        if (objects) *objects = sessions_[index].object_count;
        if (outstanding) *outstanding = sessions_[index].outstanding_count;
        if (watermark) *watermark = sessions_[index].watermark;
        return true;
    }

private:
    struct MemoryObject {
        bool used = false;
        mx_u64_t id = 0;
        mx_u64_t arena_offset = 0;
        mx_u32_t size = 0;
        mx_u32_t memory_type = 0;
    };
    struct Surface {
        bool used = false;
        mx_u64_t id = 0;
        mx_u32_t width = 0;
        mx_u32_t height = 0;
        mx_u32_t format = 0;
    };
    struct SwapchainImage {
        mx_u64_t memory_id = 0;
        mx_u64_t arena_offset = 0;
        mx_u32_t size = 0;
        mx_u32_t acquired = 0;
    };
    struct Swapchain {
        bool used = false;
        mx_u64_t id = 0;
        mx_u64_t surface_id = 0;
        mx_u32_t image_count = 0;
        mx_u32_t stride = 0;
        mx_u32_t pixel_format = 0;
        mx_u64_t frame_index = 0;
        mx_u32_t next_image = 0;
        SwapchainImage images[kMaxImages];
    };
    struct Session {
        bool used = false;
        mx_u32_t index = 0;
        mx_u32_t client_id = 0;
        mx_u64_t id = 0;
        mx_u64_t boot_generation = 0;
        mx_u32_t flags = 0;
        mx_u32_t arena_bytes = 0;
        mx_u32_t watermark = 0;
        mx_u32_t max_objects = 0;
        mx_u32_t max_outstanding = 0;
        mx_u32_t object_count = 0;
        mx_u32_t outstanding_count = 0;
        mx_u64_t outstanding_ids[kMaxOutstanding];
        mx_u32_t surface_count = 0;
        mx_u32_t swapchain_count = 0;
        MemoryObject objects[kMaxObjects];
        Surface surfaces[kMaxSurfaces];
        Swapchain swapchains[kMaxSwapchains];
    };

    // Lifetime counters survive session close. A stale object must not become
    // valid when either its own slot or its parent session slot is reused.
    struct Incarnations {
        mx_u32_t session = 0;
        mx_u32_t memory[kMaxObjects]{};
        mx_u32_t surface[kMaxSurfaces]{};
        mx_u32_t swapchain[kMaxSwapchains]{};
    };

    static mx_u64_t issue_id(mx_u64_t generation, mx_u32_t type, mx_u32_t session,
                              mx_u32_t index, mx_u32_t& incarnation) {
        return mx_vk_object_id_with_incarnation(
            mx_vk_object_id_make(generation, type, session, index), incarnation++);
    }

    // --- session lookup -----------------------------------------------------
    //
    // A session id is itself an id of type MX_VK_OBJECT_SESSION whose index is
    // the 1-based session slot. Lookup validates the generation, the type and
    // the owning client, so a request that names another client's session is
    // refused (never aliased).
    Session* find_session(mx_u32_t client_id, mx_u64_t session_id) {
        // A session id is not scoped to a session, so mx_vk_object_id_check does
        // not apply; validate its generation and type directly, then match it and
        // its owning client. A request that names another client's session never
        // resolves.
        const mx_u32_t type =
            (mx_u32_t)((session_id & MX_VK_OBJECT_TYPE_MASK) >> MX_VK_OBJECT_TYPE_SHIFT);
        const mx_u64_t generation = session_id >> MX_VK_OBJECT_GENERATION_SHIFT;
        if (type != MX_VK_OBJECT_SESSION || generation != (boot_generation_ & 0xFFFFFFFFull)) {
            return nullptr;
        }
        for (mx_u32_t i = 0; i < kMaxSessions; ++i) {
            Session& s = sessions_[i];
            if (s.used && s.id == session_id && s.client_id == client_id &&
                s.boot_generation == boot_generation_) {
                return &s;
            }
        }
        return nullptr;
    }

    Session* free_session_slot() {
        for (mx_u32_t i = 0; i < kMaxSessions; ++i) {
            if (!sessions_[i].used && incarnations_[i].session <= MX_VK_OBJECT_MAX_INCARNATION) return &sessions_[i];
        }
        return nullptr;
    }

    // --- object tables ------------------------------------------------------
    MemoryObject* find_memory(Session& s, mx_u64_t memory_id) {
        mx_u32_t index = 0;
        if (mx_vk_object_id_check(memory_id, MX_VK_OBJECT_MEMORY, boot_generation_, s.index,
                                  &index) != 0) {
            return nullptr;
        }
        if (index > kMaxObjects) return nullptr;
        MemoryObject& obj = s.objects[index - 1];
        return (obj.used && obj.id == memory_id) ? &obj : nullptr;
    }

    Swapchain* find_swapchain(Session& s, mx_u64_t swapchain_id) {
        mx_u32_t index = 0;
        if (mx_vk_object_id_check(swapchain_id, MX_VK_OBJECT_SWAPCHAIN, boot_generation_, s.index,
                                  &index) != 0) {
            return nullptr;
        }
        if (index > kMaxSwapchains) return nullptr;
        Swapchain& sc = s.swapchains[index - 1];
        return (sc.used && sc.id == swapchain_id) ? &sc : nullptr;
    }

    Surface* find_surface(Session& s, mx_u64_t surface_id) {
        mx_u32_t index = 0;
        if (mx_vk_object_id_check(surface_id, MX_VK_OBJECT_SURFACE, boot_generation_, s.index,
                                  &index) != 0) {
            return nullptr;
        }
        if (index > kMaxSurfaces) return nullptr;
        Surface& sf = s.surfaces[index - 1];
        return (sf.used && sf.id == surface_id) ? &sf : nullptr;
    }

    // Allocate `size` bytes from the session arena and return the offset, or
    // fail if the arena is exhausted. Command-region bytes are never handed out.
    bool arena_alloc(Session& s, mx_u32_t size, mx_u32_t alignment, mx_u64_t* offset_out) {
        if (alignment && (alignment & (alignment - 1)) != 0) return false;
        const mx_u32_t align = alignment < MX_VK_SESSION_ARENA_ALIGNMENT
            ? MX_VK_SESSION_ARENA_ALIGNMENT : alignment;
        const mx_u32_t start = s.watermark < kCommandRegionBytes ? kCommandRegionBytes : s.watermark;
        mx_u64_t base = 0;
        if (!libk::checked_align_up(start, align, &base) || size == 0 ||
            base > s.arena_bytes || size > s.arena_bytes - base) return false;
        *offset_out = base;
        s.watermark = base + size;
        return true;
    }

    // --- reply builders -----------------------------------------------------
    template <typename T>
    bool reply_with(MxVkFrame* reply, const MxVkFrame& request, mx_u32_t opcode, const T& msg) {
        static_assert(sizeof(T) <= MX_VK_SESSION_INLINE_PAYLOAD,
                      "reply must fit one ring payload");
        mx_vk_slot_encode(reply, opcode, request.seq, &msg, sizeof(T));
        return true;
    }

    bool error(MxVkFrame* reply, const MxVkFrame& request, mx_status_t status, mx_u64_t session_id,
               mx_u64_t detail) {
        MxVkSessionError msg{};
        msg.status = status;
        msg.session_id = session_id;
        msg.detail = detail;
        msg.opcode = request.type;
        return reply_with(reply, request, VK_SESSION_ERROR, msg);
    }

    // --- opcode handlers ----------------------------------------------------
    bool on_open(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSessionOpenRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_OPEN, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, 0, request.length);
        }
        if (req.major != MX_VK_SESSION_VERSION_MAJOR || req.minor != MX_VK_SESSION_VERSION_MINOR) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, 0,
                         ((mx_u64_t)req.major << 16) | req.minor);
        }
        Session* slot = free_session_slot();
        if (!slot) return error(reply, request, MX_ERR_BUSY, 0, kMaxSessions);

        // Grant at most the ABI quotas, whatever the client asked for.
        mx_u32_t arena = (mx_u32_t)(req.arena_bytes > MX_VK_SESSION_MAX_ARENA_BYTES
                                        ? MX_VK_SESSION_MAX_ARENA_BYTES
                                        : req.arena_bytes);
        const mx_u32_t min_arena =
            libk::align_up(kCommandRegionBytes + MX_VK_SESSION_ARENA_ALIGNMENT,
                     MX_VK_SESSION_ARENA_ALIGNMENT);
        if (arena < min_arena) arena = min_arena;
        mx_u32_t max_objects = req.max_objects == 0 || req.max_objects > kMaxObjects
                                   ? kMaxObjects
                                   : req.max_objects;
        mx_u32_t max_outstanding = req.max_outstanding == 0 || req.max_outstanding > kMaxOutstanding
                                       ? kMaxOutstanding
                                       : req.max_outstanding;

        const mx_u32_t index = (mx_u32_t)(slot - sessions_) + 1u;
        const mx_u64_t session_id = issue_id(boot_generation_, MX_VK_OBJECT_SESSION, 0,
                                               index, incarnations_[index - 1].session);
        mx_u32_t backend_flags = 0;
        const bool backend_ok =
            backend_.open == nullptr ||
            backend_.open(backend_.ctx, session_id, client_id, arena, &backend_flags);

        if (!backend_ok) return error(reply, request, MX_ERR_SERVICE_UNAVAILABLE, 0, 0);

        *slot = Session{};
        slot->used = true;
        slot->index = index;
        slot->client_id = client_id;
        slot->id = session_id;
        slot->boot_generation = boot_generation_;
        slot->arena_bytes = arena;
        slot->max_objects = max_objects;
        slot->max_outstanding = max_outstanding;
        slot->watermark = kCommandRegionBytes;
        // The backend owns the arena, so it is the backend that advertises
        // ARENA_READY (and VULKAN_LIVE / WSI). A default backend with no
        // functions grants no flags: the session opens, but allocation and
        // submission are refused rather than faked.
        slot->flags = backend_ok ? backend_flags : 0;

        MxVkSessionOpenReply rep{};
        rep.status = MX_OK;
        rep.session_id = session_id;
        rep.boot_generation = boot_generation_;
        rep.arena_id = 0; // set by the service when the arena is a Dango window
        rep.arena_bytes = arena;
        rep.max_objects = max_objects;
        rep.max_outstanding = max_outstanding;
        rep.flags = slot->flags;
        return reply_with(reply, request, VK_SESSION_OPEN_REPLY, rep);
    }

    bool on_close(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSessionCloseRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_CLOSE, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, 0, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        const mx_u64_t session_id = s->id;
        if (backend_.close) backend_.close(backend_.ctx, session_id);
        *s = Session{};
        MxVkSessionCloseReply rep{};
        rep.status = MX_OK;
        rep.session_id = session_id;
        rep.boot_generation = boot_generation_;
        return reply_with(reply, request, VK_SESSION_CLOSE_REPLY, rep);
    }

    bool on_alloc(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSessionAllocRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_ALLOC, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, req.session_id, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        if ((s->flags & MX_VK_SESSION_FLAG_ARENA_READY) == 0) {
            // No arena is mapped for this session, so no allocation can be
            // honoured: refuse instead of handing out offsets into nothing.
            return error(reply, request, MX_ERR_SERVICE_UNAVAILABLE, s->id, 0);
        }
        if (req.memory_type != MX_VK_MEMORY_TYPE_HOST_VISIBLE &&
            req.memory_type != MX_VK_MEMORY_TYPE_DEVICE_LOCAL) {
            return error(reply, request, MX_ERR_INVALID, s->id, req.memory_type);
        }
        if (s->object_count >= s->max_objects) {
            return error(reply, request, MX_ERR_NOMEM, s->id, s->max_objects);
        }
        if (req.size > 0xFFFFFFFFull) return error(reply, request, MX_ERR_OVERFLOW, s->id, req.size);
        mx_u32_t index = 0;
        for (mx_u32_t i = 0; i < kMaxObjects; ++i) {
            if (!s->objects[i].used && incarnations_[s->index - 1].memory[i] <= MX_VK_OBJECT_MAX_INCARNATION) {
                index = i + 1;
                break;
            }
        }
        if (index == 0) return error(reply, request, MX_ERR_NOMEM, s->id, 0);

        const mx_u32_t size = (mx_u32_t)req.size;
        mx_u64_t offset = 0;
        if (!arena_alloc(*s, size, req.alignment, &offset)) {
            return error(reply, request, MX_ERR_NOMEM, s->id, s->arena_bytes);
        }
        MemoryObject& obj = s->objects[index - 1];
        obj.used = true;
        obj.id = issue_id(boot_generation_, MX_VK_OBJECT_MEMORY, s->index, index,
                          incarnations_[s->index - 1].memory[index - 1]);
        obj.arena_offset = offset;
        obj.size = size;
        obj.memory_type = req.memory_type;
        ++s->object_count;

        MxVkSessionAllocReply rep{};
        rep.status = MX_OK;
        rep.memory_id = obj.id;
        rep.boot_generation = boot_generation_;
        rep.arena_offset = offset;
        rep.size = size;
        rep.memory_type = req.memory_type;
        rep.alignment = req.alignment < MX_VK_SESSION_ARENA_ALIGNMENT ? MX_VK_SESSION_ARENA_ALIGNMENT
                                                                     : req.alignment;
        rep.flags = MX_VK_MEMORY_FLAG_MAPPED;
        return reply_with(reply, request, VK_SESSION_ALLOC_REPLY, rep);
    }

    bool on_free(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSessionFreeRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_FREE, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, req.session_id, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        MemoryObject* obj = find_memory(*s, req.memory_id);
        if (!obj) return error(reply, request, MX_ERR_NOT_FOUND, s->id, req.memory_id);
        for (const auto& swapchain : s->swapchains) {
            if (!swapchain.used) continue;
            for (mx_u32_t i = 0; i < swapchain.image_count; ++i) {
                if (swapchain.images[i].memory_id == obj->id) {
                    return error(reply, request, MX_ERR_BUSY, s->id, obj->id);
                }
            }
        }
        *obj = MemoryObject{};
        if (s->object_count > 0) --s->object_count;
        MxVkSessionFreeReply rep{};
        rep.status = MX_OK;
        rep.memory_id = req.memory_id;
        return reply_with(reply, request, VK_SESSION_FREE_REPLY, rep);
    }

    bool on_submit(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSessionSubmitRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_SUBMIT, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, req.session_id, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        // Bound the command range to the command region, not merely the arena.
        if (req.command_bytes == 0 || req.command_bytes > kCommandRegionBytes ||
            req.command_offset >= kCommandRegionBytes ||
            req.command_bytes > kCommandRegionBytes - req.command_offset ||
            (req.command_offset % MX_VK_COMMAND_ALIGNMENT) != 0 ||
            (req.command_bytes % MX_VK_COMMAND_ALIGNMENT) != 0) {
            return error(reply, request, MX_ERR_INVALID, s->id, req.command_bytes);
        }
        if (s->outstanding_count >= s->max_outstanding) {
            return error(reply, request, MX_ERR_BUSY, s->id, s->max_outstanding);
        }
        if ((s->flags & MX_VK_SESSION_FLAG_VULKAN_LIVE) == 0 || backend_.submit == nullptr) {
            // No Vulkan device behind the session: refuse, so the client degrades
            // to its local path instead of stalling (spec §14.7).
            return error(reply, request, MX_ERR_SERVICE_UNAVAILABLE, s->id, 0);
        }
        const mx_u64_t sync_id = req.sync_id;
        for (mx_u32_t i = 0; sync_id && i < s->outstanding_count; ++i) {
            if (s->outstanding_ids[i] == sync_id) return error(reply, request, MX_ERR_BUSY, s->id, sync_id);
        }
        const mx_status_t rc = backend_.submit(backend_.ctx, s->id, req.command_offset,
                                               req.command_bytes, sync_id);
        if (mx_err(rc)) return error(reply, request, rc, s->id, req.command_bytes);
        if (sync_id != 0 && s->outstanding_count < kMaxOutstanding) {
            s->outstanding_ids[s->outstanding_count++] = sync_id;
        }
        MxVkSessionSubmitReply rep{};
        rep.status = MX_OK;
        rep.session_id = s->id;
        rep.boot_generation = boot_generation_;
        rep.sync_id = sync_id;
        rep.command_bytes = req.command_bytes;
        rep.outstanding = s->outstanding_count;
        return reply_with(reply, request, VK_SESSION_SUBMIT_REPLY, rep);
    }

    bool on_sync_wait(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSessionSyncWaitRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_SYNC_WAIT, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, req.session_id, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        mx_status_t status = MX_ERR_NOT_FOUND;
        bool tracked = false;
        for (mx_u32_t i = 0; i < s->outstanding_count; ++i) {
            if (s->outstanding_ids[i] == req.sync_id) {
                tracked = true;
                break;
            }
        }
        if (tracked) {
            status = backend_.sync_wait
                         ? backend_.sync_wait(backend_.ctx, s->id, req.sync_id, req.timeout_ns)
                         : MX_ERR_NOT_IMPLEMENTED;
            if (status == MX_OK) {
                // Retire the token: shift the outstanding table down.
                for (mx_u32_t i = 0; i < s->outstanding_count; ++i) {
                    if (s->outstanding_ids[i] == req.sync_id) {
                        for (mx_u32_t j = i + 1; j < s->outstanding_count; ++j) {
                            s->outstanding_ids[j - 1] = s->outstanding_ids[j];
                        }
                        --s->outstanding_count;
                        break;
                    }
                }
            }
        }
        MxVkSessionSyncWaitReply rep{};
        rep.status = status;
        rep.session_id = s->id;
        rep.boot_generation = boot_generation_;
        return reply_with(reply, request, VK_SESSION_SYNC_WAIT_REPLY, rep);
    }

    bool on_surface_create(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSurfaceCreateRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_SURFACE_CREATE, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, req.session_id, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        if ((s->flags & MX_VK_SESSION_FLAG_WSI) == 0) {
            return error(reply, request, MX_ERR_SERVICE_UNAVAILABLE, s->id, 0);
        }
        if (req.width == 0 || req.height == 0 || s->surface_count >= kMaxSurfaces) {
            return error(reply, request, MX_ERR_NOMEM, s->id, s->surface_count);
        }
        mx_u32_t index = 0;
        for (mx_u32_t i = 0; i < kMaxSurfaces; ++i) {
            if (!s->surfaces[i].used && incarnations_[s->index - 1].surface[i] <= MX_VK_OBJECT_MAX_INCARNATION) {
                index = i + 1;
                break;
            }
        }
        if (index == 0) return error(reply, request, MX_ERR_NOMEM, s->id, 0);
        Surface& sf = s->surfaces[index - 1];
        sf.used = true;
        sf.id = issue_id(boot_generation_, MX_VK_OBJECT_SURFACE, s->index, index,
                         incarnations_[s->index - 1].surface[index - 1]);
        sf.width = req.width;
        sf.height = req.height;
        sf.format = req.format;
        ++s->surface_count;

        MxVkSurfaceCreateReply rep{};
        rep.status = MX_OK;
        rep.surface_id = sf.id;
        rep.boot_generation = boot_generation_;
        rep.width = sf.width;
        rep.height = sf.height;
        return reply_with(reply, request, VK_SESSION_SURFACE_CREATE_REPLY, rep);
    }

    bool on_surface_destroy(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSurfaceDestroyRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_SURFACE_DESTROY, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, req.session_id, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        Surface* sf = find_surface(*s, req.surface_id);
        if (!sf) return error(reply, request, MX_ERR_NOT_FOUND, s->id, req.surface_id);
        // A surface with a live swapchain cannot be destroyed out from under it.
        for (mx_u32_t i = 0; i < kMaxSwapchains; ++i) {
            if (s->swapchains[i].used && s->swapchains[i].surface_id == req.surface_id) {
                return error(reply, request, MX_ERR_BUSY, s->id, req.surface_id);
            }
        }
        *sf = Surface{};
        if (s->surface_count > 0) --s->surface_count;
        MxVkSurfaceDestroyReply rep{};
        rep.status = MX_OK;
        rep.surface_id = req.surface_id;
        return reply_with(reply, request, VK_SESSION_SURFACE_DESTROY_REPLY, rep);
    }

    bool on_swapchain_create(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSwapchainCreateRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_SWAPCHAIN_CREATE, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, req.session_id, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        Surface* sf = find_surface(*s, req.surface_id);
        if (!sf) return error(reply, request, MX_ERR_NOT_FOUND, s->id, req.surface_id);
        if (req.image_count == 0 || req.image_count > kMaxImages ||
            s->swapchain_count >= kMaxSwapchains) {
            return error(reply, request, MX_ERR_INVALID, s->id, req.image_count);
        }
        if (req.stride < static_cast<mx_u64_t>(sf->width) * sizeof(mx_u32_t) || req.stride > 0xFFFFFFFFu / sf->height) {
            return error(reply, request, MX_ERR_INVALID, s->id, req.stride);
        }
        const mx_u64_t image_bytes64 = (mx_u64_t)req.stride * sf->height;
        if (image_bytes64 == 0 || image_bytes64 > 0xFFFFFFFFull) {
            return error(reply, request, MX_ERR_OVERFLOW, s->id, image_bytes64);
        }
        const mx_u32_t image_bytes = (mx_u32_t)image_bytes64;
        const mx_u64_t total = (mx_u64_t)image_bytes * req.image_count;

        mx_u32_t sc_index = 0;
        for (mx_u32_t i = 0; i < kMaxSwapchains; ++i) {
            if (!s->swapchains[i].used && incarnations_[s->index - 1].swapchain[i] <= MX_VK_OBJECT_MAX_INCARNATION) {
                sc_index = i + 1;
                break;
            }
        }
        if (sc_index == 0) return error(reply, request, MX_ERR_NOMEM, s->id, 0);
        // Every check that can fail runs before anything is committed, so a
        // create can never half-succeed and leak arena bytes or object slots.
        if (s->object_count + req.image_count > s->max_objects) {
            return error(reply, request, MX_ERR_NOMEM, s->id, s->max_objects);
        }
        if (total > MX_VK_SESSION_MAX_ARENA_BYTES) {
            return error(reply, request, MX_ERR_NOMEM, s->id, total);
        }
        mx_u32_t free_objects = 0;
        for (mx_u32_t i = 0; i < kMaxObjects; ++i) {
            if (!s->objects[i].used && incarnations_[s->index - 1].memory[i] <= MX_VK_OBJECT_MAX_INCARNATION) ++free_objects;
        }
        if (free_objects < req.image_count) return error(reply, request, MX_ERR_NOMEM, s->id, 0);
        mx_u64_t first_offset = 0;
        if (!arena_alloc(*s, (mx_u32_t)total, MX_VK_SESSION_ARENA_ALIGNMENT, &first_offset)) {
            return error(reply, request, MX_ERR_NOMEM, s->id, s->arena_bytes);
        }
        Swapchain& sc = s->swapchains[sc_index - 1];
        sc.used = true;
        sc.id = issue_id(boot_generation_, MX_VK_OBJECT_SWAPCHAIN, s->index, sc_index,
                         incarnations_[s->index - 1].swapchain[sc_index - 1]);
        sc.surface_id = req.surface_id;
        sc.image_count = req.image_count;
        sc.stride = req.stride;
        sc.pixel_format = req.pixel_format;
        for (mx_u32_t i = 0; i < req.image_count; ++i) {
            // Each image has a memory id; the swapchain owns its lifetime.
            mx_u32_t obj_index = 0;
            for (mx_u32_t k = 0; k < kMaxObjects; ++k) {
                if (!s->objects[k].used && incarnations_[s->index - 1].memory[k] <= MX_VK_OBJECT_MAX_INCARNATION) {
                    obj_index = k + 1;
                    break;
                }
            }
            if (obj_index == 0) return error(reply, request, MX_ERR_NOMEM, s->id, 0);
            MemoryObject& obj = s->objects[obj_index - 1];
            obj.used = true;
            obj.id = issue_id(boot_generation_, MX_VK_OBJECT_MEMORY, s->index, obj_index,
                              incarnations_[s->index - 1].memory[obj_index - 1]);
            obj.arena_offset = first_offset + (mx_u64_t)i * image_bytes;
            obj.size = image_bytes;
            obj.memory_type = MX_VK_MEMORY_TYPE_HOST_VISIBLE;
            ++s->object_count;
            sc.images[i].memory_id = obj.id;
            sc.images[i].arena_offset = obj.arena_offset;
            sc.images[i].size = image_bytes;
            sc.images[i].acquired = 0;
        }
        sc.next_image = 0;
        sc.frame_index = 0;
        ++s->swapchain_count;

        MxVkSwapchainCreateReply rep{};
        rep.status = MX_OK;
        rep.swapchain_id = sc.id;
        rep.boot_generation = boot_generation_;
        rep.image_count = sc.image_count;
        rep.image_bytes = image_bytes;
        return reply_with(reply, request, VK_SESSION_SWAPCHAIN_CREATE_REPLY, rep);
    }

    bool on_swapchain_acquire(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSwapchainAcquireRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_SWAPCHAIN_ACQUIRE, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, req.session_id, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        Swapchain* sc = find_swapchain(*s, req.swapchain_id);
        if (!sc) return error(reply, request, MX_ERR_NOT_FOUND, s->id, req.swapchain_id);
        // Skip images still owned by the client. Exhaustion is backpressure.
        mx_u32_t index = sc->next_image;
        mx_u32_t checked = 0;
        while (checked < sc->image_count && sc->images[index].acquired) {
            index = (index + 1) % sc->image_count;
            ++checked;
        }
        if (checked == sc->image_count) return error(reply, request, MX_ERR_BUSY, s->id, sc->id);
        sc->next_image = (index + 1) % sc->image_count;
        SwapchainImage& img = sc->images[index];
        img.acquired = 1;

        MxVkSwapchainAcquireReply rep{};
        rep.status = MX_OK;
        rep.swapchain_id = sc->id;
        rep.image_id = img.memory_id;
        rep.arena_offset = img.arena_offset;
        rep.image_index = index;
        rep.image_bytes = img.size;
        return reply_with(reply, request, VK_SESSION_SWAPCHAIN_ACQUIRE_REPLY, rep);
    }

    bool on_swapchain_present(mx_u32_t client_id, const MxVkFrame& request, MxVkFrame* reply) {
        MxVkSwapchainPresentRequest req{};
        if (mx_vk_slot_decode(&request, VK_SESSION_SWAPCHAIN_PRESENT, sizeof(req), &req) != 0) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, req.session_id, request.length);
        }
        Session* s = find_session(client_id, req.session_id);
        if (!s) return error(reply, request, MX_ERR_NOT_FOUND, req.session_id, 0);
        Swapchain* sc = find_swapchain(*s, req.swapchain_id);
        if (!sc) return error(reply, request, MX_ERR_NOT_FOUND, s->id, req.swapchain_id);
        if (req.image_index >= sc->image_count) {
            return error(reply, request, MX_ERR_INVALID, s->id, req.image_index);
        }
        SwapchainImage& img = sc->images[req.image_index];
        if (!img.acquired) return error(reply, request, MX_ERR_INVALID, s->id, req.image_index);
        if (backend_.present == nullptr) {
            return error(reply, request, MX_ERR_SERVICE_UNAVAILABLE, s->id, 0);
        }
        mx_u32_t copied = 0;
        const mx_status_t rc = backend_.present(backend_.ctx, s->id, sc->id, req.image_index,
                                                img.arena_offset, img.size, &copied);
        if (mx_err(rc)) return error(reply, request, rc, s->id, img.size);
        if (copied > img.size || copied > MX_VK_SHARED_BUFFER_MAX_BYTES) {
            return error(reply, request, MX_ERR_BAD_PROTOCOL, s->id, copied);
        }
        img.acquired = 0;
        ++sc->frame_index;

        MxVkSwapchainPresentReply rep{};
        rep.status = MX_OK;
        rep.swapchain_id = sc->id;
        rep.frame_index = sc->frame_index;
        rep.copied_bytes = copied;
        rep.image_index = req.image_index;
        return reply_with(reply, request, VK_SESSION_SWAPCHAIN_PRESENT_REPLY, rep);
    }

    VulkanBackend backend_{};
    mx_u64_t boot_generation_ = 0;
    Session sessions_[kMaxSessions];
    Incarnations incarnations_[kMaxSessions];
};

} // namespace drv::gpu::linux
