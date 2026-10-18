#include "emmc.h"
#include "boot_diagnostics.h"
#include "anko/platform_clock.h"
#include "rpmb_diagnostics.h"

namespace anne::emmc {
namespace {
u32 le32(const volatile u8* p) {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}
u16 be16(const u8* p) { return (u16(p[0]) << 8) | p[1]; }
u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
constexpr u64 kMmcError = 0xff04, kInvalid = 0xff03, kKeyError = 0xff09;
void barrier() {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
    asm volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
}
bool Backend::config(rpmb::ConfigInfo& out) const {
    if (!driver_.ready() || !driver_.info().ext_csd[168]) return false;
    const u8* csd = driver_.info().ext_csd;
    out = rpmb::emmc_config({csd[168], csd[166], csd[124], {csd[180], csd[182], csd[186], csd[188]}});
    return true;
}
rpmb::KeyInfo Backend::key_info() {
    rpmb::KeyInfo out{};
    if (pending_) { out.error = kMmcError; return out; }
    key_ready_ = false;
    if (!driver_.ready()) { out.error = rpmb::kErrorBlockDevice; return out; }
    drivers::emmc::Frame request{}, response{};
    request.bytes[511] = 2;
    if (driver_.rpmb(&request, 1, &response, 1) != drivers::emmc::Error::Ok) {
        out.error = kMmcError; return out;
    }
    out.response = be16(response.bytes + 510);
    out.result = be16(response.bytes + 508);
    out.counter = be32(response.bytes + 500);
    key_ready_ = out.present();
    return out;
}
u64 Backend::provision_key(volatile u8* window, usize bytes) {
    if (!window || bytes < 512 || pending_) return kInvalid;
    if (!driver_.ready()) return rpmb::kErrorBlockDevice;
    drivers::emmc::Frame request{}, response{};
    struct Wipe {
        drivers::emmc::Frame& frame;
        ~Wipe() { for (usize i = 0; i < 512; ++i) static_cast<volatile u8*>(frame.bytes)[i] = 0; }
    } wipe{request}, wipe_response{response};
    barrier();
    for (u32 i = 0; i < 512; ++i) request.bytes[i] = window[i];
    if (be16(request.bytes + 510) != 1) return rpmb::kErrorSetKey;
    key_ready_ = false;
    const auto result = driver_.rpmb(&request, 1, &response, 1);
    if (result != drivers::emmc::Error::Ok) return kMmcError;
    for (u32 i = 0; i < 512; ++i) window[i] = response.bytes[i];
    barrier();
    return 0;
}
u64 Backend::service(volatile u8* window, usize bytes) {
    if (!window || bytes < rpmb::kExceptionStatusOffset + 2) return kInvalid;
    if (!driver_.ready()) { pending_ = false; return rpmb::kErrorBlockDevice; }
    // Same guard as Huawei rpmb_work_routine: unprovisioned Hynix devices may
    // not accept reads larger than 4 KiB. Probe readiness with one counter frame.
    if (!key_ready_) return rpmb::kErrorKey;
    barrier();
    const auto* info = window + rpmb::kRequestInfoOffset;
    const u32 owner = le32(info), base = le32(info + 8), total = le32(info + 12);
    const u32 state = le32(info + 20), offset = le32(info + 24), count = le32(info + 28);
    if (state == 1) return kKeyError; // never program a permanent key
    if (state < 2 || state > 5) return kInvalid;
    if (pending_ && (state != 5 || owner != owner_ || base != base_ || total != total_ || offset != next_))
        return kMmcError;
    if (state == 5 && !pending_) return kMmcError;
    auto invalid = [&]() { if (state == 5) pending_ = false; return kInvalid; };
    const bool data = state == 2 || state == 5;
    if (data && (!count || count > 32 || offset > total || count > total - offset)) return invalid();
    if (state == 4 && (!total || offset)) return kInvalid;
    const u32 requests = state == 5 ? count : 1, responses = state == 2 ? count : 1;
    // Use explicit per-frame copies; the firmware window is volatile and may
    // have no alignment suitable for a C++ Frame object.
    for (u32 f = 0; f < requests; ++f)
        for (u32 i = 0; i < 512; ++i) request_[f].bytes[i] = window[f * 512 + i];
    const u16 expected = state == 2 ? 4 : state == 5 ? 3 : 2;
    if ((be16(request_[0].bytes + 510) & 0xfff) != expected) return invalid();
    // info.base is an opaque firmware transaction field. Hardware addresses
    // come from the authenticated frame and are bounded by Driver::rpmb.
    const auto result = driver_.rpmb(request_, requests, response_, responses);
    if (result != drivers::emmc::Error::Ok) { pending_ = false; return kMmcError; }
    for (u32 f = 0; f < responses; ++f)
        for (u32 i = 0; i < 512; ++i) window[f * 512 + i] = response_[f].bytes[i];
    barrier();
    const bool success = be16(response_[0].bytes + 508) == 0;
    if (state == 4 && success) {
        // Match emmc_rpmb_unlock_counterlock: malformed counter responses must
        // reach firmware without retaining the writer reservation.
        pending_ = be16(response_[0].bytes + 510) == 0x0200;
        owner_ = owner; base_ = base; total_ = total; next_ = 0;
    } else if (state == 5) {
        next_ += count;
        if (!success || next_ == total_) pending_ = false;
    }
    return 0; // card result remains in the real response for secure validation
}

#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST) && !defined(ANKO_HOST_TEST)
namespace {
u32 native_read(void*, u32 offset, unsigned width) {
    const u64 address = kBase + offset;
    u32 value;
    asm volatile("dmb osh" ::: "memory");
    if (width == 1) value = *reinterpret_cast<volatile u8*>(address);
    else if (width == 2) value = *reinterpret_cast<volatile u16*>(address);
    else value = *reinterpret_cast<volatile u32*>(address);
    asm volatile("dmb osh" ::: "memory");
    return value;
}
void native_write(void*, u32 offset, unsigned width, u32 value) {
    const u64 address = kBase + offset;
    asm volatile("dmb osh" ::: "memory");
    if (width == 1) *reinterpret_cast<volatile u8*>(address) = u8(value);
    else if (width == 2) *reinterpret_cast<volatile u16*>(address) = u16(value);
    else *reinterpret_cast<volatile u32*>(address) = value;
    asm volatile("dmb osh" ::: "memory");
}
u64 native_us(void*) {
    u64 ticks, frequency;
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    frequency = platform::counter_frequency(frequency);
    return (ticks / frequency) * 1000000 + (ticks % frequency) * 1000000 / frequency;
}
drivers::emmc::Driver driver({nullptr, native_read, native_write, native_us});
Backend backend(driver);
u32 gate;
u64 user_owner, controller_epoch, driver_epoch;
class Access {
public:
    Access() { u32 empty = 0; held = __atomic_compare_exchange_n(&gate, &empty, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); }
    ~Access() { if (held) __atomic_store_n(&gate, 0u, __ATOMIC_RELEASE); }
    bool held;
};
}
bool prepare(rpmb::ConfigInfo& config) {
    Access access;
    if (!access.held || __atomic_load_n(&user_owner, __ATOMIC_ACQUIRE)) return false;
    const u64 epoch = __atomic_load_n(&controller_epoch, __ATOMIC_ACQUIRE);
    if ((!driver.ready() || driver_epoch != epoch) && driver.initialize(200000000, true) != drivers::emmc::Error::Ok) return false;
    driver_epoch = epoch;
    return backend.config(config);
}
u64 service(volatile u8* window) {
    Access access;
    return access.held && !__atomic_load_n(&user_owner, __ATOMIC_ACQUIRE) ? backend.service(window, anne::kRpmbRequestSize) : kMmcError;
}
u64 provision_key(volatile u8* window) {
    Access access;
    return access.held && !__atomic_load_n(&user_owner, __ATOMIC_ACQUIRE) ? backend.provision_key(window, anne::kRpmbRequestSize) : kMmcError;
}
rpmb::KeyInfo key_info() {
    Access access;
    return access.held && !__atomic_load_n(&user_owner, __ATOMIC_ACQUIRE) ? backend.key_info() : rpmb::KeyInfo{kMmcError};
}
void delay_ms(u32 ms) {
    const u64 start = native_us(nullptr);
    while (native_us(nullptr) - start < u64(ms) * 1000) asm volatile("yield" ::: "memory");
}
bool write_pending() { Access access; return !access.held || backend.write_pending(); }
bool claim_user(u64 identity) {
    rpmb_diag::ResearchAccess research;
    Access access;
    if (!identity || !research || !access.held || backend.write_pending()) return false;
    const auto owner = __atomic_load_n(&user_owner, __ATOMIC_ACQUIRE);
    if (owner == identity) return true;
    if (owner) return false;
    __atomic_add_fetch(&controller_epoch, u64{1}, __ATOMIC_RELEASE);
    __atomic_store_n(&user_owner, identity, __ATOMIC_RELEASE);
    return true;
}
void release_user(u64 identity) {
    Access access;
    if (!access.held || __atomic_load_n(&user_owner, __ATOMIC_ACQUIRE) != identity) return;
    // Stop ADMA before a replacement driver can access the pinned arena.
    // If reset does not finish, retain ownership and refuse further claims.
    native_write(nullptr, 0x38, 4, 0);
    native_write(nullptr, 0x2f, 1, 6);
    const u64 start = native_us(nullptr);
    while (native_read(nullptr, 0x2f, 1) & 6) {
        if (native_us(nullptr) - start >= 100000) return;
    }
    asm volatile("dsb osh" ::: "memory");
    native_write(nullptr, 0x58, 4, 0);
    __atomic_store_n(&user_owner, u64{0}, __ATOMIC_RELEASE);
}
drivers::emmc::Error read(u32 lba, u32 count, void* buffer) {
    Access access;
    return access.held && !__atomic_load_n(&user_owner, __ATOMIC_ACQUIRE) ? driver.read(lba, count, buffer) : drivers::emmc::Error::Busy;
}
drivers::emmc::Error write(u32 lba, u32 count, const void* buffer) {
    Access access;
    return access.held && !__atomic_load_n(&user_owner, __ATOMIC_ACQUIRE) ? driver.write(lba, count, buffer) : drivers::emmc::Error::Busy;
}
bool status(drivers::emmc::CardInfo& info, drivers::emmc::Failure& failure) {
    Access access;
    if (!access.held) return false;
    info = driver.info(); failure = driver.failure();
    return driver.ready() && !__atomic_load_n(&user_owner, __ATOMIC_ACQUIRE);
}
#else
bool prepare(rpmb::ConfigInfo&) { return false; }
u64 service(volatile u8*) { return rpmb::kErrorBlockDevice; }
u64 provision_key(volatile u8*) { return rpmb::kErrorBlockDevice; }
rpmb::KeyInfo key_info() { return {rpmb::kErrorBlockDevice}; }
void delay_ms(u32) {}
bool write_pending() { return false; }
bool claim_user(u64) { return false; }
void release_user(u64) {}
drivers::emmc::Error read(u32, u32, void*) { return drivers::emmc::Error::NotReady; }
drivers::emmc::Error write(u32, u32, const void*) { return drivers::emmc::Error::NotReady; }
bool status(drivers::emmc::CardInfo& info, drivers::emmc::Failure& failure) {
    info = {}; failure = {drivers::emmc::Error::NotReady, 0, 0, 0}; return false;
}
#endif
}
