#pragma once
#include "midix/abi/protocols/app_stream.h"
#include "midix/abi/status.h"
#include "libk/endian.h"
#include "libk/bytes.h"

namespace mochi::apps {
// Resources owns the upload arenas and suspended child. Cleanup waits for the
// generation-tagged exit before permitting another upload or reporting closed.
template <class Resources> struct Session {
    Resources resources;
    midix::apps::State state = midix::apps::Closed;
    mx_u64_t id = 0, generation = 0, deadline = 0;
    mx_u32_t size = 0, received = 0, run_ms = 0;
    mx_i32_t exit_code = midix::apps::kNoExit;
    bool close_after_exit = false;

    void discard_upload() {
        resources.discard_upload();
        size = received = 0;
    }
    mx_status_t start(mx_u64_t now) {
        if (state != midix::apps::Closed) return MX_ERR_BUSY;
        id = now ? now : id + 1;
        generation = deadline = 0;
        exit_code = midix::apps::kNoExit;
        close_after_exit = false;
        state = midix::apps::Ready;
        return MX_OK;
    }
    void cancel(bool close) {
        discard_upload();
        if (state == midix::apps::Running || state == midix::apps::Stopping) {
            close_after_exit |= close;
            resources.kill();
            state = midix::apps::Stopping;
        } else {
            resources.clear_app();
            state = close || state == midix::apps::Closed ? midix::apps::Closed : midix::apps::Ready;
        }
    }
    void tick(mx_u64_t now) {
        if (state == midix::apps::Running || state == midix::apps::Stopping) {
            mx_i32_t code;
            if (resources.poll_exit(code)) {
                exit_code = code;
                resources.clear_app();
                discard_upload();
                state = close_after_exit ? midix::apps::Closed : midix::apps::Ready;
                deadline = 0;
                close_after_exit = false;
            } else if (state == midix::apps::Running && now >= deadline) {
                cancel(false);
            }
        } else if (state == midix::apps::Receiving && now >= deadline) {
            cancel(false);
        }
    }
    void disconnect() { generation = 0; cancel(true); }
    mx_status_t request(mx_u16_t type, libk::const_bytes payload,
                        mx_u64_t usb_generation, mx_u64_t now) {
        using namespace midix::apps;
        tick(now);
        if (!usb_generation || state == Closed) return MX_ERR_SERVICE_UNAVAILABLE;
        if (generation && generation != usb_generation) return MX_ERR_DEAD;
        if (!generation) generation = usb_generation;
        if (type != kHello) {
            if (payload.size < 8 || libk::load_le<mx_u64_t>(payload.data) != id) return MX_ERR_DEAD;
            payload = libk::slice(payload, 8, payload.size - 8);
        }
        if (type == kHello || type == kPoll) return payload.size ? MX_ERR_INVALID : MX_OK;
        if (type == kCancel) {
            if (payload.size) return MX_ERR_INVALID;
            cancel(false);
            return MX_OK;
        }
        if (type == kBegin) {
            if (state != Ready) return MX_ERR_BUSY;
            if (payload.size != 8) return MX_ERR_INVALID;
            const auto bytes = libk::load_le<mx_u32_t>(payload.data);
            const auto limit = libk::load_le<mx_u32_t>(payload.data + 4);
            if (!bytes || bytes > kMaxImage || !limit || limit > kMaxRunMs) return MX_ERR_INVALID;
            if (!resources.allocate(bytes)) { discard_upload(); return MX_ERR_NOMEM; }
            size = bytes;
            received = 0;
            run_ms = limit;
            exit_code = kNoExit;
            state = Receiving;
            deadline = now + kUploadTimeoutNs;
            return MX_OK;
        }
        if (type == kData) {
            if (state != Receiving) return MX_ERR_BUSY;
            if (payload.size <= 4 || payload.size > 56) return MX_ERR_INVALID;
            const auto offset = libk::load_le<mx_u32_t>(payload.data);
            const auto count = static_cast<mx_u32_t>(payload.size - 4);
            if (offset != received || count > size - received) return MX_ERR_INVALID;
            resources.write(received, libk::slice(payload, 4, count));
            received += count;
            deadline = now + kUploadTimeoutNs;
            return MX_OK;
        }
        if (type == kRun) {
            if (state != Receiving || received != size || payload.size) return MX_ERR_INVALID;
            const auto status = resources.launch(size);
            discard_upload(); // loader copies into separately owned child pages
            if (status != MX_OK) {
                if (resources.has_child()) {
                    resources.kill();
                    state = Stopping;
                } else {
                    resources.clear_app();
                    state = Ready;
                }
                return status;
            }
            state = Running;
            deadline = now + static_cast<mx_u64_t>(run_ms) * 1'000'000;
            return MX_OK;
        }
        return MX_ERR_BAD_PROTOCOL;
    }
};
} // namespace mochi::apps
