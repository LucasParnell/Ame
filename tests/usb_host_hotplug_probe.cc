// Exercise production topology cleanup when the controller has already
// removed a disconnected device before io.usb receives its port event.
#define _start unused_usb_host_start
#include "../userland/usb/main.cc"
#undef _start
#include <cassert>
#include <cstdio>

struct Storage { MxRingPage page{}; MxRingSlot slots[8]{}; };
static Runtime runtime;
static Storage commands, replies;
static mx_status_t controller_status;
static MxUsbToken expected_device;
static bool wrong_reply, timeout_reply;

static mx_i64_t controller_reply() {
    MxRingSlot slot{};
    assert(g->controller.tx().try_pop(&slot));
    MxUsbControllerRequest request{};
    assert(mochi::message_get(slot, &request, MX_USB_CONTROLLER_REMOVE_DEVICE));
    assert(request.device == expected_device);
    if (timeout_reply) {
        host_now = host_wait_deadline;
    } else if (wrong_reply) {
        const MxUsbCompletion done{request.request_id, 0, MxUsbStatus::Disconnected, 0, 0, 0};
        assert(g->controller.rx().try_push_msg(MX_USB_TRANSFER_COMPLETE, done));
    } else {
        const MxUsbControllerReply reply{request.request_id, 0, 0, controller_status, 0,
                                         MxUsbSpeed::Full, {}};
        assert(g->controller.rx().try_push_msg(MX_USB_CONTROLLER_REPLY, reply));
    }
    return MX_OK;
}

int main() {
    g = &runtime;
    g->controller = {&commands.page, &replies.page, 3, 1, 1};
    assert(device_pool.init(g->device_slots, kDevices));
    assert(interface_pool.init(g->interface_slots, kInterfaces));
    assert(endpoint_pool.init(g->endpoint_slots, kEndpoints));
    g->watching = true;
    host_on_wait = controller_reply;

    // More cycles than the fixed device/interface tables can hold: every
    // disconnect must reclaim its rows and notify the keyboard driver once.
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
        const auto dev = device_pool.acquire();
        const auto iface = interface_pool.acquire();
        const auto ep = endpoint_pool.acquire();
        assert(dev && iface && ep);
        expected_device = 100 + cycle;
        auto& d = *device(dev);
        d.controller_device = expected_device;
        d.port = 1;
        d.interfaces[0].interface_class = 3;
        d.interfaces[0].subclass = d.interfaces[0].protocol = 1;
        g->interfaces[interface_pool.index_of(iface)] = {dev, 0};
        g->claimed[interface_pool.index_of(iface)] = 1;
        g->endpoints[endpoint_pool.index_of(ep)] = {iface, 7, 0x81};
        controller_status = cycle % 2 ? MX_ERR_NOT_FOUND : MX_OK;
        if (cycle == 0) {
            // A real failure or timeout must preserve live topology.
            controller_status = MX_ERR_FAULT;
            assert(!remove_device(dev) && device(dev) && interface(iface) && endpoint(ep));
            timeout_reply = true;
            assert(!remove_device(dev) && device(dev) && !g->event_count);
            timeout_reply = false;
            // An unrelated completion cannot reuse a stale NOT_FOUND reply.
            g->cold_reply.status = MX_ERR_NOT_FOUND;
            wrong_reply = true;
            assert(!remove_device(dev) && device(dev) && !g->event_count);
            wrong_reply = false;
            controller_status = MX_OK;
        }
        assert(remove_device(dev));
        assert(!device(dev) && !interface(iface) && !endpoint(ep));
        assert(!g->claimed[0]);
        assert(g->event_count == 1);
        const auto& event = g->events[g->event_head];
        assert(event.interface == iface && event.device == dev);
        assert(event.event == MxUsbDeviceEvent::Detached && event.interface_class == 3);
        g->event_count = 0; // Simulate consumption by the class driver.
        assert(!remove_device(dev) && !g->event_count);
    }
    host_on_wait = nullptr;
    std::puts("usb host hotplug probe ok");
}
