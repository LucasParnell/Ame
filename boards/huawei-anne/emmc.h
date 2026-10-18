#pragma once
#include "anko/emmc.h"
#include "rpmb.h"

namespace anne::emmc {
inline constexpr u64 kBase = 0xff390000;
inline constexpr u64 kBytes = 0x1000;

// Adapts the pinned Huawei shared-window protocol to the real card transport.
// A successful WR_CNT reserves the RPMB writer until the final chunk or error.
// One owner must serialize prepare/service/release. No lock spans an SMC.
class Backend {
public:
    explicit Backend(drivers::emmc::Driver& driver) : driver_(driver) {}
    bool config(rpmb::ConfigInfo& out) const;
    rpmb::KeyInfo key_info();
    u64 provision_key(volatile u8* window, usize bytes);
    u64 service(volatile u8* window, usize bytes);
    bool write_pending() const { return pending_; }
private:
    drivers::emmc::Driver& driver_;
    drivers::emmc::Frame request_[32]{}, response_[32]{};
    bool pending_{};
    bool key_ready_{};
    u32 owner_{}, base_{}, total_{}, next_{};
};

// Native singleton. All access is serialized with a nonblocking gate, leaving
// timer/watchdog IRQs enabled during potentially slow PIO.
bool prepare(rpmb::ConfigInfo& config);
u64 service(volatile u8* window);
// Explicit one-time RPMB key provisioning. This is never used by normal
// SGI-5 request servicing.
u64 provision_key(volatile u8* window);
rpmb::KeyInfo key_info();
void delay_ms(u32 ms);
bool write_pending();
bool claim_user(u64 identity);
void release_user(u64 identity);
drivers::emmc::Error read(u32 lba, u32 count, void* buffer);
drivers::emmc::Error write(u32 lba, u32 count, const void* buffer);
// Copies the last controller failure and EXT_CSD without issuing card commands.
bool status(drivers::emmc::CardInfo& info, drivers::emmc::Failure& failure);
}
