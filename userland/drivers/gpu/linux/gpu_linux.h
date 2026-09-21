#pragma once
// userland/drivers/gpu/linux/gpu_linux.h — drv.gpu.linux, the native Mochi face
// of the Ame GPU appliance (spec §15.1/§15.2).
//
// The *server* half of this service runs inside `dango.manager` (which owns the
// VM, the Dango channel and the appliance authority caps); the *client* half is
// used by `io.framebuffer`. There is deliberately no `drv.gpu.linux` process:
// the V0 kernel cannot pass the Dango channel cap between two services, so the
// only place that can legally drive the appliance is the process that already
// holds it. This header is the one implementation both halves compile against,
// so the wire contract has exactly one home and one set of guards.
//
// Architecture rule: no service that includes this header may name a virtio-gpu
// register or a DRM object. Everything here is expressed in the
// `protocols/gpu_service.h` vocabulary; only `dango.manager` translates a
// request into the Phase H/I appliance opcodes.

#include "mochi/ring.h"
#include "midix/abi/protocols/gpu_service.h"
#include "vulkan_broker.h"

namespace drv::gpu::linux {

// The endpoint pair init hands to both halves: a request ring (client ->
// service), a reply ring (service -> client) and the two doorbells.
struct Endpoint {
    RingPage* request_ring;
    RingPage* reply_ring;
    uint32_t order;
    CapHandle request_doorbell; // client NOTIFY, service WAIT
    CapHandle reply_doorbell;   // client WAIT,   service NOTIFY

    bool valid() const {
        return request_ring && reply_ring &&
               request_doorbell != kNullCap && reply_doorbell != kNullCap;
    }
};

// ---------------------------------------------------------------------------
// Client (io.framebuffer)
//
// The handshake is non-blocking by construction: the client asks once and then
// polls for the reply as it works, so a service that never comes up costs one
// failed poll per present and never a boot delay. Until the service is Live the
// client keeps its own (ramfb) path authoritative, so a failed or refused
// handshake can never leave the shell without a display.
// ---------------------------------------------------------------------------
enum class ClientState : uint32_t {
    Idle = 0,     // endpoint present; INFO not sent yet
    AwaitInfo,
    AwaitRegister,
    Live,
    Failed,       // permanent: refused, or the reply ring stopped draining
};

class Client {
public:
    // Attach to the endpoint and the appliance window geometry the client
    // renders for (built by the caller from the shared Dango binding). An
    // invalid endpoint puts the client straight into Failed, where it stays.
    void attach(const Endpoint& endpoint, const MxGpuServiceRegisterStagingRequest& window);

    bool attached() const { return endpoint_.valid(); }
    ClientState state() const { return state_; }
    bool live() const { return state_ == ClientState::Live; }

    // Advance the handshake and absorb any completed presents. Never blocks.
    void poll();

    // Queue one present. `damage` may be null only when `full_frame` is set.
    // Returns false when the request could not be queued (the caller keeps the
    // damage so the next present still carries it) or the service is not Live.
    bool submit_present(const MxGpuDamageRect* damage, bool full_frame);

private:
    void fail() { state_ = ClientState::Failed; }
    bool push(uint32_t type, const void* payload, uint32_t length);

    Endpoint endpoint_{};
    ClientState state_ = ClientState::Idle;
    MxGpuServiceRegisterStagingRequest window_{};
    uint64_t buffer_id_ = 0;
    bool register_sent_ = false;
    uint32_t dropped_presents_ = 0;
};

// ---------------------------------------------------------------------------
// Server (dango.manager)
//
// The facts the service validates a client against are the appliance staging
// binding (single source: the shared Dango binding) and the registered buffer
// token minted by the Phase H/I exchange.
// ---------------------------------------------------------------------------
struct ServerConfig {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint64_t staging_pa;
    uint64_t staging_size;
};

// Called for each validated PRESENT with a damage rectangle already clamped to
// the window. Implementations copy the damaged band into the appliance staging
// window and drive the appliance present; returning false yields a
// GPU_SERVICE_ERROR for that present.
using PresentFn = bool (*)(void* ctx, const MxGpuDamageRect& damage, uint32_t flags);

// Consecutive failed presents the service tolerates before it stops answering
// PRESENT with success. Bounded so a wedged appliance degrades the client to
// its local path instead of stalling the client forever.
constexpr uint32_t kMaxConsecutivePresentFailures = 3;

// Consecutive presents the client may fail to queue (reply ring not draining)
// before it gives up on the service and keeps its local path for good.
constexpr uint32_t kMaxConsecutiveDroppedPresents = 64;

class Server {
public:
    void init(const Endpoint& endpoint, const ServerConfig& config);

    // Publish the opaque buffer token clients must present with. Set once the
    // appliance exchange names the registered staging buffer; before that a
    // present is refused with MX_ERR_SERVICE_UNAVAILABLE.
    void set_buffer(uint64_t buffer_id, uint64_t boot_generation);
    bool buffer_ready() const { return buffer_id_ != 0; }

    // Bind the Vulkan session broker this endpoint serves (spec §14.7). The
    // broker owns one logical session per client identity on this ring and
    // enforces every quota; `backend` executes a session's work (the guest
    // dango-vkd in production). Passing a backend with no functions brokers
    // sessions that advertise no Vulkan device, so a submit is refused and the
    // native client keeps its local path -- it never fakes a present.
    void init_vulkan(const VulkanBackend& backend, mx_u32_t client_id);

    // Serve one request, blocking on the request doorbell between requests.
    // Returns false only when the endpoint is unusable.
    bool serve_once(PresentFn present, void* ctx);

private:
    bool reply(uint32_t type, const void* payload, uint32_t length);
    bool reply_frame(const MxVkFrame& frame);
    void error(uint32_t opcode, mx_status_t status, uint64_t detail);

    Endpoint endpoint_{};
    ServerConfig config_{};
    uint64_t buffer_id_ = 0;
    uint64_t boot_generation_ = 0;
    uint64_t frame_index_ = 0;
    uint32_t consecutive_failures_ = 0;
    VulkanSessionBroker vk_{};
    mx_u32_t vk_client_id_ = 0;
    bool vk_enabled_ = false;
};

} // namespace drv::gpu::linux
