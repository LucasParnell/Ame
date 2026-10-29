#pragma once
#include "midix/abi/protocols/fido2.h"
#include "mochi/ring.h"
namespace mochi::fido2 {
struct PromptPublisher {
    mochi::Ring ring;
    MxFido2Prompt message{};
    mx_u8_t rp[253]{};
    bool pending = false;
    void start(mx_u64_t generation, mx_u64_t request, mx_u8_t operation, libk::const_bytes site) {
        if (!site.size || site.size > sizeof(rp)) return;
        libk::copy_n(libk::as_bytes(rp), site, site.size);
        message = {generation, request, 0, 0, static_cast<mx_u16_t>(site.size), operation,
                   MX_FIDO2_WAITING, {}};
        pending = true;
    }
    void finish(mx_u8_t state) {
        message.offset = message.length = message.total = 0;
        message.state = state;
        pending = message.request != 0;
    }
    bool flush() {
        if (!pending || !ring.valid()) return false;
        const mx_u32_t left = message.total - message.offset;
        message.length = left < sizeof(message.rp) ? left : sizeof(message.rp);
        libk::zero(message.rp);
        libk::copy_n(libk::as_bytes(message.rp), libk::as_bytes(rp + message.offset, message.length), message.length);
        if (!ring.try_push_msg(MX_FIDO2_PROMPT, message)) return false;
        message.offset += message.length;
        pending = message.offset != message.total;
        return true;
    }
};
struct PromptView {
    mx_u64_t generation = 0, request = 0, expires = 0;
    mx_u16_t received = 0, total = 0;
    mx_u8_t operation = 0, state = 0;
    char rp[254]{};
    bool accept(const MxRingSlot& slot, mx_u64_t now) {
        MxFido2Prompt m{};
        if (!mochi::message_get(slot, &m, MX_FIDO2_PROMPT) || !m.generation || !m.request ||
            (m.operation != 1 && m.operation != 2 && m.operation != MX_FIDO2_SELECT) || m.state < MX_FIDO2_WAITING ||
            m.state > MX_FIDO2_SELECTED || m.total > 253 || m.length > sizeof(m.rp) ||
            m.offset > m.total || m.length > m.total - m.offset ||
            m.generation < generation || (m.generation == generation && m.request < request)) return false;
        if (m.state != MX_FIDO2_WAITING) {
            if (m.offset || m.length || m.total || m.generation != generation || m.request != request) return false;
            state = m.state; expires = 0;
            return true;
        }
        if (!m.total || !m.length) return false;
        if (!m.offset) {
            generation = m.generation; request = m.request;
            received = 0; total = m.total; operation = m.operation; state = 0;
        }
        if (m.generation != generation || m.request != request || m.offset != received ||
            m.total != total || m.operation != operation) return false;
        for (mx_u32_t i = 0; i < m.length; ++i) {
            const auto c = m.rp[i];
            rp[received++] = c >= 32 && c <= 126 ? c : '?';
        }
        if (received != total) return false;
        rp[total] = 0; state = MX_FIDO2_WAITING; expires = now + MX_FIDO2_PRESENCE_NS;
        return true;
    }
    bool expire(mx_u64_t now) {
        if (!expires || now < expires) return false;
        state = MX_FIDO2_EXPIRED; expires = 0;
        return true;
    }
};
} // namespace mochi::fido2
