#pragma once
// Single-owner transfer ledger. Cold resource records and dense hot transfer
// columns live in a registered arena. Hardware execution belongs to the HCD.
#include "libk/slot_pool.h"
#include "libk/bytes.h"
#include "libk/hash.h"
#include "mochi/arena.h"
#include "mochi/usb/platform.h"
#include "midix/abi/protocols/usb_host.h"
#include "midix/usb/descriptors.h"

namespace mochi::usb {

struct SessionScope {
    mx_u64_t principal;
    mx_u64_t generation;
    mx_u64_t controller_generation;
};
inline bool same_scope(const SessionScope& a, const SessionScope& b) {
    return a.principal == b.principal && a.generation == b.generation &&
           a.controller_generation == b.controller_generation;
}
inline bool valid_scope(const SessionScope& scope) {
    return scope.principal && scope.generation && scope.controller_generation;
}
enum class TransferState : mx_u8_t { Free, Queued, Active, Stopping, Terminal };
enum BufferAccess : mx_u32_t { BufferRead = 1, BufferWrite = 2 };
enum class ControlPolicy : mx_u8_t { None, InterfaceClass, InterfaceClassVendor, Bus };

struct EndpointGrant {
    SessionScope owner;
    MxUsbToken device;
    midix::usb::EndpointDescriptor descriptor;
    MxUsbSpeed speed;
    mx_u8_t interface_number;
    ControlPolicy control_policy;
    MxUsbToken control_target = 0; // bus-installed interface lease or device authority
};
struct BufferGrant {
    SessionScope owner;
    libk::bytes cpu;
    mx_u64_t device_address;
    mx_u32_t access;
    CacheMode cache_mode;
    bool direct_dma; // set by the trusted resource owner, never by wire metadata
};
struct TransferLimits {
    mx_u32_t endpoints;
    mx_u32_t buffers;
    mx_u32_t transfers;
    mx_u32_t max_transfer_bytes;
    mx_u32_t supported_types;
};
struct PreparedTransfer {
    MxUsbToken token;
    MxUsbSubmit request;
    MxUsbSetup setup;
    libk::bytes payload;
    mx_u64_t device_address;
    mx_u16_t max_packet;
    MxUsbTransferType type;
    MxUsbDirection direction;
    CacheMode cache_mode;
    bool direct_dma;
};

class TransferEngine {
    struct EndpointRecord { EndpointGrant grant; mx_u32_t references; bool closing; };
    struct BufferRecord { BufferGrant grant; mx_u32_t references; bool closing; };
    struct RequestEntry { SessionScope owner; mx_u64_t id; MxUsbToken token; };
public:
    bool init(Arena& arena, const TransferLimits& limits) {
        if (initialized_ || !limits.endpoints || !limits.buffers || !limits.transfers ||
            !limits.max_transfer_bytes || limits.transfers > (1u << 30) ||
            (limits.supported_types & ~0xfu) || (limits.supported_types & (1u << 1))) return false;
        mx_u32_t request_capacity = 1;
        while (request_capacity < limits.transfers * 2) request_capacity <<= 1;
        Arena candidate = arena;
        auto* endpoint_slots = candidate.alloc<libk::SlotMetadata>(limits.endpoints);
        auto* buffer_slots = candidate.alloc<libk::SlotMetadata>(limits.buffers);
        auto* transfer_slots = candidate.alloc<libk::SlotMetadata>(limits.transfers);
        auto* endpoints = candidate.alloc<EndpointRecord>(limits.endpoints);
        auto* buffers = candidate.alloc<BufferRecord>(limits.buffers);
        auto* states = candidate.alloc<TransferState>(limits.transfers);
        auto* requests = candidate.alloc<MxUsbSubmit>(limits.transfers);
        auto* setups = candidate.alloc<MxUsbSetup>(limits.transfers);
        auto* completions = candidate.alloc<MxUsbCompletion>(limits.transfers);
        auto* endpoint_indices = candidate.alloc<mx_u32_t>(limits.transfers);
        auto* buffer_indices = candidate.alloc<mx_u32_t>(limits.transfers);
        auto* request_positions = candidate.alloc<mx_u32_t>(limits.transfers);
        auto* request_index = candidate.alloc<RequestEntry>(request_capacity);
        if (!endpoint_slots || !buffer_slots || !transfer_slots || !endpoints || !buffers ||
            !states || !requests || !setups || !completions || !endpoint_indices || !buffer_indices ||
            !request_positions || !request_index)
            return false;
        if (!endpoint_pool_.init(endpoint_slots, limits.endpoints) ||
            !buffer_pool_.init(buffer_slots, limits.buffers) ||
            !transfer_pool_.init(transfer_slots, limits.transfers)) return false;
        endpoints_ = endpoints; buffers_ = buffers; states_ = states; requests_ = requests;
        setups_ = setups; completions_ = completions;
        endpoint_indices_ = endpoint_indices; buffer_indices_ = buffer_indices;
        request_positions_ = request_positions; request_index_ = request_index;
        request_mask_ = request_capacity - 1;
        limits_ = limits;
        for (mx_u32_t i = 0; i < limits.transfers; ++i) states_[i] = TransferState::Free;
        for (mx_u32_t i = 0; i < request_capacity; ++i) request_index_[i].token = 0;
        arena = candidate;
        initialized_ = true;
        return true;
    }

