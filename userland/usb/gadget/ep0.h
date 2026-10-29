#pragma once
// Standard USB requests and EP0 phases. Class request interpretation remains
// in the function process; descriptors and endpoint allocation are composition.
#include "state.h"
#include "midix/abi/diagnostics.h"
namespace mochi::usb::gadget {
inline void function_setup(Runtime& r) {
    r.function_request = {r.setup.id, r.function_generation, r.setup.setup, MX_OK, 0, {}};
    if (r.setup.setup.length && !(r.setup.setup.request_type & 128)) {
        if (r.setup.setup.length > 16) {
            stall(r);
            return;
        }
        r.phase = ControlPhase::OutClass;
    } else {
        r.pending_function = true;
        r.phase = ControlPhase::Function;
    }
}
inline void setup(Runtime& r, const MxDeviceEvent& event) {
    const auto previous_phase = r.phase;
    r.setup = event;
    r.generation = event.generation;
    r.speed = static_cast<MxUsbSpeed>(event.speed);
    r.phase = ControlPhase::Idle;
    r.ep0_active = false;
    r.pending_function = false;
    r.observe_pending = false;
    r.observe_history = false;
    r.observe_trial = false;
    r.address_pending = false;
    const auto& s = event.setup;
    // Fixed retained-log tail for diagnosing a USB service that restarted.
    // Metadata freezes an at-most-8-KiB window; page reads never accept an
    // address or extend beyond it. Each response fits one EP0 packet.
    if (s.request_type == 0xc0 && s.request == 0x5f && s.length && s.length <= 64) {
        if (!s.value && !s.index && s.length == 16) {
            MxDiagSystem info{};
            const auto got=sys_debug_read(MX_DIAG_SYSTEM,0,&info,sizeof(info));
            if (got != sizeof(info) || info.boot_log_size > info.boot_log_capacity ||
                info.boot_log_size > 0xffffffffu) { stall(r);return; }
            r.log_tail_length=info.boot_log_size < 8192 ? info.boot_log_size : 8192;
            r.log_tail_start=info.boot_log_size-r.log_tail_length;
            const mx_u32_t words[]={0x31474c42u,r.log_tail_start,r.log_tail_length,
                                   static_cast<mx_u32_t>(info.boot_log_size)};
            libk::copy_n(libk::slice(r.payload,512,sizeof(words)),
                        libk::as_bytes(words,sizeof(words)),sizeof(words));
            reply_data(r,sizeof(words));return;
        }
        const mx_u32_t offset=mx_u32_t{s.index}*64;
        if (s.value==1 && offset < r.log_tail_length) {
            const auto left=r.log_tail_length-offset;
            const auto count=s.length < left ? s.length : left;
            const auto got=sys_debug_read(MX_DIAG_BOOT_LOG,r.log_tail_start+offset,
                                         r.payload.data+512,count);
            if (got==count) {reply_data(r,count);return;}
        }
        stall(r);return;
    }
    if (s.request_type == 0xc0 && s.request == 0x5e && !s.value && !s.index &&
        s.length == 48) {
        const auto& last=r.last_out_control;
        const mx_u32_t words[]={0x31534341u,
            last.setup.request_type | (mx_u32_t{last.setup.request}<<8) |
                (mx_u32_t{last.setup.length}<<16),
            last.setup.index | (mx_u32_t{last.setup.value}<<16), last.length,
            static_cast<mx_u32_t>(r.last_out_reply_status),r.last_out_reply_length,
            static_cast<mx_u32_t>(last.id),static_cast<mx_u32_t>(last.id>>32),
            libk::load_le<mx_u32_t>(last.data),libk::load_le<mx_u32_t>(last.data+4),
            libk::load_le<mx_u32_t>(last.data+8),libk::load_le<mx_u32_t>(last.data+12)};
        libk::copy_n(libk::slice(r.payload,512,sizeof(words)),
                    libk::as_bytes(words,sizeof(words)),sizeof(words));
        reply_data(r,sizeof(words));
        return;
    }
    // An explicit one-shot trial can be armed after normal enumeration. It
    // exposes two fixed receive sequences, never caller-chosen registers.
    if (s.request_type == 0xc0 && s.request == 0x5d && s.value <= 2 &&
        !s.index && s.length == sizeof(MxDeviceSnapshot) && r.configuration) {
        r.observe_endpoint=0;
        r.observe_trial=true;
        r.trial_mode=s.value;
        r.observe_pending=true;
        r.phase=ControlPhase::Observe;
        return;
    }
    // Development diagnostics over EP0 stay usable when a byte endpoint is
    // stalled. Return bounded state only, never arbitrary memory/registers.
    if (s.request_type == 0xc0 && s.request == 0x5a && !s.value && !s.index && s.length) {
        const auto& f=r.functions[0];
        const mx_u32_t words[]={0x31475355u,static_cast<mx_u32_t>(previous_phase),
            r.configuration,static_cast<mx_u32_t>(r.speed),
            static_cast<mx_u32_t>(f.rx_active | (f.rx_pending << 1) |
                (f.tx_active << 2) | (f.tx_pending << 3) | (f.reset << 4)),
            f.received,f.forwarded,f.tx_bytes,
            r.links[0].tx().depth(),r.links[0].rx().depth(),
            r.links[1].tx().depth(),r.links[1].rx().depth(),
            r.acm_control.tx().depth(),r.acm_control.rx().depth(),
            (static_cast<mx_u32_t>(r.function_request.setup.request) << 16) |
                r.function_request.setup.length, r.function_request.length};
        libk::copy_n(libk::slice(r.payload,512,sizeof(words)),
                    libk::as_bytes(words,sizeof(words)),sizeof(words));
        reply_data(r,sizeof(words));
        return;
    }
    if (s.request_type == 0xc0 && !s.value && s.length &&
        ((s.request == 0x5b && !(s.index & ~0x87u)) ||
         (s.request == 0x5c && !s.index))) {
        r.observe_endpoint=s.index;
        r.observe_history=s.request==0x5c;
        r.observe_pending=true;
        r.phase=ControlPhase::Observe;
        return;
    }
    // Descriptor discovery and HID SET_IDLE are handled without spawning FIDO.
    if (s.request_type == 0x81 && s.request == 6 && s.index == 3 &&
        (s.value == 0x2200 || s.value == 0x2100)) {
        const auto descriptor = s.value == 0x2200 ? fido_report_descriptor() : fido_hid_descriptor();
        libk::copy_n(libk::slice(r.payload, 512, 512), descriptor, descriptor.size);
        reply_data(r, descriptor.size);
        return;
    }
    if (s.request_type == 0x21 && s.request == 10 && s.index == 3 && !s.length &&
        !(s.value & 255) && r.configuration) {
        r.phase = ControlPhase::StatusIn;
        return;
    }
    if ((s.request_type & 0x60) == 0x20 && (s.request_type & 31) == 1 && s.index == 0) {
        function_setup(r);
        return;
    }
    if (s.request_type & 0x60) {
        stall(r);
        return;
    }
    if (s.request == 6 && s.request_type == 0x80) {
        const auto kind = s.value >> 8, index = s.value & 255;
        mx_u32_t length = 0;
        auto buffer = libk::slice(r.payload, 512, 512);
        if (kind == 1 && !index) {
            const auto d = mochi::usb::gadget::device_descriptor();
            libk::copy_n(buffer, d, d.size);
            length = d.size;
        } else if ((kind == 2 || kind == 7) && !index) {
            length = mochi::usb::gadget::configuration(
                buffer, kind == 7 ? r.speed != MxUsbSpeed::High : r.speed == MxUsbSpeed::High,
                kind == 7);
        } else if (kind == 3 && (s.index == 0 || s.index == 0x409)) {
            length = mochi::usb::gadget::string_descriptor(buffer, index);
        } else if (kind == 6 && !index) {
            const auto d = mochi::usb::gadget::qualifier();
            libk::copy_n(buffer, d, d.size);
            length = d.size;
        }
        if (length) {
            reply_data(r, length);
        } else {
            stall(r);
        }
    } else if (s.request == 5 && s.request_type == 0 && !s.index && !s.length && s.value <= 127) {
        r.pending_address = s.value;
        r.address_pending = true;
        r.phase = ControlPhase::StatusIn;
    } else if (s.request == 9 && s.request_type == 0 && !s.index && !s.length && s.value <= 1) {
        r.configuration = s.value;
        r.configure_next = s.value ? 0 : kFunctionEndpointCount;
        reset_functions(r);
        r.phase = ControlPhase::StatusIn;
    } else if (s.request == 8 && s.request_type == 0x80 && !s.index && !s.value && s.length == 1) {
        r.payload.data[512] = r.configuration;
        reply_data(r, 1);
    } else if (s.request == 0 &&
               (s.request_type == 0x80 || s.request_type == 0x81 || s.request_type == 0x82) &&
               !s.value && s.length == 2) {
        r.payload.data[512] = r.payload.data[513] = 0;
        reply_data(r, 2);
    } else if (s.request == 10 && s.request_type == 0x81 && s.index < 4 && !s.value &&
               s.length == 1) {
        r.payload.data[512] = 0;
        reply_data(r, 1);
    } else if (s.request == 11 && s.request_type == 1 && s.index < 4 && !s.value && !s.length) {
        r.phase = ControlPhase::StatusIn;
    } else {
        stall(r);
    }
}
inline void completion(Runtime& r, const MxDeviceEvent& done) {
    if (done.generation != r.generation) {
        return;
    }
    if (!(done.endpoint & 15)) {
        if (done.id != r.setup.id) {
            return;
        }
        r.ep0_active = false;
        if (done.status != MxUsbStatus::Success) {
            stall(r);
            return;
        }
        if (r.phase == ControlPhase::In) {
            r.sent += done.length;
            if (r.sent >= r.length) {
                if (r.zlp && done.length) {
                    r.zlp = false;
                } else {
                    r.phase = ControlPhase::StatusOut;
                }
            }
        } else if (r.phase == ControlPhase::OutClass) {
            if (done.length != r.setup.setup.length) {
                stall(r);
                return;
            }
            r.function_request.length = done.length;
            libk::copy_n(libk::as_bytes(r.function_request.data),
                         libk::slice(r.payload, 0, done.length), done.length);
            r.last_out_control=r.function_request;
            r.last_out_reply_status=MX_ERR_BUSY;
            r.last_out_reply_length=0;
            r.pending_function = true;
            r.phase = ControlPhase::Function;
        } else if (r.phase == ControlPhase::StatusIn) {
            r.phase = ControlPhase::Idle;
        } else if (r.phase == ControlPhase::StatusOut) {
            r.phase = ControlPhase::Idle;
        }
        return;
    }
    const auto endpoint = done.endpoint & 15;
    if (endpoint != 1 && endpoint != 2 && endpoint != 4 && endpoint != 5 && endpoint != 6) return;
    const auto index = endpoint == 6 ? 2u : endpoint >= 4 ? 1u : 0u;
    auto& f = r.functions[index];
    if (done.endpoint & 128) {
        if (done.id != f.tx_id || !f.tx_active) {
            return;
        }
        f.tx_bytes = 0;
        f.tx_active = false;
        f.tx_pending = false;
    } else {
        if (done.id != f.rx_id || !f.rx_active) {
            return;
        }
        f.rx_active = false;
        f.received = done.length;
        f.forwarded = 0;
        f.rx_pending = done.status == MxUsbStatus::Success && done.length;
        if (index == 2) {
            f.rx_pending = done.status == MxUsbStatus::Success && done.length == 64;
            if (f.rx_pending && !r.fido2_requested) {
                r.fido2_requested = true;
                r.demand_pending = true;
                f.reset = true;
            }
        }
    }
}
} // namespace mochi::usb::gadget
