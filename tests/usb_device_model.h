#pragma once
// USB2 packet effects, not a replacement for the device/gadget implementations.
// RX data and phase completions are distinct FIFO entries. Endpoint IRQ bits
// have W1C semantics; hardware clears EPENA when a transfer completes.
#include "../userland/drivers/usb/dwc2/device.h"
#include <cassert>
using namespace mochi::usb::dwc2;
struct Storage { MxRingPage page{}; MxRingSlot slots[8]{}; };
struct Model {
    struct Rx { mx_u32_t status, bytes; mx_u8_t data[512]; };
    mx_u32_t words[1024]{};
    Rx fifo[16]{}, current{};
    mx_u8_t tx[512]{};
    mx_u32_t head = 0, tail = 0, tx_bytes = 0;
    bool hold_flush = false;
    bool pending() const { return head != tail; }
    void receive(mx_u32_t kind, mx_u32_t ep, libk::const_bytes data = {}) {
        assert(head - tail < 16 && data.size <= 512);
        auto& row = fifo[head++ % 16];
        row.status = ep | (static_cast<mx_u32_t>(data.size) << 4) | (kind << 17);
        row.bytes = data.size;
        assert(libk::copy_n(libk::as_bytes(row.data), data, data.size));
    }
    void setup(const MxUsbSetup& setup) {
        mx_u8_t bytes[8]{setup.request_type, setup.request};
        libk::write_le<mx_u16_t>(libk::as_bytes(bytes + 2, 2), setup.value);
        libk::write_le<mx_u16_t>(libk::as_bytes(bytes + 4, 2), setup.index);
        libk::write_le<mx_u16_t>(libk::as_bytes(bytes + 6, 2), setup.length);
        receive(6, 0, libk::as_bytes(bytes));
        receive(4, 0); // SETUP phase completion is separate from the eight bytes
    }
    void setup(mx_u16_t length) { setup({0x80, 6, 0x100, 0, length}); }
    void ack_in(mx_u32_t ep = 0) {
        assert(words[Device::reg(true, ep) / 4] & (1u << 31));
        words[Device::reg(true, ep) / 4] &= ~(1u << 31);
        words[Device::reg(true, ep, 8) / 4] |= 1;
    }
    void out_packet(mx_u32_t ep, libk::const_bytes data = {}, bool endpoint_irq = false) {
        assert(words[Device::reg(false, ep) / 4] & (1u << 31));
        receive(2, ep, data);
        receive(3, ep);
        if (endpoint_irq) words[Device::reg(false, ep, 8) / 4] |= 1;
    }
    Registers registers() {
        return {this,
            [](void* p, mx_u32_t reg, mx_u32_t* out) {
                auto& m = *static_cast<Model*>(p);
                if (!out || reg >= sizeof(m.words) || reg % 4) return false;
                *out = m.words[reg / 4];
                if (reg == kInterruptStatus) *out |= m.pending() ? kRxAvailable : 0;
                if (reg == kRxStatusPop) {
                    assert(m.pending());
                    m.current = m.fifo[m.tail++ % 16];
                    *out = m.current.status;
                    const auto kind = (*out >> 17) & 15u;
                    if (kind == 6) m.words[Device::reg(true, 0) / 4] &= ~(1u << 31);
                    if (kind == 3 || kind == 4) {
                        m.words[Device::reg(false, *out & 15u) / 4] &= ~(1u << 31);
                    }
                }
                if (reg >= kInEndpointBase && reg < kOutEndpointBase &&
                    (reg - kInEndpointBase) % 32 == 0x18) *out = 128;
                return true;
            },
            [](void* p, mx_u32_t reg, mx_u32_t value) {
                auto& m = *static_cast<Model*>(p);
                if (reg >= sizeof(m.words) || reg % 4) return false;
                const bool endpoint_irq = reg >= kInEndpointBase && (reg & 31) == 8;
                if (reg == kInterruptStatus || endpoint_irq) m.words[reg / 4] &= ~value;
                else if (reg == kReset) m.words[reg / 4] = kAhbIdle | (m.hold_flush ? value : 0);
                else if (reg == kDeviceControl) m.words[reg / 4] = value & ~(1u << 8);
                else m.words[reg / 4] = value;
                return true;
            },
            [](void* p, mx_u32_t reg, libk::bytes destination) {
                auto& m = *static_cast<Model*>(p);
                return reg == fifo_register(0) && destination.size == m.current.bytes &&
                       libk::copy_n(destination, libk::as_bytes(m.current.data), destination.size);
            },
            [](void* p, mx_u32_t reg, libk::const_bytes source) {
                auto& m = *static_cast<Model*>(p);
                m.tx_bytes = source.size;
                return reg >= kFifoBase && libk::copy_n(libk::as_bytes(m.tx), source, source.size);
            }, 0x40000};
    }
};