    mx_status_t install_endpoint(const EndpointGrant& grant, MxUsbToken* out) {
        if (!initialized_ || !out || !valid_scope(grant.owner) || !grant.device ||
            !midix::usb::valid_speed(grant.speed)) return MX_ERR_INVALID;
        if (grant.descriptor.type == MxUsbTransferType::Control) {
            const auto packet = grant.descriptor.max_packet;
            const bool packet_valid = grant.speed >= MxUsbSpeed::Super ? packet == 512 :
                grant.speed == MxUsbSpeed::High ? packet == 64 :
                grant.speed == MxUsbSpeed::Low ? packet == 8 :
                packet == 8 || packet == 16 || packet == 32 || packet == 64;
            if (grant.descriptor.address != 0 || !packet_valid || !grant.control_target ||
                grant.control_policy == ControlPolicy::None || grant.control_policy > ControlPolicy::Bus)
                return MX_ERR_INVALID;
        } else if (!midix::usb::endpoint_valid(grant.descriptor, grant.speed) ||
                   grant.control_policy != ControlPolicy::None) return MX_ERR_INVALID;
        const auto token = endpoint_pool_.acquire();
        if (!token) return MX_ERR_WOULD_BLOCK;
        endpoints_[endpoint_pool_.index_of(token)] = {grant, 0, false};
        *out = token;
        return MX_OK;
    }

    mx_status_t install_buffer(const BufferGrant& grant, MxUsbToken* out) {
        if (!initialized_ || !out || !valid_scope(grant.owner) || !grant.cpu.data ||
            !grant.cpu.size || !grant.access || (grant.access & ~(BufferRead | BufferWrite)) ||
            grant.cache_mode > CacheMode::NonCoherent) return MX_ERR_INVALID;
        const auto cpu_address = reinterpret_cast<mx_u64_t>(grant.cpu.data);
        if (grant.cpu.size > ~mx_u64_t{0} - cpu_address) return MX_ERR_INVALID;
        if (grant.direct_dma && (grant.cache_mode == CacheMode::NonCoherent ||
            grant.cpu.size - 1 > ~mx_u64_t{0} - grant.device_address)) return MX_ERR_NOT_IMPLEMENTED;
        // Import distinct pool slices once. This makes borrowing O(1) without
        // an overlap scan on every submission, and prevents alias registrations.
        for (mx_u32_t i = 0; i < limits_.buffers; ++i) {
            if (!buffer_pool_.token_at(i)) continue;
            const auto& other = buffers_[i].grant;
            const auto other_cpu = reinterpret_cast<mx_u64_t>(other.cpu.data);
            if (cpu_address < other_cpu + other.cpu.size && other_cpu < cpu_address + grant.cpu.size)
                return MX_ERR_BUSY;
            if (grant.direct_dma && other.direct_dma && grant.device_address <= other.device_address + other.cpu.size - 1 &&
                other.device_address <= grant.device_address + grant.cpu.size - 1) return MX_ERR_BUSY;
        }
        const auto token = buffer_pool_.acquire();
        if (!token) return MX_ERR_WOULD_BLOCK;
        buffers_[buffer_pool_.index_of(token)] = {grant, 0, false};
        *out = token;
        return MX_OK;
    }

    mx_status_t revoke_endpoint(const SessionScope& scope, MxUsbToken token) {
        const auto i = endpoint_pool_.index_of(token);
        if (i == libk::kNoSlot) return MX_ERR_NOT_FOUND;
        if (!same_scope(endpoints_[i].grant.owner, scope)) return MX_ERR_PERM;
        endpoints_[i].closing = true;
        if (!endpoints_[i].references) endpoint_pool_.release(token);
        return MX_OK;
    }
    mx_status_t revoke_buffer(const SessionScope& scope, MxUsbToken token) {
        const auto i = buffer_pool_.index_of(token);
        if (i == libk::kNoSlot) return MX_ERR_NOT_FOUND;
        if (!same_scope(buffers_[i].grant.owner, scope)) return MX_ERR_PERM;
        buffers_[i].closing = true;
        if (!buffers_[i].references) buffer_pool_.release(token);
        return MX_OK;
    }

