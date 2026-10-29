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
// Observations belong to the packet executor, with no class or terminal
// interpretation. One service loop writes these lifetime counters; unlike
// endpoint rows they survive a bus reset, so a stalled enumeration is visible.
struct DeviceActivity {
    mx_u32_t resets, enumerations, setups, commands, in_complete, out_complete;
};
// EP0 may receive a packet before its consumer can cross the process boundary
// to post RECEIVE. Retain one hardware-admitted packet for the current SETUP;
// it stays private until the consumer supplies its bounded payload slice.
struct EarlyControlOut {
    mx_u8_t data[64];
    mx_u32_t length;
    MxUsbStatus status;
    bool received, complete;
};
enum class DeviceConnectPhase : mx_u8_t { Idle, Flushing, Settling, Connected, Fault };
enum class SetupFlushPhase : mx_u8_t { Idle, WaitingNak, Flushing, Settling };
struct Device {
    Registers registers;
    SpscEndpoint link;
    libk::bytes payload;
    DeviceEndpoint in[8], out[8];
    MxDeviceEvent setup, event;
    MxDeviceSnapshot snapshot, control_trace{};
    bool trace_active = false;
    bool status_count_reported = false;
    bool rx_audit_reported = false;
    mx_u32_t previous_rx_status = 0;
    mx_u32_t ep0_out_trial = 0;
    bool snapshot_pending;
    mx_u64_t generation, sequence, setup_deadline;
    mx_u8_t address, next_address;
    MxUsbSpeed speed;
    bool active, setup_pending, event_pending, setup_flushing;
    SetupFlushPhase setup_flush_phase = SetupFlushPhase::Idle;
    mx_u64_t setup_settle_at = 0;
    bool setup_received, address_pending, address_status_active;
    EarlyControlOut early_out;
    DeviceActivity activity{};
    DeviceConnectPhase connect_phase = DeviceConnectPhase::Idle;
    mx_u64_t connect_deadline, connect_at;
    mx_u32_t core_id;
    mx_u8_t in_fifo_count;
    mx_u32_t read(mx_u32_t reg) const {
        mx_u32_t v = 0;
        registers.read(registers.context, reg, &v);
        return v;
    }
    void write(mx_u32_t reg, mx_u32_t v) const {
        registers.write(registers.context, reg, v);
    }
    void log_fifo_layout() const {
        // Cold, read-only evidence for payload-looking RX status words. Include
        // all dedicated TX windows: unused bootloader defaults may overlap the
        // windows we program. Never read GRXSTSP or a FIFO data port here.
        constexpr mx_u32_t offsets[] = {
            kAhbConfig, kUsbConfig, 0x05c, kRxFifoSize, kNonperiodicTxSize,
            0x104, 0x108, 0x10c, 0x110, 0x114, 0x118, 0x11c, 0x120,
            0x124, 0x128, 0x12c, 0x130, 0x134, 0x138, 0x13c};
        constexpr char hex[] = "0123456789abcdef";
        constexpr auto register_at = sizeof("USB_FIFO_LAYOUT reg=0x") - 1;
        constexpr auto value_at = sizeof("USB_FIFO_LAYOUT reg=0x00000000 value=0x") - 1;
        for (const auto offset : offsets) {
            char message[] = "USB_FIFO_LAYOUT reg=0x00000000 value=0x00000000\n";
            const auto value = read(offset);
            for (unsigned i = 0; i < 8; ++i) {
                message[register_at + i] = hex[(offset >> (28 - 4 * i)) & 15];
                message[value_at + i] = hex[(value >> (28 - 4 * i)) & 15];
            }
            sys_debug_puts(message, sizeof(message) - 1);
        }
    }
    void audit_rx_status(mx_u32_t peek, mx_u32_t status) {
        const auto kind = (status >> 17) & 15u;
        const bool known = kind == 1 || kind == 2 || kind == 3 || kind == 4 || kind == 6;
        if (!rx_audit_reported && (peek != status || (status & 0xfe000000u) || !known)) {
            char message[] = "USB_RX_AUDIT peek=0x00000000 pop=0x00000000 prev=0x00000000 gint=0x00000000\n";
            constexpr unsigned offsets[] = {
                sizeof("USB_RX_AUDIT peek=0x") - 1,
                sizeof("USB_RX_AUDIT peek=0x00000000 pop=0x") - 1,
                sizeof("USB_RX_AUDIT peek=0x00000000 pop=0x00000000 prev=0x") - 1,
                sizeof("USB_RX_AUDIT peek=0x00000000 pop=0x00000000 prev=0x00000000 gint=0x") - 1};
            const mx_u32_t values[] = {peek, status, previous_rx_status, read(kInterruptStatus)};
            constexpr char hex[] = "0123456789abcdef";
            for (unsigned word = 0; word < 4; ++word)
                for (unsigned i = 0; i < 8; ++i)
                    message[offsets[word] + i] = hex[(values[word] >> (28 - 4 * i)) & 15];
            sys_debug_puts(message, sizeof(message) - 1);
            rx_audit_reported = true;
        }
        previous_rx_status = status;
    }
    static mx_u32_t reg(bool input, mx_u32_t ep, mx_u32_t offset = 0) {
        return (input ? kInEndpointBase : kOutEndpointBase) + ep * kEndpointStride + offset;
    }
    void trace_state() {
        control_trace.length = (control_trace.length & (15u | (7u << 9))) |
            (static_cast<mx_u32_t>(early_out.received) << 4) |
            (static_cast<mx_u32_t>(early_out.complete) << 5) |
            (static_cast<mx_u32_t>(out[0].active) << 6) |
            (static_cast<mx_u32_t>(out[0].terminal) << 7) |
            (static_cast<mx_u32_t>(setup_pending) << 8);
    }
    void trace_trial_receive(mx_u32_t mode) {
        if (!trace_active || !mode) return;
        control_trace.global_interrupt = read(reg(false,0));
        control_trace.flags = read(reg(false,0,0x10));
        control_trace.length |= (1u << 9) | (mode << 10);
        trace_state();
    }
    void arm_setup() {
        // On revision 3.x, do not replace a live OUT transfer's size. Reserve
        // the vendor's three eight-byte SETUP credits when it becomes idle.
        if ((core_id & 0xffffu) >= 0x300au && (read(reg(false, 0)) & (1u << 31))) {
            return;
        }
        write(reg(false, 0, 0x10), (3u << 29) | (1u << 19) | 24u);
        write(reg(false, 0), read(reg(false, 0)) | (1u << 26) | (1u << 31));
    }
    void arm_control_out() {
        // Keep one maximum-size EP0 packet plus aborting SETUP credits live
        // while the policy process prepares its next command. The private
        // early_out row retains a packet accepted before RECEIVE is posted.
        // Preserve the live hardware window. Rewriting it after SETUPDONE
        // regressed enumeration on the phone (v25 and v26).
        if (read(reg(false, 0)) & (1u << 31)) {
            return;
        }
        write(reg(false, 0, 0x10), (3u << 29) | (1u << 19) | 64u);
        write(reg(false, 0), read(reg(false, 0)) | (1u << 26) | (1u << 31));
    }
    bool activate(const Registers& regs, const Geometry& geometry) {
        registers = regs;
        if (!geometry.device || !geometry.dynamic_fifo || !geometry.dedicated_fifos ||
            geometry.device_endpoints < 7 || geometry.in_endpoints < 4 || geometry.in_endpoints > 15 ||
            geometry.fifo_words < 1024 || payload.size < 8192) {
            return false;
        }
        // Each SEND is at most one 512-byte packet. Give every implemented
        // IN FIFO space for that packet, including currently unused FIFOs,
        // and dedicate the remaining RAM to RX. Leaving reset/default TX
        // windows untouched can leave them outside RAM or overlapping RX.
        constexpr mx_u32_t tx = 512 / 4;
        const mx_u32_t fifos = geometry.in_endpoints + 1;
        if (geometry.fifo_words < fifos * tx + 512) {
            return false;
        }
        const mx_u32_t rx = geometry.fifo_words - fifos * tx;
        in_fifo_count = static_cast<mx_u8_t>(fifos);
        core_id = geometry.core_id;
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
        for (mx_u32_t i = 1; i < fifos; ++i) {
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
        // FIFO pointers must be flushed AFTER their new geometry is installed.
        // Keep the pull-up disconnected until that finishes and the vendor's
        // 150 us disconnect interval has elapsed. The loop advances this cold
        // state machine; no interrupt-masked spin or blocking delay is needed.
        write(kReset, kRxFlush | kTxFlush | (0x10u << 6));
        connect_deadline = mx_clock_ns() + 100000000ull;
        connect_phase = DeviceConnectPhase::Flushing;
        libk::dma::order_mmio();
        return true;
    }
    bool advance_connect(mx_u64_t now) {
        if (connect_phase == DeviceConnectPhase::Connected) {
            return true;
        }
        if (connect_phase == DeviceConnectPhase::Idle ||
            connect_phase == DeviceConnectPhase::Fault) {
            return false;
        }
        if (now >= connect_deadline) {
            connect_phase = DeviceConnectPhase::Fault;
            active = false;
            return false;
        }
        if (connect_phase == DeviceConnectPhase::Flushing) {
            if (read(kReset) & (kRxFlush | kTxFlush)) {
                return false;
            }
            connect_at = now + 150000ull;
            connect_phase = DeviceConnectPhase::Settling;
        }
        if (connect_phase == DeviceConnectPhase::Settling && now >= connect_at) {
            log_fifo_layout();
            write(kDeviceControl, read(kDeviceControl) & ~(1u << 1));
            libk::dma::order_mmio();
            connect_phase = DeviceConnectPhase::Connected;
            return true;
        }
        return false;
    }
    void reset_endpoints() {
        setup_flushing = false;
        setup_flush_phase = SetupFlushPhase::Idle;
        setup_settle_at = 0;
        ep0_out_trial = 0;
        snapshot_pending = false;
        address_pending = address_status_active = setup_received = false;
        libk::zero(early_out);
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
    void write_address(mx_u8_t value) {
        write(kDeviceConfig, (read(kDeviceConfig) & ~(0x7fu << 4)) | (mx_u32_t{value} << 4));
    }
    void finish_packet(bool input, mx_u32_t ep) {
        auto& row = input ? in[ep] : out[ep];
        if (!row.active) {
            return; // FIFO and endpoint IRQ may report the same OUT completion
        }
        if (input) {
            ++activity.in_complete;
            row.actual = row.length;
            if (!ep && !row.length && address_status_active) {
                address = next_address;
                address_pending = address_status_active = false;
            }
        } else {
            ++activity.out_complete;
        }
        row.active = false;
        row.terminal = true;
        if (!input && !ep && trace_active) trace_state();
        if (!ep && (!input || !row.length)) {
            arm_setup();
        }
    }
    bool advance_setup_flush(mx_u64_t now) {
        if (!setup_flushing) return true;
        if (now >= setup_deadline) {
            constexpr char message[] = "USB_EP0_FAIL setup_tx_flush_timeout\n";
            sys_debug_puts(message, sizeof(message) - 1);
            mx_exit(MX_ERR_FAULT);
            return false;
        }
        if (setup_flush_phase == SetupFlushPhase::WaitingNak) {
            // Stop the selected MAC reader before flushing its FIFO. An
            // already disabled EP0 is also quiescent after a new SETUP.
            if ((read(reg(true, 0)) & (1u << 31)) &&
                !(read(reg(true, 0, 8)) & (1u << 6))) return false;
            if (!(read(kReset) & kAhbIdle)) return false;
            write(kReset, kTxFlush);
            libk::dma::order_mmio();
            setup_flush_phase = SetupFlushPhase::Flushing;
            return false;
        }
        if (setup_flush_phase == SetupFlushPhase::Flushing) {
            // GRSTCTL is the only controller register accessed while a
            // flush is in flight (Hi6250 grstctl_t TXFFLSH contract).
            if (read(kReset) & kTxFlush) return false;
            setup_settle_at = now + 1000; // at least three PHY clocks
            setup_flush_phase = SetupFlushPhase::Settling;
            return false;
        }
        if (now < setup_settle_at) return false;
        setup_flushing = false;
        setup_flush_phase = SetupFlushPhase::Idle;
        setup_settle_at = 0;
        return true;
    }
    void receive_fifo() {
        if (setup_flushing) return;
        // Fully drain RX before publishing endpoint completion or reusing rows.
        for (mx_u32_t budget = 0; budget < 64 && (read(kInterruptStatus) & kRxAvailable);
             ++budget) {
            // GRXSTSR observes the queued status without consuming it. Retain
            // the first mismatch/malformed header to distinguish what the
            // core advertised from what the destructive pop returned.
            const auto peek = read(kRxStatusPeek);
            const auto status = read(kRxStatusPop), ep = status & 15u,
                       bytes = (status >> 4) & 0x7ffu, kind = (status >> 17) & 15u;
            audit_rx_status(peek, status);
            mx_u8_t scratch[512];          // only the FIFO-admitted bytes are read below
            // GRXSTSP is a queue of both payload updates and status-only
            // entries. Only OUT DATA (2) and SETUP DATA (6) own FIFO words;
            // completion/NAK counts must not drain another packet's bytes.
            const bool payload_update = kind == 2 || kind == 6;
            if (!payload_update && bytes && !status_count_reported) {
                char message[]="USB_RX_STATUS_ONLY raw=0x00000000\n";
                constexpr auto hex_offset=sizeof(message)-10;
                constexpr char hex[]="0123456789abcdef";
                for (unsigned i=0;i<8;++i) message[hex_offset+i]=hex[(status>>(28-4*i))&15];
                sys_debug_puts(message,sizeof(message)-1);
                status_count_reported=true;
                log_fifo_layout();
            }
            if (payload_update && bytes > sizeof(scratch)) {
                char message[]="USB_EP0_FAIL rx_status_exceeds_packet_scratch raw=0x00000000\n";
                constexpr auto hex_offset=sizeof(message)-10;
                constexpr char hex[]="0123456789abcdef";
                for (unsigned i=0;i<8;++i) message[hex_offset+i]=hex[(status>>(28-4*i))&15];
                sys_debug_puts(message,sizeof(message)-1);
                log_fifo_layout();
                for (mx_u32_t i = 0; i < (bytes + 3) / 4; ++i) {
                    (void)read(fifo_register(0));
                }
                mx_exit(MX_ERR_OVERFLOW);
            }
            if (payload_update && bytes) {
                registers.receive(registers.context, fifo_register(0),
                                  libk::as_bytes(scratch, bytes));
            }
            if (!ep && trace_active && kind != 6) {
                control_trace.transfer_size = status;
                if (kind == 2) {
                    control_trace.length |= 2;
                    control_trace.actual += bytes;
                } else if (kind == 3) {
                    control_trace.length |= 4;
                }
            }
            if (ep == 0 && kind == 6 && bytes == 8) {
                ++activity.setups;
                // A new SETUP can share this poll with the previous status IN
                // ACK. Observe that ACK before cancelling its staged address;
                // otherwise roll back the unacknowledged register preparation.
                if (in[0].active && (read(reg(true, 0, 8)) & 1u)) {
                    finish_packet(true, 0);
                }
                if (address_status_active) {
                    write_address(address);
                }
                address_pending = address_status_active = false;
                libk::zero(early_out);
                // A new SETUP aborts old EP0 work. Keep only the newest request
                // identity; delayed replies to an older setup cannot activate it.
                // The host can abort an unfinished control IN with a new
                // SETUP. Stop EP0 publication and flush only its TX FIFO before
                // offering the new request; other function FIFOs stay owned.
                if (in[0].active) {
                    write(reg(true, 0), (read(reg(true, 0)) & ~(1u << 26)) | (1u << 27));
                    libk::dma::order_mmio();
                    setup_flushing = true;
                    setup_flush_phase = SetupFlushPhase::WaitingNak;
                    setup_deadline = mx_clock_ns() + 100000000ull;
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
                trace_active = !(scratch[0] == 0xc0 && scratch[1] >= 0x5a && scratch[1] <= 0x5f);
                if (trace_active) {
                    control_trace = {};
                    control_trace.tx_bytes = scratch[0] | (mx_u32_t{scratch[1]} << 8) |
                        (mx_u32_t{setup.setup.length} << 16);
                }
                setup_received = true;
                setup_pending = false; // publish after the FIFO's SETUP phase completion
            } else if (ep == 0 && (kind == 4 || kind == 3) && setup_received) {
                setup_received = false;
                setup_pending = true;
                if (trace_active) {
                    control_trace.control = read(reg(false,0));
                    control_trace.interrupt = read(reg(false,0,0x10));
                    control_trace.length |= 1;
                }
                arm_control_out();
            } else if (kind == 2 && ep < 8) {
                if (out[ep].active) {
                    auto& row = out[ep];
                    if (bytes > row.length - row.actual || !range(row.offset + row.actual, bytes)) {
                        row.status = MxUsbStatus::IoError;
                    } else {
                        libk::copy_n(libk::slice(payload, row.offset + row.actual, bytes),
                                     libk::as_bytes(scratch, bytes), bytes);
                        row.actual += bytes;
                    }
                } else if (!ep && sequence) {
                    early_out.status = MxUsbStatus::Success;
                    if (early_out.received || bytes > sizeof(early_out.data)) {
                        early_out.status = MxUsbStatus::IoError;
                    } else {
                        libk::copy_n(libk::as_bytes(early_out.data), libk::as_bytes(scratch),
                                     bytes);
                    }
                    early_out.length = bytes;
                    early_out.received = true;
                }
            } else if (kind == 3 && ep < 8) {
                // Slave/PIO OUT completion comes from GRXSTSP. DMA backends
                // use endpoint interrupts instead. Retain the terminal row
                // until the FIFO is drained and its event can be published.
                if (!ep && !out[0].active && early_out.received) {
                    early_out.complete = true;
                } else {
                    finish_packet(false, ep);
                }
            }
            if (!ep && trace_active) trace_state();
            if (setup_flushing) return;
        }
    }
    void fill_in_fifo(mx_u32_t ep) {
        auto& row = in[ep];
        if (!row.active || row.queued) {
            return;
        }
        const auto remaining = row.length - row.tx_bytes;
        const auto available = (read(reg(true, ep, 0x18)) & 0xffffu) * 4;
        // SEND owns one packet. Do not expose a prefix to the MAC while
        // waiting for the rest of that packet to fit in its TX FIFO.
        if (remaining > available) return;
        const auto bytes = remaining;
        if (bytes) {
            registers.transmit(registers.context, fifo_register(ep),
                               libk::slice(payload, row.offset + row.tx_bytes, bytes));
        }
        row.tx_bytes += bytes;
        if (row.tx_bytes == row.length) {
            row.queued = true;
            write(0x834, read(0x834) & ~(1u << ep));
        }
    }
    bool poll_hardware() {
        if (!advance_setup_flush(mx_clock_ns())) return false;
        if (active) {
            const auto interrupts = read(kInterruptStatus);
            if (interrupts & (1u << 12)) {
                ++activity.resets;
                write(kDeviceConfig, read(kDeviceConfig) & ~(0x7fu << 4));
                ++generation;
                address = 0;
                setup_pending = setup_flushing = false;
                reset_endpoints();
                queue_event(MX_DEVICE_RESET, {0, generation, MxUsbStatus::Success, 0, 0, 0, 0, {}});
                write(kInterruptStatus, 1u << 12);
            }
            if (interrupts & (1u << 13)) {
                ++activity.enumerations;
                const auto detected = (read(kDeviceStatus) >> 1) & 3;
                speed = detected == 0 ? MxUsbSpeed::High : MxUsbSpeed::Full;
                if (!device_turnaround(registers, core_id, speed)) {
                    mx_exit(MX_ERR_FAULT);
                    return false;
                }
                // ENUMDONE activates EP0: clear the global nonperiodic IN
                // NAK as in dwc_otg_ep0_activate(), then admit SETUP traffic.
                write(kDeviceControl, read(kDeviceControl) | (1u << 8));
                arm_setup();
                write(kInterruptStatus, 1u << 13);
            }
            receive_fifo();
            if (setup_flushing) return false;
            if (read(kInterruptStatus) & kRxAvailable) {
                return false;
            }
            for (mx_u32_t ep = 0; ep < 8; ++ep) {
                for (mx_u32_t direction = 0; direction < 2; ++direction) {
                    const bool input = direction != 0;
                    auto& row = input ? in[ep] : out[ep];
                    if (!row.configured) {
                        continue;
                    }
                    const auto interrupt = read(reg(input, ep, 8));
                    if (interrupt) {
                        write(reg(input, ep, 8), interrupt);
                    }
                    // This executor uses slave/PIO mode: OUTDONE in the RX
                    // FIFO owns OUT completion. Endpoint XFRC may describe
                    // SETUP or arrive after the FIFO completion and after a
                    // newer RECEIVE was posted; it cannot retire that row.
                    if (input && row.active && (interrupt & 1)) {
                        finish_packet(input, ep);
                    }
                    if (input) {
                        fill_in_fifo(ep);
                    }
                }
            }
            write(kInterruptStatus, 1u << 29);
        }
        return true;
    }
    void publish_events() {
        if (event_pending) {
            const auto type = 0x55440000u | event.reserved;
            auto copy = event;
            copy.reserved = 0;
            if (link.tx().try_push_msg(type, copy)) {
                event_pending = false;
            }
        }
        if (snapshot_pending && link.tx().try_push_msg(MX_DEVICE_SNAPSHOT, snapshot))
            snapshot_pending = false;
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
    }
    // Each accepted command retains its own endpoint row. Drain a bounded
    // burst in ring order so ADDRESS and its status SEND share one CPU turn.
    void command(const MxRingSlot& slot) {
        MxDevicePacket packet{};
        if (!message_get(slot, &packet, slot.type)) {
            return;
        }
        ++activity.commands;
        const auto ep = packet.endpoint & 15u;
        const bool input = packet.endpoint & 128;
        if (ep >= 8 || packet.endpoint & 0x70u || packet.reserved) {
            return;
        }
        auto& row = input ? in[ep] : out[ep];
        if (!active || packet.generation != generation || (!ep && packet.id != sequence)) {
            return;
        }
        if (slot.type == MX_DEVICE_EP0_TRIAL) {
            if (snapshot_pending || packet.endpoint || packet.value > 2 ||
                packet.offset || packet.length) return;
            ep0_out_trial = packet.value;
            snapshot = {packet.id, generation, read(reg(false,0)), read(reg(false,0,8)),
                read(reg(false,0,0x10)), read(kInterruptStatus), ep0_out_trial, 0, 0, 0};
            snapshot_pending = true;
        } else if (slot.type == MX_DEVICE_OBSERVE) {
            if (snapshot_pending) return;
            if (packet.value == MX_DEVICE_OBSERVE_CONTROL_HISTORY && !packet.endpoint) {
                snapshot = control_trace;
                snapshot.id = packet.id;
                snapshot.generation = generation;
                snapshot_pending = true;
                return;
            }
            if (packet.value) return;
            snapshot = {packet.id, generation,
                read(reg(input,ep)), read(reg(input,ep,8)),
                read(reg(input,ep,0x10)), read(kInterruptStatus),
                static_cast<mx_u32_t>(row.configured | (row.active << 1) |
                    (row.queued << 2) | (row.terminal << 3) | (input << 4)) |
                    (static_cast<mx_u32_t>(row.status) << 16),
                row.length, row.actual, row.tx_bytes};
            snapshot_pending = true;
        } else if (slot.type == MX_DEVICE_ADDRESS) {
            if (packet.value <= 127 && !in[0].active && !out[0].active) {
                next_address = packet.value;
                address_pending = true;
            }
        } else if (slot.type == MX_DEVICE_STALL) {
            write(reg(true, ep), read(reg(true, ep)) | (1u << 21));
            write(reg(false, ep), read(reg(false, ep)) | (1u << 21));
            if (!ep) {
                arm_setup();
            }
        } else if (slot.type == MX_DEVICE_CONFIGURE && !row.active && !row.terminal && ep) {
            const bool valid = packet.max_packet && packet.max_packet <= 512 &&
                               (!input || ep < in_fifo_count) &&
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
            const auto trial = !input && !ep && packet.length ? ep0_out_trial : 0;
            if (trial) ep0_out_trial = 0;
            if (!input && !ep && trace_active) {
                control_trace.global_interrupt = read(reg(false,0));
                control_trace.flags = read(reg(false,0,0x10));
                control_trace.length |= 8;
            }
            row.id = packet.id;
            row.offset = packet.offset;
            row.length = packet.length;
            row.actual = 0;
            row.tx_bytes = 0;
            row.status = MxUsbStatus::Success;
            row.queued = false;
            row.active = true;
            if (!input && !ep && trace_active) trace_state();
            if (!input && !ep && early_out.received) {
                row.status = early_out.status;
                if (early_out.length > row.length) {
                    row.status = MxUsbStatus::IoError;
                }
                if (row.status == MxUsbStatus::Success) {
                    libk::copy_n(libk::slice(payload, row.offset, early_out.length),
                                 libk::as_bytes(early_out.data), early_out.length);
                    row.actual = early_out.length;
                }
                const bool complete = early_out.complete;
                libk::zero(early_out);
                if (complete) {
                    finish_packet(false, ep);
                }
                return; // already admitted by hardware; do not re-arm that packet
            }
            if (!input && !ep && (read(reg(false, 0)) & (1u << 31))) {
                // The default is v32's hardware-proven enumeration behavior.
                // A one-shot diagnostic trial is armed only after enumeration.
                if (!trial) return;
                const auto control = read(reg(false, 0));
                if (trial == 1) {
                    write(reg(false,0), (control & ~((1u<<21)|(1u<<27))) | (1u<<26));
                    libk::dma::order_mmio();
                    trace_trial_receive(trial);
                    return;
                }
                const auto size = read(reg(false, 0, 0x10));
                if (!(control & (1u << 17)) &&
                    (size & ((3u << 19) | 127u)) == ((1u << 19) | row.max_packet)) {
                    trace_trial_receive(trial);
                    return;
                }
            }
            const mx_u32_t bytes = input ? packet.length : row.max_packet;
            const mx_u32_t setup_credits = !input && !ep ? (3u << 29) : 0;
            write(reg(input, ep, 0x10), bytes | (1u << 19) | setup_credits);
            if (input && !ep && !packet.length && address_pending) {
                // DWC2's vendor and upstream drivers prepare DCFG before
                // enabling status IN. Software ownership commits at XFRC.
                write_address(next_address);
                address_status_active = true;
            }
            // The new transfer size must reach the core before CNAK admits
            // a DATA or STATUS packet into that window.
            libk::dma::order_mmio();
            auto ctrl = read(reg(input, ep));
            ctrl &= ~((1u << 21) | (1u << 27));
            write(reg(input, ep), ctrl | (1u << 26) | (1u << 31));
            if (trial) {
                libk::dma::order_mmio();
                trace_trial_receive(trial);
            }
            if (input && !ep && packet.length) {
                arm_setup();
            }
            if (input) {
                write(0x834, read(0x834) | (1u << ep));
                // Fill in the same turn as SEND, avoiding a timer/IRQ round
                // trip merely to copy an already-owned CPU payload to PIO.
                fill_in_fifo(ep);
            }
            libk::dma::order_mmio();
        }
    }
    void poll() {
        if (active && connect_phase != DeviceConnectPhase::Connected &&
            !advance_connect(mx_clock_ns())) {
            if (connect_phase == DeviceConnectPhase::Fault) {
                mx_exit(MX_ERR_FAULT);
            }
            return;
        }
        if (!poll_hardware()) {
            return;
        }
        publish_events();
        MxRingSlot slot{};
        for (mx_u32_t i = 0; i < driver::kBatchLimit && !event_pending && !snapshot_pending; ++i) {
            if (!link.try_recv(&slot)) {
                break;
            }
            command(slot);
        }
    }
    // Hardware completions wake the shared IRQ doorbell. Timers exist only
    // for asynchronous connect/flush work, FIFO refill or retained publication.
    mx_u64_t wake_deadline(mx_u64_t now) const {
        mx_u64_t deadline = 0;
        if (active && connect_phase != DeviceConnectPhase::Connected) {
            deadline = connect_phase == DeviceConnectPhase::Settling ? connect_at : now + 100000;
            deadline = libk::earlier_deadline(deadline, connect_deadline);
        }
        if (setup_flushing) {
            deadline = libk::earlier_deadline(deadline,
                setup_flush_phase == SetupFlushPhase::Settling ? setup_settle_at : now + 100000);
            deadline = libk::earlier_deadline(deadline, setup_deadline);
        }
        bool retained = event_pending || setup_pending || snapshot_pending;
        for (mx_u32_t ep = 0; ep < 8; ++ep) {
            retained |= in[ep].terminal || out[ep].terminal;
            if (in[ep].active && !in[ep].queued) {
                retained = true;
            }
        }
        if (retained) {
            deadline = libk::earlier_deadline(deadline, now + driver::kBackpressureRetryNs);
        }
        return deadline;
    }
    bool quiesce() {
        const auto now = mx_clock_ns();
        if (!advance_setup_flush(now)) return false;
        if (active && connect_phase == DeviceConnectPhase::Flushing) {
            if (read(kReset) & (kRxFlush | kTxFlush)) return false;
            connect_at = now + 150000;
            connect_phase = DeviceConnectPhase::Settling;
            return false;
        }
        if (active && connect_phase == DeviceConnectPhase::Settling && now < connect_at)
            return false;
        ep0_out_trial = 0;
        if (active) {
            write(kDeviceControl, read(kDeviceControl) | (1u << 1));
            write(kInterruptMask, 0);
        }
        active = false;
        address_pending = address_status_active = setup_received = false;
        connect_phase = DeviceConnectPhase::Idle;
        ++generation;
        setup_pending = false;
        // The main controller performs a core reset before allowing the PHY
        // owner to change role, so no endpoint buffer remains borrowed.
        queue_event(MX_DEVICE_DISCONNECT,
                    {0, generation, MxUsbStatus::Disconnected, 0, 0, 0, 0, {}});
        libk::zero(in);
        libk::zero(out);
        return true;
    }
};
} // namespace mochi::usb::dwc2
