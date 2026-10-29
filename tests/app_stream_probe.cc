#include <cassert>
#include <array>
#include <cstdio>
#include "../userland/apps/session.h"

struct Resources {
    std::array<mx_u8_t, 128> upload{};
    bool fail_allocate = false, fail_launch = false, child = false, exited = false;
    unsigned clears = 0, kills = 0;
    bool allocate(mx_u32_t size) {
        assert(size <= upload.size());
        upload.fill(0xa5); // a partial allocation must also be cleared on error
        return !fail_allocate;
    }
    void write(mx_u32_t offset, libk::const_bytes bytes) {
        assert(offset + bytes.size <= upload.size());
        libk::copy_exact(libk::as_bytes(upload.data() + offset, bytes.size), bytes);
    }
    void discard_upload() { upload.fill(0); }
    mx_status_t launch(mx_u32_t) { child = !fail_launch; return fail_launch ? MX_ERR_BAD_IMAGE : MX_OK; }
    bool has_child() { return child; }
    void kill() { ++kills; }
    bool poll_exit(mx_i32_t& code) { if (!exited) return false; code = 42; exited = false; return true; }
    void clear_app() { child = false; ++clears; }
};
using Session = mochi::apps::Session<Resources>;
static mx_status_t request(Session& s, mx_u16_t type, libk::const_bytes body = {}, mx_u64_t now = 100) {
    mx_u8_t bytes[64]{};
    libk::write_le<mx_u64_t>(libk::as_bytes(bytes, 8), s.id);
    libk::copy_exact(libk::as_bytes(bytes + 8, body.size), body);
    return s.request(type, type == midix::apps::kHello ? body : libk::as_bytes(bytes, body.size + 8), 7, now);
}
static void begin(Session& s) {
    mx_u8_t body[8]{};
    libk::write_le<mx_u32_t>(libk::as_bytes(body, 4), 4);
    libk::write_le<mx_u32_t>(libk::as_bytes(body + 4, 4), 1);
    assert(request(s, midix::apps::kBegin, libk::as_bytes(body)) == MX_OK);
}
static void upload(Session& s) {
    mx_u8_t body[8] = {0, 0, 0, 0, 1, 2, 3, 4};
    assert(request(s, midix::apps::kData, libk::as_bytes(body)) == MX_OK);
}
int main() {
    using namespace midix::apps;
    Session s{};
    assert(request(s, kHello) == MX_ERR_SERVICE_UNAVAILABLE);
    assert(s.start(99) == MX_OK && s.state == Ready);
    assert(s.start(100) == MX_ERR_BUSY);
    assert(request(s, kHello) == MX_OK);
    const mx_u8_t stale[8]{};
    assert(s.request(kPoll, libk::as_bytes(stale), 7, 100) == MX_ERR_DEAD);
    s.resources.fail_allocate = true;
    mx_u8_t begin_body[8] = {4, 0, 0, 0, 1, 0, 0, 0};
    assert(request(s, kBegin, libk::as_bytes(begin_body)) == MX_ERR_NOMEM);
    assert(libk::is_zero(libk::as_bytes(s.resources.upload.data(), 128)));
    s.resources.fail_allocate = false;
    begin(s);
    assert(request(s, kRun) == MX_ERR_INVALID); // truncated executable
    mx_u8_t wrong_offset[5] = {1, 0, 0, 0, 42};
    assert(request(s, kData, libk::as_bytes(wrong_offset)) == MX_ERR_INVALID);
    upload(s);
    assert(request(s, kData, libk::as_bytes(wrong_offset)) == MX_ERR_INVALID);
    assert(request(s, kRun) == MX_OK && s.state == Running);
    assert(libk::is_zero(libk::as_bytes(s.resources.upload.data(), 128)));
    assert(request(s, kBegin, libk::as_bytes(begin_body)) == MX_ERR_BUSY);
    s.cancel(true);
    assert(s.state == Stopping && s.resources.child && s.resources.kills);
    assert(s.start(200) == MX_ERR_BUSY); // do not reuse while a core runs the child
    s.resources.exited = true;
    s.tick(200);
    assert(s.state == Closed && !s.resources.child && s.exit_code == 42);
    assert(s.start(201) == MX_OK);
    begin(s);
    s.tick(100 + kUploadTimeoutNs);
    assert(s.state == Ready && libk::is_zero(libk::as_bytes(s.resources.upload.data(), 128)));
    begin(s); upload(s); assert(request(s, kRun) == MX_OK);
    s.tick(1'000'100);
    assert(s.state == Stopping && s.resources.child);
    s.resources.exited = true; s.tick(1'000'101);
    assert(s.state == Ready && !s.resources.child);
    begin(s); upload(s);
    s.resources.fail_launch = true;
    assert(request(s, kRun) == MX_ERR_BAD_IMAGE && s.state == Ready);
    assert(libk::is_zero(libk::as_bytes(s.resources.upload.data(), 128)));
    s.resources.fail_launch = false;
    begin(s); upload(s); assert(request(s, kRun) == MX_OK);
    s.disconnect(); assert(s.state == Stopping);
    s.resources.exited = true; s.tick(300);
    assert(s.state == Closed && s.generation == 0);
    std::puts("app streaming session probe ok");
}
