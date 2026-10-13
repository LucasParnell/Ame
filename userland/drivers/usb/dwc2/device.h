#pragma once
// DWC2 device packet engine. It knows endpoint direction/type/packet size,
// not descriptors, CDC, debug records, keyboard usages or terminal commands.
#include "mochi/usb/dwc2_core.h"
#include "mochi/driver.h"
#include "midix/abi/protocols/usb_device.h"
namespace mochi::usb::dwc2 {
struct DeviceEndpoint {
    mx_u64_t id;
    mx_u32_t offset, length, actual, tx_bytes;
    mx_u16_t max_packet;
    MxUsbTransferType type;
    bool configured, active, queued, terminal;
    MxUsbStatus status;
};
struct Device {
    Registers registers;
    SpscEndpoint link;
    libk::bytes payload;
    DeviceEndpoint in[8], out[8];
    MxDeviceEvent setup, event;
    mx_u64_t generation, sequence, setup_deadline;
    mx_u8_t address;
    MxUsbSpeed speed;
    bool active, setup_pending, event_pending, setup_flushing;
    mx_u32_t diag_pcgcctl, diag_dsts, diag_otgctl;
    mx_u32_t read(mx_u32_t reg) const {
        mx_u32_t v = 0;
        registers.read(registers.context, reg, &v);
        return v;
    }
    void write(mx_u32_t reg, mx_u32_t v) const { registers.write(registers.context, reg, v); }
    static mx_u32_t reg(bool input, mx_u32_t ep, mx_u32_t offset = 0) {
        return (input ? kInEndpointBase : kOutEndpointBase) + ep * kEndpointStride + offset;
    }
    void arm_setup() {
        write(reg(false, 0, 0x10), (3u << 29) | (1u << 19) | 64u);
        write(reg(false, 0), read(reg(false, 0)) | (1u << 26) | (1u << 31));
    }
    bool activate(const Registers& regs, const Geometry& geometry) {
        registers = regs;
        if (!geometry.device || !geometry.dynamic_fifo || !geometry.dedicated_fifos ||
            geometry.device_endpoints < 6 || geometry.in_endpoints < 3 ||
            geometry.fifo_words < 1024 || payload.size < 8192) {
            return false;
        }
        const mx_u32_t rx = geometry.fifo_words / 2, tx = (geometry.fifo_words - rx) / 6;
        if (tx < 16) {
            return false;
        }
        // Restart the PHY clock before touching the device state machine. The
        // vendor device init does this first; PCGCCTL is not cleared by the
        // core soft reset, so a PHY left gated by the bootloader would keep
        // the bus silent (no D+ pull-up ever visible to the host).
        write(kPowerClock, 0);
        // Force VBUS-valid and B-valid into the core. This board routes the
        // session comparators through glue we do not drive yet; without these
        // the core refuses to attach even with SFTDISCON=0 (GOTGCTL.bits
        // 2/3 = VBValid override, 6/7 = BValid override).
        {
            const mx_u32_t otg = read(kOtgControl);
            write(kOtgControl, otg | (1u << 2) | (1u << 3) | (1u << 6) | (1u << 7));
        }
        write(kDeviceControl, read(kDeviceControl) | (1u << 1)); // disconnect during construction
        write(kRxFifoSize, rx);
        write(kNonperiodicTxSize, rx | (tx << 16));
        for (mx_u32_t i = 1; i < 6; ++i) {
            write(0x104 + (i - 1) * 4, (rx + tx * i) | (tx << 16));
        }
        write(kDeviceConfig, 0); // USB2 high-speed, address zero
        write(kDeviceInMask, 1u | (1u << 1));
        write(kDeviceOutMask, 1u | (1u << 3));
        write(kDeviceInterruptMask, 0x00010001);
        write(kAhbConfig, kGlobalInterruptEnable);
        write(kInterruptStatus, 0xffffffffu & ~kHostMode);
        write(kInterruptMask,
              kRxAvailable | (1u << 12) | (1u << 13) | (1u << 18) | (1u << 19) | (1u << 29));
        active = true;
        generation = mx_clock_ns() | 1;
        address = 0;
        reset_endpoints();
        write(kDeviceControl, read(kDeviceControl) & ~(1u << 1));
        libk::dma::order_mmio();
        // Cold-path readback for the bring-up log.
        diag_pcgcctl = read(kPowerClock);
        diag_dsts = read(kDeviceStatus);
        diag_otgctl = read(kOtgControl);
        return true;
    }
    void reset_endpoints() {
        libk::zero(in);
        libk::zero(out);
        in[0].configured = out[0].configured = true;
        in[0].max_packet = out[0].max_packet = 64;
        write(reg(true, 0), 1u << 15);
        write(reg(false, 0), 1u << 15);
        write(reg(true, 0, 8), 0xffffffffu);
        write(reg(false, 0, 8), 0xffffffffu);
        write(kDeviceInterruptMask, 0x00010001);
        write(0x834, 0);
        arm_setup();
    }
    bool range(mx_u32_t at, mx_u32_t bytes) const {
        return at <= payload.size && bytes <= payload.size - at;
    }
    void queue_event(mx_u32_t type, const MxDeviceEvent& next) {
        event = next;
        event_pending = true;
        event.reserved = static_cast<mx_u16_t>(type & 0xffff);
    }
    void receive_fifo() {
        // Fully drain RX before publishing endpoint completion or reusing rows.
        for (mx_u32_t budget = 0; budget < 64 && (read(kInterruptStatus) & kRxAvailable);
             ++budget) {
            const auto status = read(kRxStatusPop), ep = status & 15u,
                       bytes = (status >> 4) & 0x7ffu, kind = (status >> 17) & 15u;
            mx_u8_t scratch[512]{};
            if (bytes > sizeof(scratch)) { // drain word-aligned pieces before failing
                for (mx_u32_t i = 0; i < (bytes + 3) / 4; ++i) {
                    (void)read(fifo_register(0));
                }
                mx_exit(MX_ERR_OVERFLOW);
            }
            if (bytes) {
                registers.receive(registers.context, fifo_register(0),
                                  libk::as_bytes(scratch, bytes));
            }
            if (ep == 0 && kind == 6 && bytes == 8) {
                // A new SETUP aborts old EP0 work. Keep only the newest request
                // identity; delayed replies to an older setup cannot activate it.
                // The host can abort an unfinished control IN with a new
                // SETUP. Stop EP0 publication and flush only its TX FIFO before
                // offering the new request; other function FIFOs stay owned.
                if (in[0].active) {
                    write(reg(true,0),read(reg(true,0))|(1u<<27));
                    write(kReset,kTxFlush); libk::dma::order_mmio();
                    setup_flushing=true; setup_deadline=mx_clock_ns()+100000000ull;
                }
                ++sequence;
                in[0].active = out[0].active = false;
                in[0].terminal = out[0].terminal = false;
                setup = {sequence, generation, MxUsbStatus::Success,
                         8,        0,          static_cast<mx_u8_t>(speed),
                         0,        {}};
                setup.setup = {scratch[0], scratch[1], libk::load_le<mx_u16_t>(scratch + 2),
                               libk::load_le<mx_u16_t>(scratch + 4),
                               libk::load_le<mx_u16_t>(scratch + 6)};
                setup_pending = true;
            } else if (kind == 2 && ep < 8 && out[ep].active) {
                auto& row = out[ep];
                if (bytes > row.length - row.actual || !range(row.offset + row.actual, bytes)) {
                    row.status = MxUsbStatus::IoError;
                } else {
                    libk::copy_n(libk::slice(payload, row.offset + row.actual, bytes),
                                 libk::as_bytes(scratch, bytes), bytes);
                    row.actual += bytes;
                }
            }
        }
    }
    void poll() {
        if (active) {
            const auto interrupts = read(kInterruptStatus);
            if (interrupts & (1u << 12)) {
                write(kDeviceConfig, read(kDeviceConfig) & ~(0x7fu << 4));
                ++generation;
                address = 0;
                setup_pending = setup_flushing = false;
                reset_endpoints();
                queue_event(MX_DEVICE_RESET, {0, generation, MxUsbStatus::Success, 0, 0, 0, 0, {}});
                write(kInterruptStatus, 1u << 12);
            }
            if (interrupts & (1u << 13)) {
                const auto detected = (read(kDeviceStatus) >> 1) & 3;
                speed = detected == 0 ? MxUsbSpeed::High : MxUsbSpeed::Full;
                arm_setup();
                write(kInterruptStatus, 1u << 13);
            }
            receive_fifo();
            if (read(kInterruptStatus) & kRxAvailable) {
                return;
            }
            for (mx_u32_t ep = 0; ep < 8; ++ep) {
                for (mx_u32_t direction = 0; direction < 2; ++direction) {
                    const bool input = direction != 0;
                    auto& row = input ? in[ep] : out[ep];
                    const auto interrupt = read(reg(input, ep, 8));
                    if (interrupt) {
                        write(reg(input, ep, 8), interrupt);
                    }
                    if (row.active && (interrupt & 1)) {
                        row.active = false;
                        row.terminal = true;
                        if (input) {
                            row.actual = row.length;
                        }
                        if (!ep && (!input || !row.length)) {
                            arm_setup();
                        }
                    }
                    if (input && row.active && !row.queued &&
                        (read(reg(true, ep, 0x18)) & 0xffff)) {
                        const auto available = (read(reg(true, ep, 0x18)) & 0xffff) * 4;
                        const auto remaining = row.length - row.tx_bytes;
                        const auto bytes = remaining < available ? remaining : available;
                        if (bytes) {
                            registers.transmit(
                                registers.context, fifo_register(ep),
                                libk::slice(payload, row.offset + row.tx_bytes, bytes));
                        }
                        row.tx_bytes += bytes;
                        if (row.tx_bytes == row.length) {
                            row.queued = true;
                            write(0x834, read(0x834) & ~(1u << ep));
                        }
                    }
                }
            }
            write(kInterruptStatus, 1u << 29);
        }
        if (event_pending) {
            const auto type = 0x55440000u | event.reserved;
            auto copy = event;
            copy.reserved = 0;
            if (link.tx().try_push_msg(type, copy)) {
                event_pending = false;
            }
        }
        if (setup_flushing) {
            if (!(read(kReset)&kTxFlush)) setup_flushing=false;
            else if (mx_clock_ns()>=setup_deadline) mx_exit(MX_ERR_FAULT);
        }
        if (setup_pending && !setup_flushing && link.tx().try_push_msg(MX_DEVICE_SETUP, setup)) {
            setup_pending = false;
        }
        for (mx_u32_t ep = 0; ep < 8; ++ep) {
            for (mx_u32_t direction = 0; direction < 2; ++direction) {
                auto& row = direction ? in[ep] : out[ep];
                if (!row.terminal) {
                    continue;
                }
                const MxDeviceEvent done{row.id,
                                         generation,
                                         row.status,
                                         row.actual,
                                         static_cast<mx_u8_t>(ep | (direction ? 128 : 0)),
                                         static_cast<mx_u8_t>(speed),
                                         0,
                                         {}};
                if (link.tx().try_push_msg(MX_DEVICE_COMPLETE, done)) {
                    row.terminal = false;
                }
            }
        }
        MxRingSlot slot{};
        MxDevicePacket packet{};
        if (event_pending || !link.try_recv(&slot) || !message_get(slot, &packet, slot.type)) {
            return;
        }
        const auto ep = packet.endpoint & 15u;
        const bool input = packet.endpoint & 128;
        if (ep >= 8 || packet.endpoint & 0x70u || packet.reserved) {
            return;
        }
        auto& row = input ? in[ep] : out[ep];
        if (!active || packet.generation != generation || (!ep && packet.id != sequence)) {
            return;
        }
        if (slot.type == MX_DEVICE_ADDRESS) {
            if (packet.value <= 127 && !in[0].active && !out[0].active) {
                address = packet.value;
                write(kDeviceConfig,
                      (read(kDeviceConfig) & ~(0x7fu << 4)) | (mx_u32_t{address} << 4));
            }
        } else if (slot.type == MX_DEVICE_STALL) {
            write(reg(true, ep), read(reg(true, ep)) | (1u << 21));
            write(reg(false, ep), read(reg(false, ep)) | (1u << 21));
            if (!ep) {
                arm_setup();
            }
        } else if (slot.type == MX_DEVICE_CONFIGURE && !row.active && !row.terminal && ep) {
            const bool valid = packet.max_packet && packet.max_packet <= 512 &&
                               packet.type != MxUsbTransferType::Isochronous &&
                               packet.type != MxUsbTransferType::Control;
            if (!valid) {
                return;
            }
            row.configured = true;
            row.max_packet = packet.max_packet;
            row.type = packet.type;
            write(reg(input, ep), packet.max_packet | (1u << 15) |
                                      (static_cast<mx_u32_t>(packet.type) << 18) |
                                      (input ? (ep << 22) : 0) | (1u << 28) | (1u << 27));
            write(kDeviceInterruptMask,
                  read(kDeviceInterruptMask) | (1u << (ep + (input ? 0 : 16))));
        } else if ((slot.type == MX_DEVICE_SEND || slot.type == MX_DEVICE_RECEIVE) &&
                   row.configured && !row.active && !row.terminal &&
                   input == (slot.type == MX_DEVICE_SEND) && packet.length <= row.max_packet &&
                   range(packet.offset, packet.length)) {
            row.id = packet.id;
            row.offset = packet.offset;
            row.length = packet.length;
            row.actual = 0;
            row.tx_bytes = 0;
            row.status = MxUsbStatus::Success;
            row.queued = false;
            row.active = true;
            const mx_u32_t bytes = input ? packet.length : row.max_packet;
            write(reg(input, ep, 0x10), bytes | (1u << 19));
            auto ctrl = read(reg(input, ep));
            ctrl &= ~((1u << 21) | (1u << 27));
            write(reg(input, ep), ctrl | (1u << 26) | (1u << 31));
            if (input) {
                write(0x834, read(0x834) | (1u << ep));
            }
            libk::dma::order_mmio();
        }
    }
    void quiesce() {
        if (active) {
            write(kDeviceControl, read(kDeviceControl) | (1u << 1));
            write(kInterruptMask, 0);
        }
        active = false;
        ++generation;
        setup_pending = false;
        // The main controller performs a core reset before allowing the PHY
        // owner to change role, so no endpoint buffer remains borrowed.
        queue_event(MX_DEVICE_DISCONNECT,
                    {0, generation, MxUsbStatus::Disconnected, 0, 0, 0, 0, {}});
        libk::zero(in);
        libk::zero(out);
    }
};
} // namespace mochi::usb::dwc2
