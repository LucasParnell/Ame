#pragma once
// Controller-local host service. It implements USC1/UST1, never USB class
// requests. Port enumeration policy and HID remain in io.usb and drv.usb.hid.
#include "mochi/usb/dwc2_transfer.h"
#include "mochi/usb/link.h"
#include "mochi/driver.h"
namespace mochi::usb::dwc2 {
constexpr mx_u32_t kEndpoints = 32, kTransfers = 16, kPayloadBytes = 4096;
struct Host {
    Registers registers;
    PacketExecutor packets;
    TransferEngine ledger;
    SessionScope scope;
    SpscEndpoint link;
    libk::bytes payload;
    libk::SlotPool endpoints, devices;
    libk::SlotMetadata endpoint_slots[kEndpoints], device_slots[1], packet_slots[16];
    PacketState packet_states[16];
    PacketCold packet_rows[16];
    Endpoint endpoint_rows[kEndpoints];
    Transfer transfer_rows[kTransfers];
    MxUsbToken buffers[kTransfers], tokens[kTransfers], device, ep0;
    MxUsbControllerReply reply;
    MxUsbPortEvent event;
    mx_u64_t sequence, address_deadline;
    mx_u32_t address_step;
    mx_u8_t address;
    MxUsbSpeed speed;
    bool active, connected, event_pending, reply_pending, addressing;

