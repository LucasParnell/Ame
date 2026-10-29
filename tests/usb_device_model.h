#pragma once
// USB2 packet effects, not a replacement for the device/gadget implementations.
// RX data and phase completions are distinct FIFO entries. Endpoint IRQ bits
// have W1C semantics; hardware clears EPENA when a transfer completes.
#include "../userland/drivers/usb/dwc2/device.h"
#include <cassert>
using namespace mochi::usb::dwc2;
struct Storage {
    MxRingPage page{};
    MxRingSlot slots[8]{};
};
struct Model {
    struct Rx {
        mx_u32_t status, bytes;
        mx_u8_t data[512];
    };
    mx_u32_t words[1024]{};
    Rx fifo[16]{}, current{};
    mx_u8_t tx[512]{};
    mx_u32_t head = 0, tail = 0, tx_bytes = 0;
    mx_u32_t rx_payload_reads = 0, raw_fifo_reads = 0;
    mx_u32_t unread_rx_words = 0, rx_words_read = 0;
    mx_u32_t accesses_during_flush = 0;
    mx_u32_t tx_space[8]{128, 128, 128, 128, 128, 128, 128, 128};
    mx_u32_t tx_calls = 0;
    mx_u32_t live_ep0_size_writes = 0;
    mx_u32_t live_ep0_size_before_setup_ack = 0;
    bool hold_flush = false;
    bool keep_setup_window_enabled = false;
    bool pending() const {
        return head != tail || unread_rx_words;
    }
    mx_u32_t pop_payload_word() {
        // GRXSTSP and FIFO0 advance one receive stream. A payload must be
        // consumed in whole words before another status can be popped.
        assert(unread_rx_words);
        const auto at = ((current.bytes + 3) / 4 - unread_rx_words) * 4;
        mx_u32_t value = 0;
        for (unsigned i = 0; i < 4 && at + i < current.bytes; ++i)
            value |= mx_u32_t{current.data[at + i]} << (8 * i);
        --unread_rx_words;
        ++rx_words_read;
        return value;
    }
    void receive(mx_u32_t kind, mx_u32_t ep, libk::const_bytes data = {}) {
        assert(head - tail < 16 && data.size <= 512);
        auto& row = fifo[head++ % 16];
        row.status = ep | (static_cast<mx_u32_t>(data.size) << 4) | (kind << 17);
        row.bytes = data.size;
        assert(libk::copy_n(libk::as_bytes(row.data), data, data.size));
    }
    void status(mx_u32_t kind, mx_u32_t ep, mx_u32_t unused_count) {
        assert(kind != 2 && kind != 6 && unused_count <= 0x7ff);
        receive(kind, ep);
        fifo[(head - 1) % 16].status |= unused_count << 4;
    }
    void setup(const MxUsbSetup& setup) {
        mx_u8_t bytes[8]{setup.request_type, setup.request};
        libk::write_le<mx_u16_t>(libk::as_bytes(bytes + 2, 2), setup.value);
        libk::write_le<mx_u16_t>(libk::as_bytes(bytes + 4, 2), setup.index);
        libk::write_le<mx_u16_t>(libk::as_bytes(bytes + 6, 2), setup.length);
        receive(6, 0, libk::as_bytes(bytes));
        receive(4, 0); // SETUP phase completion is separate from the eight bytes
    }
    void setup(mx_u16_t length) {
        setup({0x80, 6, 0x100, 0, length});
    }
    void ack_in(mx_u32_t ep = 0) {
        assert(words[Device::reg(true, ep) / 4] & (1u << 31));
        words[Device::reg(true, ep) / 4] &= ~(1u << 31);
        words[Device::reg(true, ep, 8) / 4] |= 1;
    }
    void out_packet(mx_u32_t ep, libk::const_bytes data = {}, bool endpoint_irq = false) {
        assert(words[Device::reg(false, ep) / 4] & (1u << 31));
        receive(2, ep, data);
        receive(3, ep);
        if (endpoint_irq) {
            words[Device::reg(false, ep, 8) / 4] |= 1;
        }
    }
    Registers registers() {
        return {
            this,
            [](void* p, mx_u32_t reg, mx_u32_t* out) {
                auto& m = *static_cast<Model*>(p);
                if (reg != kReset && (m.words[kReset / 4] & (kTxFlush | kRxFlush)))
                    ++m.accesses_during_flush;
                if (reg == fifo_register(0)) {
                    ++m.raw_fifo_reads;
                    assert(out);
                    *out = m.pop_payload_word();
                    return true;
                }
                if (!out || reg >= sizeof(m.words) || reg % 4) {
                    return false;
                }
                *out = m.words[reg / 4];
                if (reg == kInterruptStatus) {
                    *out |= m.pending() ? kRxAvailable : 0;
                }
                if (reg == kRxStatusPeek) {
                    assert(!m.unread_rx_words);
                    *out = m.head != m.tail ? m.fifo[m.tail % 16].status : 0;
                }
                if (reg == kRxStatusPop) {
                    assert(!m.unread_rx_words && m.head != m.tail);
                    m.current = m.fifo[m.tail++ % 16];
                    *out = m.current.status;
                    const auto kind = (*out >> 17) & 15u;
                    m.unread_rx_words = (kind == 2 || kind == 6) ? (m.current.bytes + 3) / 4 : 0;
                    if (kind == 6) {
                        m.words[Device::reg(true, 0) / 4] &= ~(1u << 31);
                        if (m.keep_setup_window_enabled) {
                            auto& size=m.words[Device::reg(false,0,0x10)/4];
                            const auto credits=(size>>29)&3u;
                            const auto bytes=size&127u;
                            size=(size&~((3u<<29)|127u)) |
                                ((credits ? credits-1 : 0)<<29) | (bytes>=8 ? bytes-8 : 0);
                        }
                    }
                    if (kind == 4 && m.keep_setup_window_enabled) {
                        m.words[Device::reg(false,0)/4] |= 1u<<17; // NAK after SETUP
                    }
                    if (kind == 4) {
                        m.words[Device::reg(false,0,8)/4] |= 1u<<3; // SETUP interrupt is W1C
                    }
                    if (kind == 3 || (kind == 4 && !m.keep_setup_window_enabled)) {
                        m.words[Device::reg(false, *out & 15u) / 4] &= ~(1u << 31);
                    }
                }
                if (reg >= kInEndpointBase && reg < kOutEndpointBase &&
                    (reg - kInEndpointBase) % 32 == 0x18) {
                    *out = m.tx_space[(reg - kInEndpointBase) / kEndpointStride];
                }
                return true;
            },
            [](void* p, mx_u32_t reg, mx_u32_t value) {
                auto& m = *static_cast<Model*>(p);
                if (m.words[kReset / 4] & (kTxFlush | kRxFlush)) ++m.accesses_during_flush;
                if (reg >= sizeof(m.words) || reg % 4) {
                    return false;
                }
                const bool endpoint_irq = reg >= kInEndpointBase && (reg & 31) == 8;
                if (reg == Device::reg(false, 0, 0x10) &&
                    (m.words[Device::reg(false, 0) / 4] & (1u << 31))) {
                    ++m.live_ep0_size_writes;
                    if (m.words[Device::reg(false,0,8)/4] & (1u<<3)) {
                        ++m.live_ep0_size_before_setup_ack;
                    }
                }
                if (reg == kInterruptStatus || endpoint_irq) {
                    m.words[reg / 4] &= ~value;
                } else if (reg == kReset) {
                    m.words[reg / 4] = kAhbIdle | (m.hold_flush ? value : 0);
                } else if (reg == kDeviceControl) {
                    m.words[reg / 4] = value & ~(1u << 8);
                } else {
                    if (m.keep_setup_window_enabled && reg == Device::reg(false,0) &&
                        (value & (1u<<26))) value &= ~(1u<<17); // CNAK clears NAKSTS
                    m.words[reg / 4] = value;
                }
                return true;
            },
            [](void* p, mx_u32_t reg, libk::bytes destination) {
                auto& m = *static_cast<Model*>(p);
                if (m.words[kReset / 4] & (kTxFlush | kRxFlush)) ++m.accesses_during_flush;
                ++m.rx_payload_reads;
                if (reg != fifo_register(0) || (destination.size && !destination.data)) return false;
                for (mx_u32_t at = 0; at < destination.size; at += 4) {
                    const auto word = m.pop_payload_word();
                    for (unsigned i = 0; i < 4 && at + i < destination.size; ++i)
                        destination.data[at + i] = static_cast<mx_u8_t>(word >> (8 * i));
                }
                return true;
            },
            [](void* p, mx_u32_t reg, libk::const_bytes source) {
                auto& m = *static_cast<Model*>(p);
                if (m.words[kReset / 4] & (kTxFlush | kRxFlush)) ++m.accesses_during_flush;
                ++m.tx_calls;
                m.tx_bytes = source.size;
                return reg >= kFifoBase && libk::copy_n(libk::as_bytes(m.tx), source, source.size);
            },
            0x40000};
    }
};