    mx_status_t submit(const SessionScope& scope, const MxUsbSubmit& wire, mx_u64_t now_ns,
                       MxUsbToken* out) {
        const MxUsbSubmit request = wire; // snapshot before validation of shared metadata
        return accept(scope, request, {}, false, now_ns, out);
    }
    mx_status_t submit_control(const SessionScope& scope, MxUsbToken endpoint,
                               const MxUsbControlSubmit& wire, mx_u64_t now_ns, MxUsbToken* out) {
        const MxUsbControlSubmit control = wire;
        const MxUsbSubmit request{control.request_id, endpoint, control.buffer, control.offset,
                                 control.setup.length, control.deadline_ns, control.flags, 0};
        const auto ei = endpoint_pool_.index_of(endpoint);
        if (ei == libk::kNoSlot) return MX_ERR_NOT_FOUND;
        if (control.interface != endpoints_[ei].grant.control_target) return MX_ERR_PERM;
        return accept(scope, request, control.setup, true, now_ns, out);
    }

    mx_status_t start(MxUsbToken token, PreparedTransfer* out) {
        const auto i = transfer_pool_.index_of(token);
        if (i == libk::kNoSlot || !out) return MX_ERR_INVALID;
        if (states_[i] != TransferState::Queued) return MX_ERR_BUSY;
        const auto& ep = endpoints_[endpoint_indices_[i]].grant;
        const auto& request = requests_[i];
        PreparedTransfer prepared{};
        prepared.token = token; prepared.request = request; prepared.setup = setups_[i];
        prepared.type = ep.descriptor.type;
        prepared.max_packet = ep.descriptor.max_packet;
        prepared.direction = direction(ep, setups_[i]);
        const auto buffer_index = buffer_indices_[i];
        if (buffer_index != libk::kNoSlot) {
            const auto& buffer = buffers_[buffer_index].grant;
            prepared.payload = libk::slice(buffer.cpu, request.offset, request.length);
            prepared.device_address = buffer.direct_dma ? buffer.device_address + request.offset : 0;
            prepared.cache_mode = buffer.cache_mode;
            prepared.direct_dma = buffer.direct_dma;
        }
        *out = prepared;
        states_[i] = TransferState::Active;
        return MX_OK;
    }

    // Queued work can stop immediately. Active work stays pinned until a
    // hardware completion or stop acknowledgement proves DMA has finished.
    mx_status_t stop(MxUsbToken token, MxUsbStatus reason) {
        const auto i = transfer_pool_.index_of(token);
        if (i == libk::kNoSlot || (reason != MxUsbStatus::Cancelled && reason != MxUsbStatus::Timeout &&
            reason != MxUsbStatus::Disconnected && reason != MxUsbStatus::ControllerLost)) return MX_ERR_INVALID;
        if (states_[i] == TransferState::Terminal || states_[i] == TransferState::Stopping) return MX_OK;
        completions_[i].status = reason;
        if (states_[i] == TransferState::Queued) states_[i] = TransferState::Terminal;
        else if (states_[i] == TransferState::Active) states_[i] = TransferState::Stopping;
        else return MX_ERR_INVALID;
        return MX_OK;
    }
    mx_status_t cancel(const SessionScope& scope, mx_u64_t request_id) {
        if (!initialized_) return MX_ERR_NOT_FOUND;
        const auto position = request_position(scope, request_id);
        const auto token = request_index_[position].token;
        return token ? stop(token, MxUsbStatus::Cancelled) : MX_ERR_NOT_FOUND;
    }
    mx_status_t finish(MxUsbToken token, MxUsbStatus status, mx_u32_t actual, mx_u32_t raw_status = 0) {
        const auto i = transfer_pool_.index_of(token);
        if (i == libk::kNoSlot || actual > requests_[i].length || status > MxUsbStatus::IoError)
            return MX_ERR_INVALID;
        if (states_[i] != TransferState::Active && states_[i] != TransferState::Stopping) return MX_ERR_BUSY;
        if (states_[i] != TransferState::Stopping) {
            if (status == MxUsbStatus::Success && actual < requests_[i].length) status = MxUsbStatus::Short;
            completions_[i].status = status;
        }
        completions_[i].actual_length = actual;
        completions_[i].controller_status = raw_status;
        states_[i] = TransferState::Terminal;
        return MX_OK;
    }
    mx_status_t stopped(MxUsbToken token) {
        const auto i = transfer_pool_.index_of(token);
        if (i == libk::kNoSlot || states_[i] != TransferState::Stopping) return MX_ERR_INVALID;
        states_[i] = TransferState::Terminal;
        return MX_OK;
    }
    void expire(mx_u64_t now_ns) {
        for (mx_u32_t i = 0; i < limits_.transfers; ++i)
            if ((states_[i] == TransferState::Queued || states_[i] == TransferState::Active) &&
                requests_[i].deadline_ns && now_ns >= requests_[i].deadline_ns)
                (void)stop(transfer_pool_.token_at(i), MxUsbStatus::Timeout);
    }
    void close_session(const SessionScope& scope) {
        for (mx_u32_t i = 0; i < limits_.transfers; ++i)
            if (states_[i] != TransferState::Free &&
                same_scope(endpoints_[endpoint_indices_[i]].grant.owner, scope))
                (void)stop(transfer_pool_.token_at(i), MxUsbStatus::Disconnected);
        for (mx_u32_t i = 0; i < limits_.endpoints; ++i)
            if (endpoint_pool_.token_at(i) && same_scope(endpoints_[i].grant.owner, scope))
                (void)revoke_endpoint(scope, endpoint_pool_.token_at(i));
        for (mx_u32_t i = 0; i < limits_.buffers; ++i)
            if (buffer_pool_.token_at(i) && same_scope(buffers_[i].grant.owner, scope))
                (void)revoke_buffer(scope, buffer_pool_.token_at(i));
    }

