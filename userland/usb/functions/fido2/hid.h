#pragma once
#include "authenticator.h"
namespace mochi::fido2 {
constexpr mx_u32_t kBroadcast = 0xffffffff;
constexpr mx_u64_t kAssemblyTimeout = 3'000'000'000ull, kPresenceTimeout = 30'000'000'000ull;
struct Hid {
    Authenticator auth;
    mx_u32_t next_channel = 1;
    mx_u32_t cid = 0, length = 0, received = 0, sequence = 0;
    mx_u8_t command = 0, input[kMessageSize]{};
    bool assembling = false, waiting = false, approval_pending = false;
    mx_u64_t transactions = 0;
    mx_u64_t deadline = 0, keepalive_at = 0;
    mx_u32_t output_cid = 0, output_size = 0, output_at = 0, output_sequence = 0;
    mx_u8_t output_command = 0, output[kMessageSize]{};
    bool output_pending = false;

    static mx_u32_t read_cid(const mx_u8_t* p) {
        return (mx_u32_t(p[0]) << 24) | (mx_u32_t(p[1]) << 16) | (mx_u32_t(p[2]) << 8) | p[3];
    }
    static void write_cid(mx_u8_t* p, mx_u32_t value) {
        for (mx_u32_t i = 0; i < 4; ++i) p[i] = value >> (24 - 8 * i);
    }
    bool known(mx_u32_t value) const {
        return value && value < next_channel;
    }
    void reply(mx_u32_t channel, mx_u8_t cmd, libk::const_bytes data) {
        output_cid = channel; output_command = cmd;
        output_size = data.size; output_at = output_sequence = 0;
        libk::copy_n(libk::as_bytes(output), data, data.size);
        output_pending = true;
    }
    void error(mx_u32_t channel, mx_u8_t code) { reply(channel, 0xbf, libk::as_bytes(&code, 1)); }
    void cancel() {
        assembling = waiting = approval_pending = false; deadline = 0;
        auth.status(0x2d);
        auth.needs_presence = false;
        auth.assertion_count = auth.assertion_next = 0;
        crypto_wipe(input, sizeof(input));
    }
    void reset() {
        cancel(); output_pending = false; next_channel = 1;
        crypto_wipe(output, sizeof(output));
    }
    bool report(libk::bytes out) {
        if (!output_pending || out.size != 64) return false;
        libk::zero(out); write_cid(out.data, output_cid);
        mx_u32_t header;
        if (!output_at && !output_sequence) {
            out.data[4] = output_command;
            out.data[5] = output_size >> 8; out.data[6] = output_size;
            header = 7;
        } else {
            out.data[4] = output_sequence - 1;
            header = 5;
        }
        const auto left = output_size - output_at;
        const auto size = left < 64 - header ? left : 64 - header;
        libk::copy_n(libk::slice(out, header, size), libk::as_bytes(output + output_at, size), size);
        output_at += size; ++output_sequence;
        if (output_at == output_size) output_pending = false;
        return true;
    }
    void dispatch(mx_u64_t now) {
        assembling = false;
        if (command == 0x81) { reply(cid, command, libk::as_bytes(input, length)); return; }
        if (command != 0x90) { error(cid, 0x01); return; }
        if (!length) { error(cid, 0x03); return; }
        ++transactions;
        approval_pending = false;
        waiting = auth.begin(libk::as_bytes(input, length), now);
        if (waiting) { deadline = now + kPresenceTimeout; keepalive_at = now; }
        else reply(cid, command, libk::as_bytes(auth.response, auth.response_size));
    }
    void receive(libk::const_bytes packet, mx_u64_t now) {
        if (packet.size != 64 || output_pending) return;
        const auto channel = read_cid(packet.data);
        const auto cmd = packet.data[4];
        if (!channel || (channel == kBroadcast && cmd != 0x86) ||
            (channel != kBroadcast && !known(channel))) { error(channel, 0x0b); return; }
        if (cmd == 0x86) {
            if (packet.data[5] || packet.data[6] != 8) { error(channel, 0x03); return; }
            if ((assembling || waiting) && channel != cid) { error(channel, 0x06); return; }
            cancel();
            mx_u32_t allocated = channel;
            if (channel == kBroadcast) {
                if (next_channel == kBroadcast) { error(channel, 0x06); return; }
                allocated = next_channel++;
            }
            mx_u8_t result[17]{};
            libk::copy_n(libk::as_bytes(result), libk::slice(packet, 7, 8), 8);
            write_cid(result + 8, allocated);
            result[12] = 2; result[13] = 1; result[16] = 0x0c; // CBOR + NMSG
            reply(channel, cmd, libk::as_bytes(result));
            return;
        }
        if (cmd == 0x91) {
            if (packet.data[5] || packet.data[6]) { error(channel, 0x03); return; }
            if (waiting && channel == cid) {
                cancel(); auth.status(0x2d);
                reply(channel, 0x90, libk::as_bytes(auth.response, auth.response_size));
            }
            return;
        }
        if ((assembling || waiting) && channel != cid) { error(channel, 0x06); return; }
        if (waiting) { error(channel, 0x06); return; }
        mx_u32_t header = 5;
        if (cmd & 0x80) {
            if (assembling) { cancel(); error(channel, 0x04); return; }
            cid = channel; command = cmd;
            length = (mx_u32_t(packet.data[5]) << 8) | packet.data[6];
            if (length > kMessageSize) { error(channel, 0x03); return; }
            received = sequence = 0; header = 7; assembling = true;
        } else {
            if (!assembling) return; // no outstanding transaction on this CID
            if (cmd != sequence++) { cancel(); error(channel, 0x04); return; }
        }
        const auto left = length - received, size = left < 64 - header ? left : 64 - header;
        libk::copy_n(libk::as_bytes(input + received, size), libk::slice(packet, header, size), size);
        received += size;
        deadline = now + kAssemblyTimeout;
        if (received == length) dispatch(now);
    }
    void tick(mx_u64_t now, bool physical_press) {
        // Presence::sample consumes the edge. Preserve it while a keepalive or
        // other channel's BUSY reply waits for endpoint capacity.
        if (waiting && physical_press && now < deadline) approval_pending = true;
        if (output_pending) return;
        if (assembling && now >= deadline) { cancel(); error(cid, 0x05); return; }
        if (!waiting) return;
        if (approval_pending || now >= deadline) {
            waiting = false;
            if (approval_pending) auth.finish(true); else auth.status(0x2f);
            approval_pending = false;
            reply(cid, 0x90, libk::as_bytes(auth.response, auth.response_size));
        } else if (now >= keepalive_at) {
            const mx_u8_t status = 2; // UPNEEDED
            reply(cid, 0xbb, libk::as_bytes(&status, 1));
            keepalive_at = now + 100'000'000;
        }
    }
};
// A fresh released -> pressed transition, with 30 ms on each side. Sampling
// starts at the request, so a pre-held key and USB-injected input cannot approve.
struct Presence {
    bool released = false, pressed = false;
    mx_u64_t changed = 0;
    void reset(mx_u64_t now) { released = pressed = false; changed = now; }
    bool sample(bool down, mx_u64_t now) {
        if (down != pressed) { pressed = down; changed = now; }
        if (now - changed < 30'000'000) return false;
        if (!down) { released = true; return false; }
        if (!released) return false;
        released = false;
        return true;
    }
};
} // namespace mochi::fido2
