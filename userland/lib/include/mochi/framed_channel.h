#pragma once
// Owning protocol state over mapped rings, without owning their mappings/caps.
// One thread owns each FramedChannel. Callers choose peer placement once;
// same-core calls never spin. Destruction flushes any deferred publication.
#include "mochi/framed_ring.h"
#include "mochi/ipc_budget.h"

namespace mochi {

struct IpcWaitPolicy {
    bool cross_core = false;
    uint32_t poll_budget = 0;
    bool adaptive = false;
    static constexpr IpcWaitPolicy between(uint32_t local_cpu, uint32_t peer_cpu,
                                           uint32_t budget = 1024, bool adapt = true) {
        return {local_cpu != peer_cpu, local_cpu != peer_cpu ? budget : 0, adapt};
    }
};
enum class IpcRole { Caller, Server };
enum class FrameFlush { Immediate, OnWait, Buffered };
// CompleteRecords must be agreed by both peers: each message fits the ring;
// large messages use packed framing. Compatible retains fragmented messages.
enum class FrameProtocol { Compatible, CompleteRecords };

class FramedChannel {
    bool pending_data_ = false, pending_credit_ = false, polling_ = false;
    IpcWaitPolicy policy_;
    IpcRole role_;
    FrameProtocol protocol_;
    BudgetController controller_{};
    mx_status_t notify(bool& pending, CapHandle doorbell) {
        if (!pending)
            return MX_OK;
        const auto rc = mx_doorbell_notify(doorbell);
        if (rc >= 0)
            pending = false;
        return rc;
    }
    uint32_t poll_budget() const {
        return policy_.cross_core ? (policy_.adaptive ? controller_.budget : policy_.poll_budget)
                                  : 0;
    }
    mx_status_t wait(bool writable, uint32_t space = 1) {
        reader_.commit();
        batch_.commit();
        if (polling_) {
            const auto rc = flush();
            if (rc < 0)
                return rc;
            libk::cpu::relax();
            return MX_OK;
        }
        // At most one wake is carried by the syscall. Flush the second first,
        // so neither a data waiter nor a credit waiter can be stranded.
        if (pending_data_ && pending_credit_) {
            const auto rc = notify(pending_credit_, incoming.space_doorbell);
            if (rc < 0)
                return rc;
        }
        if (pending_data_ || pending_credit_) {
            const bool data = pending_data_;
            const auto doorbell = data ? outgoing.doorbell : incoming.space_doorbell;
            const uint32_t flags =
                data && !writable && !policy_.cross_core
                    ? (role_ == IpcRole::Server ? MX_IPC_EXCHANGE_REPLY : MX_IPC_EXCHANGE_HANDOFF)
                    : 0;
            const bool ready = writable ? outgoing.wait_writable_after_notify(doorbell, space)
                                        : incoming.wait_readable_after_notify(doorbell, flags);
            if (!ready)
                return MX_ERR_DEAD;
            (data ? pending_data_ : pending_credit_) = false;
            return MX_OK;
        }
        bool entered_kernel = false;
        mx_status_t status = MX_OK;
        const bool ready =
            writable ? outgoing.wait_cursor(true, space, poll_budget(), &entered_kernel, &status)
                     : incoming.wait_readable(poll_budget(), &entered_kernel, &status);
        // This is an observed kernel entry, not proof that the kernel slept.
        // The policy is local to the channel; it never changes other channels'
        // budgets or requires a policy syscall for each adaptation.
        if (policy_.adaptive && policy_.cross_core)
            controller_.note_wait(entered_kernel);
        return ready ? MX_OK : (status < 0 ? status : MX_ERR_DEAD);
    }

  public:
    Ring outgoing, incoming;

  private:
    FrameBatchWriter batch_;
    FrameBatchReader reader_;

  public:
    FramedChannel(Ring tx, Ring rx, IpcWaitPolicy policy = {}, IpcRole role = IpcRole::Caller,
                  FrameProtocol protocol = FrameProtocol::Compatible)
        : policy_(policy), role_(role), protocol_(protocol), outgoing(tx), incoming(rx),
          batch_(outgoing), reader_(incoming) {
        if (!policy_.cross_core)
            policy_.poll_budget = 0;
        if (policy_.poll_budget > BudgetController::kMax)
            policy_.poll_budget = BudgetController::kMax;
        controller_.seed(policy_.poll_budget);
        outgoing.peer_polls = incoming.peer_polls = false;
        outgoing.cross_core = incoming.cross_core = policy_.cross_core;
        outgoing.poll_budget = incoming.poll_budget = policy_.poll_budget;
        // Cross-core publication wakes promptly so production can overlap
        // consumption; local wakes can travel with the next blocking entry.
        outgoing.deferred_notify = policy_.cross_core ? nullptr : &pending_data_;
        incoming.deferred_space_notify = policy_.cross_core ? nullptr : &pending_credit_;
    }
    FramedChannel(const FramedChannel&) = delete;
    FramedChannel& operator=(const FramedChannel&) = delete;
    ~FramedChannel() {
        (void)flush();
    }
    bool valid() const {
        return outgoing.valid() && incoming.valid();
    }
    uint32_t current_poll_budget() const {
        return poll_budget();
    }