    const MxUsbCompletion* completion(MxUsbToken token) const {
        const auto i = transfer_pool_.index_of(token);
        return i != libk::kNoSlot && states_[i] == TransferState::Terminal ? completions_ + i : nullptr;
    }
    // Call only AFTER a completion is published, or after a dead session is
    // fenced. Failure to publish leaves both the completion and buffer retained.
    mx_status_t retire(MxUsbToken token) {
        const auto i = transfer_pool_.index_of(token);
        if (i == libk::kNoSlot || states_[i] != TransferState::Terminal) return MX_ERR_BUSY;
        const auto ei = endpoint_indices_[i], bi = buffer_indices_[i];
        remove_request(request_positions_[i]);
        auto& endpoint = endpoints_[ei];
        if (--endpoint.references == 0 && endpoint.closing) endpoint_pool_.release(requests_[i].endpoint);
        if (bi != libk::kNoSlot) {
            auto& buffer = buffers_[bi];
            if (--buffer.references == 0 && buffer.closing) buffer_pool_.release(requests_[i].buffer);
        }
        states_[i] = TransferState::Free;
        transfer_pool_.release(token);
        return MX_OK;
    }
    TransferState state(MxUsbToken token) const {
        const auto i = transfer_pool_.index_of(token);
        return i == libk::kNoSlot ? TransferState::Free : states_[i];
    }
    MxUsbToken token_at(mx_u32_t index) const { return transfer_pool_.token_at(index); }
    mx_u32_t available() const { return transfer_pool_.available(); }

private:
    mx_u32_t request_hash(const SessionScope& scope, mx_u64_t id) const {
        const mx_u64_t fields[] = {scope.principal, scope.generation, scope.controller_generation, id};
        return libk::fnv1a32(libk::as_bytes(fields)) & request_mask_;
    }
    mx_u32_t request_position(const SessionScope& scope, mx_u64_t id) const {
        auto position = request_hash(scope, id);
        while (request_index_[position].token) {
            const auto& entry = request_index_[position];
            if (entry.id == id && same_scope(entry.owner, scope)) break;
            position = (position + 1) & request_mask_;
        }
        return position;
    }
    void remove_request(mx_u32_t hole) {
        auto next = (hole + 1) & request_mask_;
        while (request_index_[next].token) {
            const auto ideal = request_hash(request_index_[next].owner, request_index_[next].id);
            if (((hole - ideal) & request_mask_) < ((next - ideal) & request_mask_)) {
                request_index_[hole] = request_index_[next];
                request_positions_[transfer_pool_.index_of(request_index_[hole].token)] = hole;
                hole = next;
            }
            next = (next + 1) & request_mask_;
        }
        request_index_[hole].token = 0;
    }
    static MxUsbDirection direction(const EndpointGrant& ep, const MxUsbSetup& setup) {
        const auto address = ep.descriptor.type == MxUsbTransferType::Control ? setup.request_type : ep.descriptor.address;
        return (address & 0x80u) ? MxUsbDirection::In : MxUsbDirection::Out;
    }
    static bool control_allowed(const EndpointGrant& ep, const MxUsbSetup& setup) {
        const auto type = setup.request_type & 0x60u;
        if (type == 0x60) return false;
        if (ep.control_policy == ControlPolicy::Bus) return true;
        return mx_usb_interface_control_allowed(setup, ep.interface_number) ||
               ((setup.request_type & 0x1fu) == 1 && setup.index == ep.interface_number &&
                type == 0x40 && ep.control_policy == ControlPolicy::InterfaceClassVendor);
    }
    mx_status_t accept(const SessionScope& scope, const MxUsbSubmit& request, const MxUsbSetup& setup,
                       bool control, mx_u64_t now_ns, MxUsbToken* out) {
        if (!initialized_ || !out || !valid_scope(scope) || !request.request_id || request.reserved ||
            request.flags & ~(MX_USB_TRANSFER_ALLOW_SHORT | MX_USB_TRANSFER_SEND_ZLP) ||
            request.length > limits_.max_transfer_bytes) return MX_ERR_INVALID;
        if (request.deadline_ns && request.deadline_ns <= now_ns) return MX_ERR_INVALID;
        const auto ei = endpoint_pool_.index_of(request.endpoint);
        if (ei == libk::kNoSlot || endpoints_[ei].closing) return MX_ERR_NOT_FOUND;
        const auto& ep = endpoints_[ei].grant;
        if (!same_scope(ep.owner, scope)) return MX_ERR_PERM;
        if (control != (ep.descriptor.type == MxUsbTransferType::Control)) return MX_ERR_INVALID;
        if (!(limits_.supported_types & (1u << static_cast<mx_u8_t>(ep.descriptor.type))))
            return MX_ERR_NOT_IMPLEMENTED;
        if (control && !control_allowed(ep, setup)) return MX_ERR_PERM;
        const auto dir = direction(ep, setup);
        if ((request.flags & MX_USB_TRANSFER_SEND_ZLP) &&
            (dir != MxUsbDirection::Out || ep.descriptor.type != MxUsbTransferType::Bulk)) return MX_ERR_INVALID;
        mx_u32_t bi = libk::kNoSlot;
        if (request.buffer) {
            bi = buffer_pool_.index_of(request.buffer);
            if (bi == libk::kNoSlot || buffers_[bi].closing) return MX_ERR_NOT_FOUND;
            const auto& buffer = buffers_[bi];
            if (!same_scope(buffer.grant.owner, scope)) return MX_ERR_PERM;
            if (!(buffer.grant.access & (dir == MxUsbDirection::In ? BufferWrite : BufferRead))) return MX_ERR_PERM;
            if (request.offset > buffer.grant.cpu.size || request.length > buffer.grant.cpu.size - request.offset)
                return MX_ERR_INVALID;
            if (buffer.references) return MX_ERR_BUSY;
        } else if (request.length || request.offset) return MX_ERR_INVALID;
        const auto position = request_position(scope, request.request_id);
        if (request_index_[position].token) return MX_ERR_BUSY;
        const auto token = transfer_pool_.acquire();
        if (!token) return MX_ERR_WOULD_BLOCK;
        const auto i = transfer_pool_.index_of(token);
        requests_[i] = request; setups_[i] = setup;
        completions_[i] = {request.request_id, request.endpoint, MxUsbStatus::Success, 0, 0, 0};
        endpoint_indices_[i] = ei; buffer_indices_[i] = bi;
        request_index_[position] = {scope, request.request_id, token};
        request_positions_[i] = position;
        ++endpoints_[ei].references;
        if (bi != libk::kNoSlot) ++buffers_[bi].references;
        states_[i] = TransferState::Queued;
        *out = token;
        return MX_OK;
    }

    libk::SlotPool endpoint_pool_, buffer_pool_, transfer_pool_;
    EndpointRecord* endpoints_ = nullptr;
    BufferRecord* buffers_ = nullptr;
    TransferState* states_ = nullptr;
    MxUsbSubmit* requests_ = nullptr;
    MxUsbSetup* setups_ = nullptr;
    MxUsbCompletion* completions_ = nullptr;
    mx_u32_t* endpoint_indices_ = nullptr;
    mx_u32_t* buffer_indices_ = nullptr;
    mx_u32_t* request_positions_ = nullptr;
    RequestEntry* request_index_ = nullptr;
    mx_u32_t request_mask_ = 0;
    TransferLimits limits_{};
    bool initialized_ = false;
};

} // namespace mochi::usb