    bool init(Arena& arena, SpscEndpoint channel, libk::bytes shared) {
        link = channel;
        payload = shared;
        scope = {1, 1, mx_clock_ns() | 1};
        if (!link.valid() || shared.size < kTransfers * kPayloadBytes ||
            !ledger.init(arena, {kEndpoints, kTransfers, kTransfers, kPayloadBytes, 0xd}) ||
            !endpoints.init(endpoint_slots, kEndpoints) || !devices.init(device_slots, 1)) {
            return false;
        }
        for (mx_u32_t i = 0; i < kTransfers; ++i) {
            if (ledger.install_buffer({scope, libk::slice(shared, i * kPayloadBytes, kPayloadBytes),
                                       0, BufferRead | BufferWrite, CacheMode::Coherent, false},
                                      &buffers[i]) != MX_OK) {
                return false;
            }
        }
        return true;
    }
    mx_u32_t read(mx_u32_t reg) const {
        mx_u32_t v = 0;
        (void)registers.read(registers.context, reg, &v);
        return v;
    }
    void write(mx_u32_t reg, mx_u32_t v) const { (void)registers.write(registers.context, reg, v); }
    void port(mx_u32_t set, mx_u32_t clear, mx_u32_t ack = 0) {
        mx_u32_t value = 0;
        if (port_write(read(kHostPort), set, clear, ack, &value)) {
            write(kHostPort, value);
        }
    }
    mx_u32_t frame_interval(MxUsbSpeed bus_speed) const {
        // UTMI stays at 30/60 MHz; low-power PHY clock switching is disabled.
        // High-speed HFIR counts a microframe, full/low-speed a whole frame.
        const auto mhz = (read(kUsbConfig) & (1u << 3)) ? 30u : 60u;
        return (bus_speed == MxUsbSpeed::High ? 125u : 1000u) * mhz - 1;
    }
    bool activate(const Registers& regs, const Geometry& geometry) {
        registers = regs;
        const auto words = geometry.fifo_words;
        if (words < 768) {
            return false;
        }
        const auto rx = static_cast<mx_u16_t>(words / 2), tx = static_cast<mx_u16_t>(words / 4);
        if (!prepare_host_pio(
                regs, geometry,
                {{0, rx},
                 {rx, tx},
                 {static_cast<mx_u16_t>(rx + tx), static_cast<mx_u16_t>(words - rx - tx)}})) {
            return false;
        }
        packets.fenced();
        if (!packets.init(regs, geometry, {packet_slots, packet_states, packet_rows, 16})) {
            return false;
        }
        write(kHostConfig, 0);
        write(kHostFrameInterval, frame_interval(MxUsbSpeed::Full));
        write(kInterruptStatus, 0xffffffffu & ~kHostMode);
        write(kInterruptMask,
              kSof | kRxAvailable | kPortInterrupt | kChannelInterrupt | (1u << 29));
        port(kPortPower, 0);
        active = true;
        connected = false;
        return true;
    }
    Endpoint* endpoint(MxUsbToken token) {
        const auto i = endpoints.index_of(token);
        return i == libk::kNoSlot ? nullptr : &endpoint_rows[i];
    }
    mx_status_t install(mx_u8_t addr, MxUsbTransferType type, mx_u16_t packet, mx_u8_t interval,
                        MxUsbToken& out) {
        midix::usb::EndpointDescriptor desc{};
        desc.address = addr;
        desc.type = type;
        desc.max_packet = packet;
        desc.interval = interval;
        desc.transactions = 1;
        if (type != MxUsbTransferType::Control && !midix::usb::endpoint_valid(desc, speed)) {
            return MX_ERR_INVALID;
        }
        const auto token = endpoints.acquire();
        if (!token) {
            return MX_ERR_WOULD_BLOCK;
        }
        MxUsbToken grant = 0;
        const auto rc = ledger.install_endpoint(
            {scope, device, desc, speed, 0,
             type == MxUsbTransferType::Control ? ControlPolicy::Bus : ControlPolicy::None, device},
            &grant);
        if (rc != MX_OK) {
            endpoints.release(token);
            return rc;
        }
        // Public tokens and ledger tokens may have different generations.
        auto& ep = endpoint_rows[endpoints.index_of(token)];
        ep = {grant, device, 0, 0, packet, addr, address, interval, speed, type, Pid::Data0};
        out = token;
        return MX_OK;
    }
    bool remove(MxUsbToken token) {
        if (token != device || !device) {
            return false;
        }
        for (mx_u32_t i = 0; i < kEndpoints; ++i) {
            if (const auto t = endpoints.token_at(i)) {
                auto& ep = endpoint_rows[i];
                if (ep.active) {
                    (void)ledger.stop(ep.active, MxUsbStatus::Disconnected);
                }
                (void)ledger.revoke_endpoint(scope, ep.token);
                (void)endpoints.release(t);
            }
        }
        (void)devices.release(device);
        device = ep0 = 0;
        return true;
    }
    void complete_reply(mx_u64_t id, mx_status_t rc, MxUsbToken end = 0) {
        reply = {id, device, end, rc, 0, speed, {}};
        reply_pending = true;
    }
    void submit(MxUsbSubmit wire, const MxUsbControlSubmit* control = nullptr) {
        auto* ep = endpoint(wire.endpoint);
        if (!active || !ep || ep->active) {
            complete_reply(wire.request_id, ep ? MX_ERR_BUSY : MX_ERR_NOT_FOUND);
            return;
        }
        const auto row = wire.offset / kPayloadBytes;
        if (row >= kTransfers || wire.offset % kPayloadBytes + wire.length > kPayloadBytes ||
            (wire.length && wire.buffer != 1)) {
            complete_reply(wire.request_id, MX_ERR_INVALID);
            return;
        }
        const auto public_endpoint = wire.endpoint;
        wire.endpoint = ep->token;
        wire.buffer = wire.length ? buffers[row] : 0;
        wire.offset %= kPayloadBytes;
        MxUsbToken token = 0;
        mx_status_t rc = MX_OK;
        if (control) {
            auto local = *control;
            local.offset = wire.offset;
            local.buffer = wire.buffer;
            rc = ledger.submit_control(scope, ep->token, local, mx_clock_ns(), &token);
        } else {
            rc = ledger.submit(scope, wire, mx_clock_ns(), &token);
        }
        if (rc != MX_OK) {
            complete_reply(wire.request_id, rc);
            return;
        }
        const auto i = static_cast<mx_u32_t>(token) - 1;
        PreparedTransfer prepared{};
        if (i >= kTransfers || ledger.start(token, &prepared) != MX_OK) {
            complete_reply(wire.request_id, MX_ERR_FAULT);
            return;
        }
        prepared.request.endpoint = public_endpoint;
        start(transfer_rows[i], prepared, *ep);
        tokens[i] = token;
        ep->active = token;
    }
    void internal_control(const MxUsbSetup& setup) {
        MxUsbControlSubmit req{
            reply.request_id,           device, 1, mx_clock_ns() + 1000000000ull, setup, 0,
            MX_USB_TRANSFER_ALLOW_SHORT};
        submit({req.request_id, ep0, 1, 0, setup.length, req.deadline_ns, req.flags, 0}, &req);
        if (reply_pending && reply.status != MX_OK) {
            addressing = false;
        }
    }
    // ADDRESS is a cold controller operation like xHCI Address Device. On this
    // core reset, discover EP0's packet size and send SET_ADDRESS explicitly.
    void address_request(const MxUsbControllerRequest& req) {
        if (!active || req.value != 1 || device || !(read(kHostPort) & kPortConnected)) {
            complete_reply(req.request_id, MX_ERR_BUSY);
            return;
        }
        reply = {req.request_id, 0, 0, MX_OK, 0, {}, {}};
        addressing = true;
        address_step = 0;
        address_deadline = mx_clock_ns() + 50000000;
        port(kPortReset, 0);
    }
    void poll_address(mx_u64_t now) {
        if (!addressing || now < address_deadline) {
            return;
        }
        if (address_step == 0) {
            port(0, kPortReset);
            address_step = 1;
            address_deadline = now + 10000000;
            return;
        }
        if (address_step == 1) {
            const auto value = read(kHostPort);
            speed = port_speed(value);
            if (!(value & kPortEnabled) || !static_cast<mx_u8_t>(speed)) {
                addressing = false;
                complete_reply(reply.request_id, MX_ERR_FAULT);
                return;
            }
            write(kHostConfig, 0);
            write(kHostFrameInterval, frame_interval(speed));
            device = devices.acquire();
            address = 0;
            if (!device || install(0, MxUsbTransferType::Control,
                                   speed == MxUsbSpeed::High ? 64 : 8, 0, ep0) != MX_OK) {
                addressing = false;
                complete_reply(reply.request_id, MX_ERR_FAULT);
                return;
            }
            address_step = 2;
            address_deadline = 0;
            internal_control({0x80, 6, 0x100, 0, 8});
        }
    }
    void poll() {
        const auto now = mx_clock_ns();
        if (active) {
            (void)packets.poll();
            if (packets.faulted()) {
                mx_exit(MX_ERR_FAULT); // kernel lifetime fence precedes reclaim
            }
            const auto value = read(kHostPort);
            const bool attached = (value & kPortConnected) && !(value & kPortOvercurrent);
            if (attached != connected) {
                connected = attached;
                event = {++sequence, 1, port_speed(value), static_cast<mx_u8_t>(attached), 0};
                event_pending = true;
                if (!attached && device) {
                    (void)remove(device);
                }
            }
            port(0, 0, value & kPortChanges);
            write(kInterruptStatus, kSof | (1u << 29));
        }
        poll_address(now);
        ledger.expire(now);
        for (mx_u32_t i = 0; i < kTransfers; ++i) {
            if (tokens[i]) {
                auto& row = transfer_rows[i];
                auto* ep = endpoint(row.prepared.request.endpoint);
                if (ledger.state(tokens[i]) == TransferState::Stopping && !row.stopping) {
                    stop(row, packets, MxUsbStatus::Cancelled);
                }
                if (!ep && !row.stopping) {
                    (void)ledger.stop(tokens[i], MxUsbStatus::Disconnected);
                    stop(row, packets, MxUsbStatus::Disconnected);
                }
                if (!active && !row.stopping) {
                    (void)ledger.stop(tokens[i], MxUsbStatus::Disconnected);
                    stop(row, packets, MxUsbStatus::Disconnected);
                }
                if (ep && advance(row, *ep, packets, now, active ? read(kHostFrame) : 0)) {
                    (void)ledger.finish(tokens[i], row.result, row.actual, row.raw);
                } else if (!ep && row.packet) {
                    if (packets.result(row.packet)) {
                        packets.release(row.packet);
                        row.packet = 0;
                        row.phase = TransferPhase::Done;
                    }
                }
                if (row.stopping && row.phase == TransferPhase::Done) {
                    (void)ledger.stopped(tokens[i]);
                }
                const auto* done = ledger.completion(tokens[i]);
                if (!done) {
                    continue;
                }
                if (addressing && row.prepared.request.request_id == reply.request_id) {
                    const bool ok =
                        done->status == MxUsbStatus::Success || done->status == MxUsbStatus::Short;
                    if (ep) {
                        ep->active = 0;
                    }
                    (void)ledger.retire(tokens[i]);
                    tokens[i] = 0;
                    if (!ok) {
                        addressing = false;
                        complete_reply(reply.request_id, MX_ERR_FAULT);
                    } else if (address_step == 2) {
                        const auto packet = payload.data[7];
                        if (packet != 8 && packet != 16 && packet != 32 && packet != 64) {
                            addressing = false;
                            complete_reply(reply.request_id, MX_ERR_BAD_PROTOCOL);
                        } else {
                            (void)update_ep0(packet);
                            address_step = 3;
                            internal_control({0, 5, 1, 0, 0});
                        }
                    } else {
                        address = 1;
                        if (ep) {
                            ep->device_address = 1;
                        }
                        address_step = 4;
                        address_deadline = now + 2000000;
                        // SET_ADDRESS takes effect after status completion and 2 ms.
                    }
                } else {
                    auto result = *done;
                    result.endpoint = row.prepared.request.endpoint;
                    if (!link.tx().try_push_msg(MX_USB_TRANSFER_COMPLETE, result)) {
                        continue;
                    }
                    if (ep) {
                        ep->active = 0;
                    }
                    (void)ledger.retire(tokens[i]);
                    tokens[i] = 0;
                }
            }
        }
        if (addressing && address_step == 4 && now >= address_deadline) {
            addressing = false;
            complete_reply(reply.request_id, MX_OK, ep0);
        }
        if (reply_pending && link.tx().try_push_msg(MX_USB_CONTROLLER_REPLY, reply)) {
            reply_pending = false;
        }
        if (event_pending && link.tx().try_push_msg(MX_USB_CONTROLLER_EVENT, event)) {
            event_pending = false;
        }
        MxRingSlot slot{};
        if (reply_pending || addressing || !link.try_recv(&slot)) {
            return;
        }
        if (slot.type == MX_USB_TRANSFER_SUBMIT) {
            MxUsbSubmit r{};
            if (message_get(slot, &r, slot.type)) {
                submit(r);
            }
        } else if (slot.type == MX_USB_TRANSFER_CONTROL) {
            MxUsbControlSubmit r{};
            if (message_get(slot, &r, slot.type)) {
                if (r.interface != device) {
                    complete_reply(r.request_id, MX_ERR_PERM);
                } else {
                    submit({r.request_id, ep0, r.buffer, r.offset, r.setup.length, r.deadline_ns,
                            r.flags, 0},
                           &r);
                }
            }
        } else if (slot.type == MX_USB_TRANSFER_CANCEL) {
            MxUsbCancel r{};
            if (message_get(slot, &r, slot.type)) {
                (void)ledger.cancel(scope, r.request_id);
            }
        } else if (slot.type == MX_USB_CONTROLLER_CONFIGURE_ENDPOINT) {
            MxUsbEndpointConfig r{};
            MxUsbToken token = 0;
            if (message_get(slot, &r, slot.type)) {
                mx_status_t rc = MX_ERR_INVALID;
                if (r.device == device && !r.max_burst && !r.mult && r.max_esit_payload<=r.max_packet &&
                    r.type != MxUsbTransferType::Isochronous) {
                    bool duplicate = false;
                    for (mx_u32_t i = 0; i < kEndpoints; ++i) {
                        if (endpoints.token_at(i) && endpoint_rows[i].address == r.address) {
                            duplicate = true;
                        }
                    }
                    if (!duplicate) {
                        rc = install(r.address, r.type, r.max_packet, r.interval, token);
                    }
                }
                complete_reply(r.request_id, rc, token);
            }
        } else {
            MxUsbControllerRequest r{};
            if (!message_get(slot, &r, slot.type)) {
                return;
            }
            if (r.reserved) {
                complete_reply(r.request_id, MX_ERR_INVALID);
            } else if (slot.type == MX_USB_CONTROLLER_ADDRESS) {
                address_request(r);
            } else if (slot.type == MX_USB_CONTROLLER_NEGOTIATE) {
                complete_reply(r.request_id, MX_OK);
                reply.value = 1;
            } else if (slot.type == MX_USB_CONTROLLER_PORT_STATUS) {
                complete_reply(r.request_id, r.value == 1 ? MX_OK : MX_ERR_INVALID);
                reply.value = active && connected;
            } else if (slot.type == MX_USB_CONTROLLER_EP0_PACKET) {
                complete_reply(r.request_id,
                               r.device == device ? update_ep0(r.value) : MX_ERR_INVALID);
            } else if (slot.type == MX_USB_CONTROLLER_REMOVE_DEVICE) {
                complete_reply(r.request_id, remove(r.device) ? MX_OK : MX_ERR_NOT_FOUND);
            } else if (slot.type == MX_USB_CONTROLLER_REVOKE_GRANT) {
                auto* ep = endpoint(r.device);
                if (ep && ep->active) {
                    (void)ledger.stop(ep->active, MxUsbStatus::Cancelled);
                    complete_reply(r.request_id, MX_ERR_BUSY);
                } else if (ep) {
                    ledger.revoke_endpoint(scope, ep->token);
                    endpoints.release(r.device);
                    complete_reply(r.request_id, MX_OK);
                } else {
                    complete_reply(r.request_id, MX_ERR_NOT_FOUND);
                }
            } else {
                complete_reply(r.request_id, MX_ERR_NOT_IMPLEMENTED);
            }
        }
    }
    mx_status_t update_ep0(mx_u32_t packet) {
        auto* ep = endpoint(ep0);
        if (!ep || ep->active || (packet != 8 && packet != 16 && packet != 32 && packet != 64)) {
            return MX_ERR_INVALID;
        }
        ledger.revoke_endpoint(scope, ep->token);
        midix::usb::EndpointDescriptor desc{};
        desc.type = MxUsbTransferType::Control;
        desc.max_packet = packet;
        const auto rc = ledger.install_endpoint(
            {scope, device, desc, speed, 0, ControlPolicy::Bus, device}, &ep->token);
        if (rc == MX_OK) {
            ep->max_packet = packet;
        }
        return rc;
    }
    bool quiesce() {
        if (device) {
            (void)remove(device);
        }
        addressing = false;
        for (mx_u32_t i = 0; i < kTransfers; ++i) {
            if (tokens[i]) {
                (void)ledger.stop(tokens[i], MxUsbStatus::Disconnected);
            }
        }
        bool stopped = true;
        for (mx_u32_t i = 0; i < kTransfers; ++i) {
            if (tokens[i] && ledger.state(tokens[i]) != TransferState::Terminal) {
                stopped = false;
            }
        }
        if (stopped) {
            if (active) {
                port(0, kPortPower);
            }
            active = false;
        }
        return stopped;
    }
};
} // namespace mochi::usb::dwc2
