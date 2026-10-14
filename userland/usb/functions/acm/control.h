#pragma once
// CDC ACM class policy only. Stream forwarding, wakeups and process ownership
// are independent of line-coding requests; diagnostic framing stays elsewhere.
#include "midix/abi/protocols/usb_function.h"
#include "libk/endian.h"
namespace mochi::usb::acm {
struct Control {
    mx_u8_t coding[7];
    mx_u16_t lines, break_duration;
    void init() {
        libk::write_le<mx_u32_t>(libk::as_bytes(coding, 4), 115200);
        coding[6] = 8;
    }
    void reply(MxFunctionSetup& request) {
        const auto& setup = request.setup;
        request.status = MX_ERR_NOT_IMPLEMENTED;
        if (setup.index != 0) {
            return;
        }
        if (setup.request_type == 0xa1 && setup.request == 0x21 && setup.length == 7) {
            libk::copy_n(libk::as_bytes(request.data), libk::as_bytes(coding), 7);
            request.length = 7;
            request.status = MX_OK;
        } else if (setup.request_type == 0x21 && setup.request == 0x20 && setup.length == 7 &&
                   request.length == 7) {
            // CDC 1.2: stop/parity encodings and supported data-bit counts.
            const auto bits = request.data[6];
            if (request.data[4] <= 2 && request.data[5] <= 4 && bits >= 5 && bits <= 8) {
                libk::copy_n(libk::as_bytes(coding), libk::as_bytes(request.data), 7);
                request.length = 0;
                request.status = MX_OK;
            }
        } else if (setup.request_type == 0x21 && !setup.length &&
                   (setup.request == 0x22 || setup.request == 0x23)) {
            if (setup.request == 0x22) {
                lines = setup.value;
            } else {
                break_duration = setup.value;
            }
            request.length = 0;
            request.status = MX_OK;
        }
    }
};
} // namespace mochi::usb::acm