    mx_status_t flush() {
        reader_.commit();
        batch_.commit();
        auto rc = notify(pending_credit_, incoming.space_doorbell);
        if (rc < 0)
            return rc;
        if (pending_data_ && role_ == IpcRole::Server) {
            // A handoff caller is waiting on its reply token, not a registered
            // reply-doorbell waiter. An ordinary notify alone cannot release it.
            const auto replied = sys_reply_handoff(MX_OK);
            if (replied == MX_OK) {
                pending_data_ = false;
                return MX_OK;
            }
            if (replied != MX_ERR_INVALID)
                return static_cast<mx_status_t>(replied);
        }
        return notify(pending_data_, outgoing.doorbell);
    }
    // Both endpoints must negotiate this policy. Flush before changing it;
    // never omit notification to a peer which might still be asleep.
    mx_status_t polling(bool enabled) {
        if (enabled == polling_)
            return MX_OK;
        const auto rc = flush();
        if (rc < 0)
            return rc;
        if (enabled && !policy_.cross_core)
            return MX_ERR_INVALID;
        polling_ = enabled;
        outgoing.peer_polls = incoming.peer_polls = enabled;
        return MX_OK;
    }
    mx_status_t send(uint32_t type, uint64_t sequence, const void* data, uint32_t size,
                     FrameFlush mode = FrameFlush::Immediate) {
        if (!valid())
            return MX_ERR_INVALID;
        if (protocol_ == FrameProtocol::CompleteRecords &&
            (frame_slots(size) > outgoing.capacity() ||
             (size > MX_RING_INLINE_PAYLOAD &&
              !(outgoing.raw()->space.features & MX_RING_PACKED_FRAMES))))
            return MX_ERR_INVALID;
        // Replies/ACKs must not leave the peer waiting for retained credits.
        reader_.commit();
        if (mode == FrameFlush::Buffered && frame_slots(size) <= outgoing.capacity() &&
            (size <= MX_RING_INLINE_PAYLOAD ||
             (outgoing.raw()->space.features & MX_RING_PACKED_FRAMES))) {
            for (;;) {
                const auto progress = batch_.push(type, sequence, data, size);
                if (progress == FrameProgress::Invalid)
                    return MX_ERR_BAD_PROTOCOL;
                if (progress == FrameProgress::Complete) {
                    // A peer on another CPU can consume while we prepare the
                    // next record. Publish now to preserve that overlap; a
                    // local peer benefits from amortising the whole run.
                    if (policy_.cross_core)
                        batch_.publish();
                    return MX_OK;
                }
                const auto rc = wait(true, static_cast<uint32_t>(frame_slots(size)));
                if (rc < 0)
                    return rc;
            }
        }
        batch_.commit();
        FrameWriter writer(type, sequence, data, size);
        for (;;) {
            const auto progress = writer.push(outgoing);
            if (progress == FrameProgress::Invalid)
                return MX_ERR_BAD_PROTOCOL;
            if (progress == FrameProgress::Complete)
                return mode == FrameFlush::Immediate ? flush() : MX_OK;
            if (progress == FrameProgress::Blocked) {
                const auto rc = wait(true, writer.required_space(outgoing));
                if (rc < 0)
                    return rc;
            }
        }
    }
    mx_status_t receive(FrameInfo& info, void* bytes, uint32_t capacity) {
        if (!valid())
            return MX_ERR_INVALID;
        batch_.commit();
        // Completing a deferred reply also resolves a possible handoff token,
        // even when the next request has already arrived.
        if (pending_data_ || pending_credit_) {
            const auto rc = wait(false);
            if (rc < 0)
                return rc;
        }
        if (protocol_ == FrameProtocol::CompleteRecords) {
            for (;;) {
                const auto progress = reader_.pop(info, bytes, capacity);
                if (progress == FrameProgress::Invalid) {
                    reader_.commit();
                    return MX_ERR_BAD_PROTOCOL;
                }
                if (progress == FrameProgress::Complete) {
                    // Keep cross-core production overlapping consumption. A
                    // local peer cannot refill until we yield, so coalesce
                    // its credits in bounded runs instead of one RMW/record.
                    if (policy_.cross_core || reader_.pending_slots() >= incoming.capacity() / 4)
                        reader_.release();
                    return MX_OK;
                }
                // wait commits copied records before arming a data wait,
                // including any final partial credit batch.
                const auto rc = wait(false);
                if (rc < 0)
                    return rc;
            }
        }
        FrameReader reader(bytes, capacity);
        for (;;) {
            const auto progress = reader.pop(incoming, 16, !polling_);
            if (progress == FrameProgress::Invalid)
                return MX_ERR_BAD_PROTOCOL;
            if (progress == FrameProgress::Complete) {
                info = {reader.type(), reader.size(), reader.sequence()};
                return MX_OK;
            }
            if (progress == FrameProgress::Blocked) {
                const auto rc = wait(false);
                if (rc < 0)
                    return rc;
            }
        }
    }
    mx_status_t call(uint32_t type, uint64_t sequence, libk::const_bytes request,
                     FrameInfo& response, libk::bytes reply) {
        if (request.size > ~uint32_t{0} || reply.size > ~uint32_t{0})
            return MX_ERR_INVALID;
        const auto rc = send(type, sequence, request.data, static_cast<uint32_t>(request.size),
                             FrameFlush::OnWait);
        return rc < 0 ? rc : receive(response, reply.data, static_cast<uint32_t>(reply.size));
    }
    // Call after the final response, before exiting or abandoning the server
    // protocol. A channel that never received a handoff has no token to finish.
    mx_status_t finish_reply(mx_status_t value = MX_OK) {
        const auto rc = flush();
        if (rc < 0)
            return rc;
        const auto replied = sys_reply_handoff(value);
        return replied == MX_ERR_INVALID ? MX_OK : static_cast<mx_status_t>(replied);
    }
};
} // namespace mochi
