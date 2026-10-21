// io.usb: enumeration, descriptor validation, topology and interface leases.
// No controller registers or class/report knowledge. Fixed tables and shared
// CPU payload arenas; transfer rows remain owned until completion publication.
#include "mochi/usb/link.h"
#include "mochi/driver.h"
#include "mochi/arena.h"
#include "mochi/va_layout.h"
using namespace mochi::va::usb_host;
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "mochi/message.h"
#include "midix/abi/protocols/usb_host.h"
#include "midix/abi/protocols/usb_controller.h"
#include "midix/usb/descriptors.h"
#include "libk/endian.h"
#include "libk/slot_pool.h"

namespace {
constexpr uint32_t kDevices = 8, kInterfaces = 32, kEndpoints = 32, kRows = 15, kBytes = 4096;
struct DeviceCold {
    MxUsbToken controller_device, ep0;
    MxUsbSpeed speed;
    uint8_t port;
    uint16_t bytes;
    midix::usb::InterfaceDescriptor interfaces[16];
    midix::usb::EndpointDescriptor endpoints[32];
    uint32_t interface_count, endpoint_count;
    uint8_t descriptors[kBytes];
};
struct InterfaceCold {
    MxUsbToken device;
    uint32_t descriptor_index;
};
struct EndpointCold {
    MxUsbToken interface, controller_endpoint;
    uint8_t address;
};
struct TransferCold {
    uint64_t controller_id, client_id;
    MxUsbToken client_endpoint;
    uint32_t offset, length;
    bool input;
};
struct Runtime {
    DeviceCold devices[kDevices];
    InterfaceCold interfaces[kInterfaces];
    EndpointCold endpoints[kEndpoints];
    uint8_t claimed[kInterfaces];
    TransferCold rows[kRows];
    MxUsbCompletion completions[kRows];
    uint8_t done[kRows], payload_copied[kRows];
    libk::SlotMetadata device_slots[kDevices], interface_slots[kInterfaces],
        endpoint_slots[kEndpoints];
    MxUsbInterfaceOffer events[64];
    uint32_t event_head, event_count;
    uint8_t port_dirty[256], port_connected[256];
    uint64_t port_sequence[256], sequence, next_id;
    mochi::SpscEndpoint controller, client;
    libk::bytes controller_payload, client_payload;
    MxUsbControllerReply cold_reply;
    MxUsbCompletion cold_completion;
    uint64_t waiting;
    bool cold_done, cold_is_completion, watching;
    MxUsbHostReply reply;
    bool reply_pending;
};
Runtime* g;
libk::SlotPool device_pool, interface_pool, endpoint_pool;
DeviceCold* device(MxUsbToken token) {
    const auto i = device_pool.index_of(token);
    return i == libk::kNoSlot ? nullptr : &g->devices[i];
}
InterfaceCold* interface(MxUsbToken token) {
    const auto i = interface_pool.index_of(token);
    return i == libk::kNoSlot ? nullptr : &g->interfaces[i];
}
EndpointCold* endpoint(MxUsbToken token) {
    const auto i = endpoint_pool.index_of(token);
    return i == libk::kNoSlot ? nullptr : &g->endpoints[i];
}
bool range(libk::bytes bytes, uint32_t offset, uint32_t length) {
    return offset <= bytes.size && length <= bytes.size - offset;
}
void queue_offer(MxUsbToken token, MxUsbDeviceEvent event) {
    auto* iface = interface(token);
    auto* dev = iface ? device(iface->device) : nullptr;
    if (!dev || g->event_count == 64) {
        mochi::log_raw_marker("io.usb: topology backpressure\n");
        return;
    }
    const auto& desc = dev->interfaces[iface->descriptor_index];
    g->events[(g->event_head + g->event_count++) & 63] = {
        token,      iface->device,        ++g->sequence, dev->bytes,    desc.number, desc.alternate,
        dev->speed, desc.interface_class, desc.subclass, desc.protocol, event,       0};
}
void pump_controller() {
    MxRingSlot slot{};
    while (g->controller.try_recv(&slot)) {
        if (slot.type == MX_USB_CONTROLLER_EVENT) {
            MxUsbPortEvent event{};
            if (mochi::message_get(slot, &event, slot.type) && event.port && event.port < 256 &&
                event.sequence > g->port_sequence[event.port]) {
                g->port_sequence[event.port] = event.sequence;
                g->port_dirty[event.port] = 1;
                g->port_connected[event.port] = event.connected;
            }
        } else if (slot.type == MX_USB_CONTROLLER_REPLY) {
            MxUsbControllerReply reply{};
            if (!mochi::message_get(slot, &reply, slot.type)) {
                continue;
            }
            if (reply.request_id == g->waiting) {
                g->cold_reply = reply;
                g->cold_done = true;
                g->cold_is_completion = false;
            } else {
                for (uint32_t i = 0; i < kRows; ++i) {
                    if (g->rows[i].controller_id == reply.request_id) {
                        g->completions[i] = {g->rows[i].client_id,
                                             g->rows[i].client_endpoint,
                                             reply.status == MX_ERR_PERM ? MxUsbStatus::Denied
                                                                         : MxUsbStatus::Invalid,
                                             0,
                                             0,
                                             0};
                        g->done[i] = 1;
                    }
                }
            }
        } else if (slot.type == MX_USB_TRANSFER_COMPLETE) {
            MxUsbCompletion done{};
            if (!mochi::message_get(slot, &done, slot.type)) {
                continue;
            }
            if (done.request_id == g->waiting) {
                g->cold_completion = done;
                g->cold_done = true;
                g->cold_is_completion = true;
            } else {
                for (uint32_t i = 0; i < kRows; ++i) {
                    if (g->rows[i].controller_id == done.request_id) {
                        done.request_id = g->rows[i].client_id;
                        done.endpoint = g->rows[i].client_endpoint;
                        if (done.actual_length > g->rows[i].length) {
                            done.status = MxUsbStatus::IoError;
                            done.actual_length = 0;
                        }
                        g->completions[i] = done;
                        g->done[i] = 1;
                    }
                }
            }
        }
    }
}
bool wait_cold() {
    const auto deadline = mx_clock_ns() + 2000000000ull;
    while (!g->cold_done && mx_clock_ns() < deadline) {
        pump_controller(); // retain unrelated topology and transfer events
        if (!g->cold_done) {
            mochi::driver::idle(&g->controller, 1, g->controller.rx_doorbell, deadline);
        }
    }
    g->waiting = 0;
    return g->cold_done;
}
bool cold(uint32_t type, MxUsbControllerRequest request, MxUsbControllerReply* reply = nullptr) {
    request.request_id = (1ull << 63) | ++g->next_id;
    g->waiting = request.request_id;
    g->cold_done = false;
    if (!g->controller.tx().push_msg(type, request, 1024) || !wait_cold()) {
        return false;
    }
    if (reply && !g->cold_is_completion) {
        *reply = g->cold_reply;
    }
    return !g->cold_is_completion && g->cold_reply.status == MX_OK;
}
bool control(DeviceCold& dev, MxUsbSetup setup) {
    MxUsbControlSubmit request{(1ull << 63) | ++g->next_id,
                               dev.controller_device,
                               setup.length ? 1ull : 0ull,
                               mx_clock_ns() + 2000000000ull,
                               setup,
                               0,
                               0};
    g->waiting = request.request_id;
    g->cold_done = false;
    if (!g->controller.tx().push_msg(MX_USB_TRANSFER_CONTROL, request, 1024) || !wait_cold()) {
        return false;
    }
    return g->cold_is_completion &&
           (g->cold_completion.status == MxUsbStatus::Success ||
            g->cold_completion.status == MxUsbStatus::Short) &&
           g->cold_completion.actual_length == setup.length;
}
bool remove_device(MxUsbToken token) {
    auto* dev = device(token);
    if (!dev) {
        return false;
    }
    MxUsbControllerReply removed{};
    // The controller revokes its endpoints as soon as it sees disconnect,
    // before its port event reaches this process. Already absent is a valid
    // removal result: still release our topology and notify class drivers.
    // Timeouts and other failures must retain the rows for a later attempt.
    if (!cold(MX_USB_CONTROLLER_REMOVE_DEVICE, {0, dev->controller_device, 0, 0, 0, 0}, &removed) &&
        removed.status != MX_ERR_NOT_FOUND) {
        return false;
    }
    for (uint32_t i = 0; i < kInterfaces; ++i) {
        const auto handle = interface_pool.token_at(i);
        if (!handle || g->interfaces[i].device != token) {
            continue;
        }
        if (g->watching) {
            queue_offer(handle, MxUsbDeviceEvent::Detached);
        }
        for (uint32_t e = 0; e < kEndpoints; ++e) {
            const auto ep = endpoint_pool.token_at(e);
            if (ep && g->endpoints[e].interface == handle) {
                endpoint_pool.release(ep);
            }
        }
        g->claimed[i] = 0;
        interface_pool.release(handle);
    }
    device_pool.release(token);
    return true;
}
void enumeration_failed(uint8_t port, const char* stage) {
    char line[112];
    const int length = libk::snprintf(line, sizeof(line),
                                    "io.usb: port %u enumeration failed at %s\n",
                                    static_cast<unsigned>(port), stage);
    if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
        mochi::log_raw_marker(line);
    }
}
bool enumerate(uint8_t port) {
    const auto token = device_pool.acquire();
    if (!token) {
        enumeration_failed(port, "device table");
        return false;
    }
    auto& dev = *device(token);
    dev.port = port;
    MxUsbControllerReply reply{};
    if (!cold(MX_USB_CONTROLLER_ADDRESS, {0, 0, 0, 0, port, 0}, &reply)) {
        enumeration_failed(port, "address/reset");
        device_pool.release(token);
        return false;
    }
    dev.controller_device = reply.device;
    dev.ep0 = reply.endpoint;
    dev.speed = reply.speed;
    const char* stage = "device descriptor prefix";
    bool ok = control(dev, {0x80, 6, 0x100, 0, 8});
    uint16_t packet = 0;
    if (ok) {
        stage = "EP0 packet size";
        const auto raw = g->controller_payload.data;
        packet = dev.speed >= MxUsbSpeed::Super && raw[7] == 9 ? 512 : raw[7];
        ok = raw[0] == 18 && raw[1] == 1 &&
             (packet == 8 || packet == 16 || packet == 32 || packet == 64 || packet == 512) &&
             cold(MX_USB_CONTROLLER_EP0_PACKET, {0, dev.controller_device, 0, 0, packet, 0});
    }
    midix::usb::DeviceDescriptor descriptor{};
    if (ok) {
        stage = "device descriptor";
        ok = control(dev, {0x80, 6, 0x100, 0, 18}) &&
             midix::usb::parse_device(libk::as_bytes(g->controller_payload.data, 18), dev.speed,
                                      &descriptor);
    }
    if (ok) {
        stage = "configuration header";
        ok = control(dev, {0x80, 6, 0x200, 0, 9});
    }
    const uint16_t bytes = ok ? libk::load_le<uint16_t>(g->controller_payload.data + 2) : 0;
    if (ok) {
        stage = "configuration descriptor";
        ok = bytes >= 9 && bytes <= kBytes && control(dev, {0x80, 6, 0x200, 0, bytes});
    }
    midix::usb::Configuration config{dev.interfaces, dev.endpoints, 16, 32, 0, 0, 0, 0, 0, 0};
    if (ok) {
        stage = "configuration parse";
        libk::copy_n(libk::as_bytes(dev.descriptors),
                     libk::as_bytes(g->controller_payload.data, bytes), bytes);
        ok = midix::usb::parse_configuration(libk::as_bytes(dev.descriptors, bytes), dev.speed,
                                             &config);
    }
    if (ok) {
        stage = "SET_CONFIGURATION";
        ok = control(dev, {0, 9, config.value, 0, 0});
    }
    if (!ok) {
        enumeration_failed(port, stage);
        (void)remove_device(token);
        return false;
    }
    dev.bytes = bytes;
    dev.interface_count = config.interface_count;
    dev.endpoint_count = config.endpoint_count;
    for (uint32_t i = 0; i < config.interface_count; ++i) {
        if (dev.interfaces[i].alternate) {
            continue;
        }
        const auto iface = interface_pool.acquire();
        if (!iface) {
            enumeration_failed(port, "interface table");
            (void)remove_device(token);
            return false;
        }
        const auto index = interface_pool.index_of(iface);
        g->interfaces[index] = {token, i};
        g->claimed[index] = 0;
        if (g->watching) {
            queue_offer(iface, MxUsbDeviceEvent::Attached);
        }
    }
    return true;
}
void reply(uint64_t id, mx_status_t status, MxUsbToken token = 0, uint32_t length = 0) {
    g->reply = {id, token, status, length};
    g->reply_pending = true;
}
void submit(const MxUsbSubmit& request, const MxUsbControlSubmit* control_request) {
    auto* ep = control_request ? nullptr : endpoint(request.endpoint);
    const auto iface_token = control_request ? control_request->interface : ep ? ep->interface : 0;
    auto* iface = interface(iface_token);
    const auto ii = interface_pool.index_of(iface_token);
    auto* dev = iface ? device(iface->device) : nullptr;
    const auto length = control_request ? control_request->setup.length : request.length;
    const auto offset = control_request ? control_request->offset : request.offset;
    if (!dev || !g->claimed[ii] || !request.request_id || (length && request.buffer != 1) ||
        !range(g->client_payload, offset, length) || length > kBytes) {
        reply(request.request_id, MX_ERR_PERM);
        return;
    }
    if (control_request) {
        const auto& setup = control_request->setup;
        const auto& desc = dev->interfaces[iface->descriptor_index];
        if (!mx_usb_interface_control_allowed(setup, desc.number)) {
            reply(request.request_id, MX_ERR_PERM);
            return;
        }
    }
    uint32_t row = kRows;
    for (uint32_t i = 0; i < kRows; ++i) {
        const auto& r = g->rows[i];
        if (r.controller_id && r.client_id == request.request_id) {
            reply(request.request_id, MX_ERR_BUSY);
            return;
        }
        if (!r.controller_id && row == kRows) {
            row = i;
        }
    }
    if (row == kRows) {
        reply(request.request_id, MX_ERR_WOULD_BLOCK);
        return;
    }
    for (uint32_t i = 0; i < kRows; ++i) {
        const auto& r = g->rows[i];
        if (r.controller_id && offset < r.offset + r.length && r.offset < offset + length) {
            reply(request.request_id, MX_ERR_BUSY);
            return;
        }
    }
    const bool input = control_request ? (control_request->setup.request_type & 128) != 0
                                       : (ep->address & 128) != 0;
    const uint32_t shared_offset = (row + 1) * kBytes;
    if (!input && length) {
        libk::copy_n(libk::as_bytes(g->controller_payload.data + shared_offset, kBytes),
                     libk::as_bytes(g->client_payload.data + offset, length), length);
    }
    const auto id = ++g->next_id;
    bool sent;
    if (control_request) {
        auto wire = *control_request;
        wire.request_id = id;
        wire.interface = dev->controller_device;
        wire.offset = shared_offset;
        sent = g->controller.tx().try_push_msg(MX_USB_TRANSFER_CONTROL, wire);
    } else {
        auto wire = request;
        wire.request_id = id;
        wire.endpoint = ep->controller_endpoint;
        wire.offset = shared_offset;
        sent = g->controller.tx().try_push_msg(MX_USB_TRANSFER_SUBMIT, wire);
    }
    if (!sent) {
        reply(request.request_id, MX_ERR_WOULD_BLOCK);
        return;
    }
    g->rows[row] = {
        id,   request.request_id, control_request ? iface_token : request.endpoint, offset, length,
        input};
    g->done[row] = 0;
    g->payload_copied[row] = 0;
}
void handle(const MxRingSlot& slot) {
    if (slot.type == MX_USB_TRANSFER_CANCEL) {
        MxUsbCancel request{};
        if (mochi::message_get(slot, &request, slot.type)) {
            for (auto& row : g->rows) {
                if (row.controller_id && row.client_id == request.request_id) {
                    (void)g->controller.tx().try_push_msg(MX_USB_TRANSFER_CANCEL,
                                                          MxUsbCancel{row.controller_id});
                }
            }
        }
        return;
    }
    if (slot.type == MX_USB_HOST_SUBMIT || slot.type == MX_USB_HOST_CONTROL) {
        if (slot.type == MX_USB_HOST_SUBMIT) {
            MxUsbSubmit request{};
            if (mochi::message_get(slot, &request, slot.type)) {
                submit(request, nullptr);
            }
        } else {
            MxUsbControlSubmit request{};
            if (mochi::message_get(slot, &request, slot.type)) {
                MxUsbSubmit wire{request.request_id,   0,
                                 request.buffer,       request.offset,
                                 request.setup.length, request.deadline_ns,
                                 request.flags,        0};
                submit(wire, &request);
            }
        }
        return;
    }
    MxUsbHostRequest request{};
    if (!mochi::message_get(slot, &request, slot.type)) {
        return;
    }
    auto* iface = interface(request.interface);
    const auto ii = interface_pool.index_of(request.interface);
    auto* dev = iface ? device(iface->device) : nullptr;
    if (request.reserved) {
        reply(request.request_id, MX_ERR_INVALID);
        return;
    }
    if (slot.type == MX_USB_HOST_WATCH) {
        // A new client incarnation first retires the previous leases. Old
        // completions retain their IDs and payload slots until publication.
        for (uint32_t e = 0; e < kEndpoints; ++e) {
            const auto token = endpoint_pool.token_at(e);
            if (token && !cold(MX_USB_CONTROLLER_REVOKE_GRANT,
                               {0, g->endpoints[e].controller_endpoint, 0, 0, 0, 0})) {
                reply(request.request_id, MX_ERR_FAULT);
                return;
            }
            if (token) {
                endpoint_pool.release(token);
            }
        }
        for (auto& claim : g->claimed) {
            claim = 0;
        }
        g->event_head = g->event_count = 0;
        g->watching = true;
        for (uint32_t i = 0; i < kInterfaces; ++i) {
            if (const auto token = interface_pool.token_at(i)) {
                queue_offer(token, MxUsbDeviceEvent::Attached);
            }
        }
        reply(request.request_id, MX_OK);
        return;
    }
    if (slot.type == MX_USB_HOST_CLOSE_ENDPOINT) {
        auto* ep = endpoint(request.interface);
        if (!ep ||
            !cold(MX_USB_CONTROLLER_REVOKE_GRANT, {0, ep->controller_endpoint, 0, 0, 0, 0})) {
            reply(request.request_id, MX_ERR_FAULT);
            return;
        }
        endpoint_pool.release(request.interface);
        reply(request.request_id, MX_OK);
        return;
    }
    if (!dev) {
        reply(request.request_id, MX_ERR_NOT_FOUND);
        return;
    }
    if (slot.type == MX_USB_HOST_CLAIM) {
        if (g->claimed[ii]) {
            reply(request.request_id, MX_ERR_BUSY);
        } else {
            g->claimed[ii] = 1;
            reply(request.request_id, MX_OK, request.interface);
        }
    } else if (slot.type == MX_USB_HOST_RELEASE) {
        for (uint32_t e = 0; e < kEndpoints; ++e) {
            const auto token = endpoint_pool.token_at(e);
            if (!token || g->endpoints[e].interface != request.interface) {
                continue;
            }
            if (!cold(MX_USB_CONTROLLER_REVOKE_GRANT,
                      {0, g->endpoints[e].controller_endpoint, 0, 0, 0, 0})) {
                reply(request.request_id, MX_ERR_FAULT);
                return;
            }
            endpoint_pool.release(token);
        }
        g->claimed[ii] = 0;
        reply(request.request_id, MX_OK);
    } else if (slot.type == MX_USB_HOST_DESCRIPTORS) {
        if (!g->claimed[ii] || request.length < dev->bytes ||
            !range(g->client_payload, request.offset, dev->bytes)) {
            reply(request.request_id, MX_ERR_PERM);
        } else {
            libk::copy_n(libk::as_bytes(g->client_payload.data + request.offset, request.length),
                         libk::as_bytes(dev->descriptors, dev->bytes), dev->bytes);
            reply(request.request_id, MX_OK, request.interface, dev->bytes);
        }
    } else if (slot.type == MX_USB_HOST_OPEN_ENDPOINT) {
        if (!g->claimed[ii]) {
            reply(request.request_id, MX_ERR_PERM);
            return;
        }
        const midix::usb::EndpointDescriptor* desc = nullptr;
        for (uint32_t i = 0; i < dev->endpoint_count; ++i) {
            if (dev->endpoints[i].interface_index == iface->descriptor_index &&
                dev->endpoints[i].address == request.value) {
                desc = &dev->endpoints[i];
            }
        }
        if (!desc) {
            reply(request.request_id, MX_ERR_NOT_FOUND);
            return;
        }
        const auto token = endpoint_pool.acquire();
        if (!token) {
            reply(request.request_id, MX_ERR_WOULD_BLOCK);
            return;
        }
        MxUsbEndpointConfig config{};
        config.request_id = (1ull << 63) | ++g->next_id;
        config.device = dev->controller_device;
        config.max_packet = desc->max_packet;
        config.max_esit_payload = desc->bytes_per_interval;
        config.address = desc->address;
        config.type = desc->type;
        config.interval = desc->interval;
        config.max_burst = desc->max_burst;
        config.mult = (desc->companion_attributes & 3u);
        g->waiting = config.request_id;
        g->cold_done = false;
        if (!g->controller.tx().push_msg(MX_USB_CONTROLLER_CONFIGURE_ENDPOINT, config, 1024) ||
            !wait_cold() || g->cold_reply.status != MX_OK) {
            endpoint_pool.release(token);
            reply(request.request_id, MX_ERR_FAULT);
            return;
        }
        g->endpoints[endpoint_pool.index_of(token)] = {request.interface, g->cold_reply.endpoint,
                                                       desc->address};
        reply(request.request_id, MX_OK, token);
    } else {
        reply(request.request_id, MX_ERR_NOT_IMPLEMENTED);
    }
}
bool service() {
    pump_controller();
    bool progress = false;
    if (g->reply_pending) {
        if (!g->client.tx().try_push_msg(MX_USB_HOST_REPLY, g->reply)) {
            return false;
        }
        g->reply_pending = false;
        progress = true;
    }
    while (g->event_count && g->watching) {
        const auto& event = g->events[g->event_head];
        if (!g->client.tx().try_push_msg(MX_USB_HOST_EVENT, event)) {
            break;
        }
        g->event_head = (g->event_head + 1) & 63;
        --g->event_count;
        progress = true;
    }
    for (uint32_t i = 0; i < kRows; ++i) {
        if (g->done[i]) {
            const auto& row = g->rows[i];
            const auto& completion = g->completions[i];
            if (row.input && completion.actual_length && !g->payload_copied[i]) {
                libk::copy_n(libk::as_bytes(g->client_payload.data + row.offset, row.length),
                             libk::as_bytes(g->controller_payload.data + (i + 1) * kBytes, kBytes),
                             completion.actual_length);
            }
            g->payload_copied[i] = 1;
            if (!g->client.tx().try_push_msg(MX_USB_HOST_COMPLETE, completion)) {
                continue;
            }
            g->rows[i] = {};
            g->done[i] = 0;
            progress = true;
        }
    }
    MxRingSlot slot{};
    for (uint32_t n = 0;
         n < mochi::driver::kBatchLimit && !g->reply_pending && g->client.try_recv(&slot); ++n) {
        handle(slot);
        progress = true;
    }
    for (uint32_t port = 1; port < 256; ++port) {
        if (g->port_dirty[port] && g->event_count <= 32) {
            g->port_dirty[port] = 0;
            for (uint32_t i = 0; i < kDevices; ++i) {
                const auto token = device_pool.token_at(i);
                if (token && g->devices[i].port == port) {
                    (void)remove_device(token);
                }
            }
            if (g->port_connected[port]) {
                (void)enumerate(static_cast<uint8_t>(port));
            }
            progress = true;
        }
    }
    return progress;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "io.usb");
    auto arena = mochi::usb::make_state(kVaState, 128 * 1024);
    g = arena.alloc<Runtime>(1);
    if (!g) {
        mochi::boot::invalid_boot();
    }
    g->controller = mochi::usb::map_link(boot, true, kVaControllerTx, kVaControllerRx);
    g->client = mochi::usb::map_link(boot, false, kVaClassTx, kVaClassRx);
    g->controller_payload =
        mochi::usb::map_payload(boot, "usb.controller.payload", kVaControllerPayload);
    g->client_payload = mochi::usb::map_payload(boot, "usb.host.payload", kVaClassPayload);
    if (!g->controller.valid() || !g->client.valid() || g->controller_payload.size < 16 * kBytes ||
        g->client_payload.size < 8192 || !device_pool.init(g->device_slots, kDevices) ||
        !interface_pool.init(g->interface_slots, kInterfaces) ||
        !endpoint_pool.init(g->endpoint_slots, kEndpoints)) {
        mochi::boot::invalid_boot();
    }
    g->next_id = mx_clock_ns() & ~(1ull << 63);
    MxUsbControllerReply reply{};
    if (!cold(MX_USB_CONTROLLER_NEGOTIATE, {}, &reply)) {
        mochi::boot::invalid_boot();
    }
    for (uint32_t port = 1; port <= reply.value && port < 256; ++port) {
        MxUsbControllerReply status{};
        if (cold(MX_USB_CONTROLLER_PORT_STATUS, {0, 0, 0, 0, port, 0}, &status) && status.value) {
            (void)enumerate(static_cast<uint8_t>(port));
        }
    }
    mochi::log_raw_marker("io.usb: ready\n");
    mochi::boot::ready(boot);
    const mochi::SpscEndpoint readers[] = {g->controller, g->client};
    for (;;) {
        if (mochi::driver::run_batch(service)) {
            mx_yield();
            continue;
        }
        bool retry = g->reply_pending || (g->watching && g->event_count);
        for (uint32_t i = 0; i < kRows; ++i) {
            retry |= g->done[i] != 0;
        }
        for (uint32_t i = 1; i < 256; ++i) {
            retry |= g->port_dirty[i] != 0;
        }
        mochi::driver::idle(readers, 2, g->controller.rx_doorbell,
                            retry ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs : 0,
                            g->reply_pending ? 1u : 3u);
    }
}
