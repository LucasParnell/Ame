// HID keyboard class driver. Owns class matching, SET_PROTOCOL/SET_IDLE,
// interrupt report requests, decoding and disconnect releases. No MMIO/DMA cap.
#include "mochi/usb/link.h"
#include "mochi/driver.h"
#include "mochi/arena.h"
#include "mochi/va_layout.h"
using namespace mochi::va::hid;
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "mochi/message.h"
#include "mochi/input.h"
#include "midix/abi/protocols/usb_host.h"
#include "midix/usb/descriptors.h"
#include "midix/usb/hid_boot.h"
#include "midix/usb/hid_report.h"
namespace {
constexpr uint32_t kKeyboards = 8;
constexpr uint32_t kReportStride = 4096;
constexpr uint32_t kDescriptorOffset = kKeyboards * kReportStride;
constexpr uint32_t kLedOffset = kDescriptorOffset + kReportStride;
enum State : uint8_t { Free, Offered, Opening, Ready, Closing };
struct Keyboard {
    MxUsbInterfaceOffer offer;
    MxUsbToken endpoint;
    uint64_t request;
    uint64_t sequence;
    uint32_t report_bytes;
    bool report_protocol;
};
struct Runtime {
    // Hot identity/in-flight rows are contiguous; compiled descriptor tables
    // and decoder storage never stride through the scheduler's state scan.
    Keyboard keyboards[kKeyboards];
    State states[kKeyboards];
    uint8_t completed[kKeyboards], release_pending[kKeyboards];
    uint8_t attach_pending[kKeyboards], led_dirty[kKeyboards], led_mask[kKeyboards];
    MxUsbCompletion completions[kKeyboards];
    mochi::SpscEndpoint host;
    mochi::input::EventSink input;
    mochi::input::LedSource leds;
    libk::bytes payload;
    uint64_t next_id, waiting, generation;
    MxUsbHostReply reply;
    MxUsbCompletion control;
    bool replied, is_completion;
    struct Decoders {
        midix::usb::BootKeyboardDecoder boot;
        midix::usb::hid::KeyboardDecoder report;
    } decoders[kKeyboards];
    midix::usb::hid::Layout layouts[kKeyboards];
};
Runtime* g;
void close(uint32_t i) {
    if (g->states[i] != Closing && g->keyboards[i].endpoint) {
        mochi::log_raw_marker("drv.usb.hid: keyboard detached\n");
    }
    g->states[i] = Closing;
    g->release_pending[i] = 1;
}
void receive(const MxRingSlot& message) {
    if (message.type == MX_USB_HOST_EVENT) {
        MxUsbInterfaceOffer offer{};
        if (!mochi::message_get(message, &offer, message.type)) {
            return;
        }
        if (offer.event == MxUsbDeviceEvent::Detached) {
            for (uint32_t i = 0; i < kKeyboards; ++i) {
                if (g->states[i] != Free && g->keyboards[i].offer.interface == offer.interface) {
                    close(i);
                }
            }
            return;
        }
        // Matching policy belongs here. Other interfaces remain available to
        // other class drivers through the bus's generic interface contract.
        if (offer.event != MxUsbDeviceEvent::Attached || offer.interface_class != 3) {
            return;
        }
        for (uint32_t i = 0; i < kKeyboards; ++i) {
            if (g->states[i] != Free && g->keyboards[i].offer.interface == offer.interface) {
                return;
            }
        }
        for (uint32_t i = 0; i < kKeyboards; ++i) {
            if (g->states[i] == Free) {
                // A compiled layout is cold arena storage. Aggregate assignment
                // would create a large temporary on the service's 4 KiB stack.
                libk::zero(g->keyboards[i]);
                libk::zero(g->decoders[i]);
                libk::zero(g->layouts[i]);
                g->keyboards[i].offer = offer;
                g->states[i] = Offered;
                return;
            }
        }
    } else if (message.type == MX_USB_HOST_REPLY) {
        MxUsbHostReply reply{};
        if (!mochi::message_get(message, &reply, message.type)) {
            return;
        }
        if (reply.request_id == g->waiting) {
            g->reply = reply;
            g->replied = true;
            g->is_completion = false;
        } else {
            for (uint32_t i = 0; i < kKeyboards; ++i) {
                if (g->keyboards[i].request == reply.request_id) {
                    g->completions[i] = {
                        reply.request_id, g->keyboards[i].endpoint, MxUsbStatus::IoError, 0, 0, 0};
                    g->completed[i] = 1;
                }
            }
        }
    } else if (message.type == MX_USB_HOST_COMPLETE) {
        MxUsbCompletion completion{};
        if (!mochi::message_get(message, &completion, message.type)) {
            return;
        }
        if (completion.request_id == g->waiting) {
            g->control = completion;
            g->replied = true;
            g->is_completion = true;
        } else {
            for (uint32_t i = 0; i < kKeyboards; ++i) {
                if (g->keyboards[i].request == completion.request_id) {
                    g->completions[i] = completion;
                    g->completed[i] = 1;
                }
            }
        }
    }
}
void pump() {
    MxRingSlot message{};
    while (g->host.try_recv(&message)) {
        receive(message);
    }
}
bool wait() {
    const auto deadline = mx_clock_ns() + 2000000000ull;
    while (!g->replied && mx_clock_ns() < deadline) {
        pump(); // preserve other keyboards' completions while this call waits
        if (!g->replied) {
            mochi::driver::idle(&g->host, 1, g->host.rx_doorbell, deadline);
        }
    }
    g->waiting = 0;
    return g->replied;
}
bool call(uint32_t type, MxUsbHostRequest request) {
    request.request_id = ++g->next_id;
    g->waiting = request.request_id;
    g->replied = false;
    return g->host.tx().push_msg(type, request, 1024) && wait() && !g->is_completion &&
           g->reply.status == MX_OK;
}
bool class_control(const MxUsbInterfaceOffer& offer, uint8_t request, uint16_t value) {
    MxUsbControlSubmit control{++g->next_id,
                               offer.interface,
                               0,
                               mx_clock_ns() + 2000000000ull,
                               {0x21, request, value, offer.number, 0},
                               0,
                               0};
    g->waiting = control.request_id;
    g->replied = false;
    return g->host.tx().push_msg(MX_USB_HOST_CONTROL, control, 1024) && wait() &&
           g->is_completion && g->control.status == MxUsbStatus::Success;
}
bool payload_control(const MxUsbInterfaceOffer& offer, MxUsbSetup setup, uint32_t offset) {
    setup.index = offer.number;
    MxUsbControlSubmit request{++g->next_id,
                               offer.interface,
                               1,
                               mx_clock_ns() + 2000000000ull,
                               setup,
                               offset,
                               MX_USB_TRANSFER_ALLOW_SHORT};
    g->waiting = request.request_id;
    g->replied = false;
    return g->host.tx().push_msg(MX_USB_HOST_CONTROL, request, 1024) && wait() &&
           g->is_completion &&
           (g->control.status == MxUsbStatus::Success || g->control.status == MxUsbStatus::Short);
}
bool open(uint32_t i) {
    auto& keyboard = g->keyboards[i];
    const auto offer = keyboard.offer;
    g->states[i] = Opening;
    if (!call(MX_USB_HOST_CLAIM, {0, offer.interface, 0, 0, 0, 0}) ||
        !call(MX_USB_HOST_DESCRIPTORS,
              {0, offer.interface, kDescriptorOffset, kReportStride, 0, 0})) {
        return false;
    }
    midix::usb::InterfaceDescriptor interfaces[16]{};
    midix::usb::EndpointDescriptor endpoints[32]{};
    midix::usb::Configuration config{interfaces, endpoints, 16, 32, 0, 0, 0, 0, 0, 0};
    if (!midix::usb::parse_configuration(
            libk::slice(g->payload, kDescriptorOffset, g->reply.length), offer.speed, &config)) {
        return false;
    }
    uint8_t address = 0;
    uint16_t descriptor_bytes = 0;
    for (uint32_t n = 0; n < config.interface_count; ++n) {
        const auto& iface = interfaces[n];
        if (iface.number != offer.number || iface.alternate != offer.alternate) {
            continue;
        }
        descriptor_bytes = iface.hid_report_bytes;
        for (uint32_t e = 0; e < config.endpoint_count; ++e) {
            const auto& ep = endpoints[e];
            if (ep.interface_index == n && ep.type == MxUsbTransferType::Interrupt &&
                (ep.address & 128)) {
                address = ep.address;
                break;
            }
        }
    }
    if (!address || g->states[i] == Closing) {
        return false;
    }
    const bool boot_keyboard = offer.subclass == 1 && offer.protocol == 1;
    if (descriptor_bytes && descriptor_bytes <= kReportStride &&
        payload_control(offer, {0x81, 6, 0x2200, offer.number, descriptor_bytes},
                        kDescriptorOffset) &&
        g->control.actual_length == descriptor_bytes &&
        midix::usb::hid::compile(libk::slice(g->payload, kDescriptorOffset, descriptor_bytes),
                                 &g->layouts[i])) {
        // SET_PROTOCOL is defined only for the boot subclass. Other keyboards
        // already speak report protocol and must not receive that request.
        if ((!boot_keyboard || class_control(offer, 11, 1)) &&
            g->decoders[i].report.init(&g->layouts[i])) {
            keyboard.report_protocol = true;
            for (uint32_t r = 0; r < g->layouts[i].report_count; ++r) {
                const auto bytes = midix::usb::hid::report_bytes(g->layouts[i].reports[r],
                                                                 midix::usb::hid::Kind::Input);
                if (bytes > keyboard.report_bytes) {
                    keyboard.report_bytes = bytes;
                }
            }
        }
    }
    if (!keyboard.report_protocol) {
        if (!boot_keyboard || !class_control(offer, 11, 0)) {
            return false;
        }
        keyboard.report_bytes = 8;
    }
    (void)class_control(offer, 10, 0); // optional idle suppression
    if (!call(MX_USB_HOST_OPEN_ENDPOINT, {0, offer.interface, 0, 0, address, 0})) {
        return false;
    }
    if (g->states[i] == Closing) {
        return false;
    }
    keyboard.endpoint = g->reply.token;
    g->states[i] = Ready;
    g->attach_pending[i] = g->led_dirty[i] = 1;
    mochi::log_raw_marker(keyboard.report_protocol ? "drv.usb.hid: report protocol\n"
                                                   : "drv.usb.hid: boot protocol\n");
    mochi::log_raw_marker("drv.usb.hid: keyboard ready\n");
    return true;
}
bool drain(uint32_t i) {
    auto& keyboard = g->keyboards[i];
    while (const auto* raw = keyboard.report_protocol ? g->decoders[i].report.event()
                                                      : g->decoders[i].boot.event()) {
        auto event = *raw;
        event.source = keyboard.offer.interface;
        event.generation = g->generation;
        event.sequence = keyboard.sequence + 1;
        if (!g->input.try_send(event)) {
            return false;
        }
        ++keyboard.sequence;
#ifdef MOCHI_USB_TRACE_INPUT
        mochi::log_counter(event.type == INPUT_KEY_DOWN ? "usb.hid.key_down" : "usb.hid.key_up",
                           event.keycode);
#endif
        if (keyboard.report_protocol) {
            (void)g->decoders[i].report.advance();
        } else {
            (void)g->decoders[i].boot.advance();
        }
    }
    return true;
}
bool source_event(uint32_t i, uint32_t type) {
    auto& keyboard = g->keyboards[i];
    const InputEvent event{
        type, 0, 0, 0, keyboard.offer.interface, g->generation, keyboard.sequence + 1, 7, 0};
    if (!g->input.try_send(event)) {
        return false;
    }
    ++keyboard.sequence;
    return true;
}
void update_leds(uint32_t i) {
    auto& keyboard = g->keyboards[i];
    auto output = libk::slice(g->payload, kLedOffset, kReportStride);
    if (keyboard.report_protocol) {
        for (uint32_t r = 0; r < g->layouts[i].report_count; ++r) {
            uint32_t bytes = 0;
            if (!midix::usb::hid::encode_leds(g->layouts[i], r, g->led_mask[i], output, &bytes)) {
                continue;
            }
            const auto value = static_cast<uint16_t>(0x200 | g->layouts[i].reports[r].id);
            if (g->states[i] != Ready) {
                break;
            }
            (void)payload_control(
                keyboard.offer,
                {0x21, 9, value, keyboard.offer.number, static_cast<uint16_t>(bytes)}, kLedOffset);
        }
    } else {
        output.data[0] = g->led_mask[i];
        (void)payload_control(keyboard.offer, {0x21, 9, 0x200, keyboard.offer.number, 1},
                              kLedOffset);
    }
    // A stalled/unsupported LED report does not disable otherwise valid input.
    // A later policy change is a fresh request through the same USB contract.
    g->led_dirty[i] = 0;
}
bool service() {
    pump();
    bool progress = false;
    InputLedCommand command{};
    while (g->leds.try_read(&command)) {
        if (command.reserved || command.mask > 31) {
            continue;
        }
        for (uint32_t i = 0; i < kKeyboards; ++i) {
            if (command.source && g->keyboards[i].offer.interface != command.source) {
                continue;
            }
            g->led_mask[i] = static_cast<uint8_t>(command.mask);
            g->led_dirty[i] = 1;
        }
        progress = true;
    }
    for (uint32_t i = 0; i < kKeyboards; ++i) {
        if (g->states[i] == Free) {
            continue;
        }
        auto& keyboard = g->keyboards[i];
        if (g->attach_pending[i]) {
            if (!source_event(i, INPUT_SOURCE_ATTACHED)) {
                continue;
            }
            g->attach_pending[i] = 0;
        }
        if (!drain(i)) {
            continue;
        }
        if (g->states[i] == Offered) {
            if (!open(i)) {
                (void)call(MX_USB_HOST_RELEASE, {0, keyboard.offer.interface, 0, 0, 0, 0});
                close(i);
            }
            progress = true;
        }
        if (g->completed[i]) {
            const auto& done = g->completions[i];
            keyboard.request = 0;
            g->completed[i] = 0;
            if (g->states[i] == Ready &&
                (done.status == MxUsbStatus::Success || done.status == MxUsbStatus::Short)) {
                const auto report = libk::slice(g->payload, i * kReportStride, done.actual_length);
                if (keyboard.report_protocol) {
                    (void)g->decoders[i].report.prepare(report);
                } else if (done.actual_length == 8) {
                    (void)g->decoders[i].boot.prepare(report);
                }
            } else {
                close(i);
            }
            progress = true;
        }
        if (!drain(i)) {
            continue;
        }
        if (g->states[i] == Closing && g->release_pending[i]) {
            if (keyboard.report_protocol) {
                (void)g->decoders[i].report.release_all();
            } else {
                const uint8_t empty[8]{};
                (void)g->decoders[i].boot.prepare(libk::as_bytes(empty));
            }
            g->release_pending[i] = 0;
            if (!drain(i)) {
                continue;
            }
        }
        if (g->states[i] == Closing && !keyboard.request) {
            if (!source_event(i, INPUT_SOURCE_DETACHED)) {
                continue;
            }
            g->states[i] = Free;
            libk::zero(keyboard);
            progress = true;
            continue;
        }
        if (g->states[i] == Ready && g->led_dirty[i]) {
            update_leds(i);
            progress = true;
        }
        if (g->states[i] == Ready && !keyboard.request) {
            MxUsbSubmit request{++g->next_id,
                                keyboard.endpoint,
                                1,
                                i * kReportStride,
                                keyboard.report_bytes,
                                0,
                                MX_USB_TRANSFER_ALLOW_SHORT,
                                0};
            if (g->host.tx().try_push_msg(MX_USB_HOST_SUBMIT, request)) {
                keyboard.request = request.request_id;
                progress = true;
            }
        }
    }
    return progress;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "drv.usb.hid");
    auto arena = mochi::usb::make_state(kVaState, 128 * 1024);
    g = arena.alloc<Runtime>(1);
    if (!g) {
        mochi::boot::invalid_boot();
    }
    g->host = mochi::usb::map_link(boot, false, kVaHostTx, kVaHostRx);
    g->payload = mochi::usb::map_payload(boot, "usb.host.payload", kVaPayload);
    g->input = {
        mochi::boot::map_ring(boot, {"hid.input.ring", kVaInputRing, "hid.input.doorbell"})};
    g->leds = {mochi::boot::map_ring(boot, {"hid.led.ring", kVaLedRing, "hid.led.doorbell"})};
    g->next_id = mx_clock_ns();
    g->generation = g->next_id ? g->next_id : 1;
    if (!g->host.valid() || g->payload.size < kLedOffset + kReportStride || !g->input.valid() ||
        !g->leds.valid() || !call(MX_USB_HOST_WATCH, {})) {
        mochi::boot::invalid_boot();
    }
    mochi::log_raw_marker("drv.usb.hid: ready\n");
    mochi::boot::ready(boot);
    mochi::SpscEndpoint readers[2]{};
    readers[0] = g->host;
    readers[1].rx_ring = g->leds.ring;
    readers[1].order = g->leds.order;
    for (;;) {
        if (mochi::driver::run_batch(service)) {
            mx_yield();
            continue;
        }
        // The event sink currently has no TX-space notification. Retry only
        // while retained decoder/lifecycle work or a submission is blocked.
        bool retry = false;
        for (uint32_t i = 0; i < kKeyboards; ++i) {
            if (g->states[i] == Free) {
                continue;
            }
            const auto& keyboard = g->keyboards[i];
            retry |= g->attach_pending[i] || g->release_pending[i] ||
                     (g->states[i] == Ready && !keyboard.request) ||
                     (keyboard.report_protocol ? g->decoders[i].report.event() != nullptr
                                               : g->decoders[i].boot.event() != nullptr) ||
                     (g->states[i] == Closing && !keyboard.request);
        }
        mochi::driver::idle(readers, 2, g->host.rx_doorbell,
                            retry ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs : 0);
    }
}
