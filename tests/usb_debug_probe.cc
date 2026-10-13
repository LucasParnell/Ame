#include "midix/abi/types.h"
#include "midix/debug/usb_input.h"
#include "../userland/usb/gadget/descriptors.h"
#include "midix/usb/descriptors.h"
#include <cassert>
#include <cstdio>
using namespace midix::debug;
int main() {
    Parser parser{};
    mx_u8_t frame[20]{};
    libk::write_le<mx_u32_t>(libk::as_bytes(frame, 4), kInputMagic);
    libk::write_le<mx_u32_t>(libk::as_bytes(frame + 4, 4), 7);
    libk::write_le<mx_u16_t>(libk::as_bytes(frame + 8, 2), kKey);
    libk::write_le<mx_u16_t>(libk::as_bytes(frame + 10, 2), 4);
    frame[16] = 4;
    frame[18] = 2;
    frame[19] = 1;
    libk::write_le<mx_u32_t>(libk::as_bytes(frame + 12, 4), checksum(libk::as_bytes(frame)));
    for (auto byte : frame) {
        parser.push(byte);
    }
    assert(parser.complete && parser.target == 20);
    parser.reset();
    frame[19] ^= 1;
    for (auto byte : frame) {
        parser.push(byte);
    }
    assert(!parser.complete && !parser.used);
    parser.reset();
    frame[19] ^= 1;
    for (mx_u32_t n = 0; n < 256; ++n) {
        parser.push(0xff);
    }
    for (auto byte : frame) {
        parser.push(byte);
    }
    assert(parser.complete);
    parser.reset();
    mx_u8_t descriptors[256]{};
    midix::usb::InterfaceDescriptor interfaces[4]{};
    midix::usb::EndpointDescriptor endpoints[8]{};
    const MxUsbSpeed speeds[]={MxUsbSpeed::Full,MxUsbSpeed::High};
    for (auto speed : speeds) {
        const auto length = mochi::usb::gadget::configuration(libk::as_bytes(descriptors),
                                                              speed == MxUsbSpeed::High);
        midix::usb::Configuration config{interfaces, endpoints, 4, 8, 0, 0, 0, 0, 0, 0};
        assert(length == 98 && midix::usb::parse_configuration(libk::as_bytes(descriptors, length),
                                                               speed, &config));
        assert(config.interface_count == 3 && config.endpoint_count == 5);
        assert(endpoints[1].max_packet == (speed == MxUsbSpeed::High ? 512 : 64));
    }
    assert(mochi::usb::gadget::string_descriptor(libk::as_bytes(descriptors), 1) == 8);
    assert(!mochi::usb::gadget::string_descriptor(libk::as_bytes(descriptors), 255));
    std::puts("usb debug probe ok");
}
