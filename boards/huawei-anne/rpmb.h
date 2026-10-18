#pragma once
#include "anko/types.h"

// Oracle: hisi-oss/android_kernel_huawei_hi6250-8 at
// 65e545cbaa11d132e243287b150146bc2a51c45e, drivers/mmc/core/emmc-rpmb.c
// and include/linux/bootdevice.h. Configuration units count 256-byte data
// blocks; the MMC transport frame is separately 512 bytes.
namespace anne::rpmb {
// drivers/mmc/card/hisi_rpmb.h; BOOT_DEVICE_EMMC in linux/bootdevice.h.
inline constexpr u64 kCallCountSmc = 0xc6000000;
inline constexpr u64 kUidSmc = 0xc6000001;
inline constexpr u64 kVersionSmc = 0xc6000002;
inline constexpr u64 kRequestAddressSmc = 0xc600ff04;
inline constexpr u64 kRequestDoneSmc = 0xc600ff06;
inline constexpr u64 kSetKeySmc = 0xc600ff05;
inline constexpr u64 kBootDeviceEmmc = 0;
inline constexpr u64 kCounterSmc = 0xc600fff3;
inline constexpr u64 kErrorBlockDevice = 0xff05;
// arch/arm64/kernel/smp.c: IPI_SECURE_RPMB is the sixth enum member.
inline constexpr u32 kRequestSgi = 5;
inline constexpr u64 kRequestInfoOffset = 0x6a00;
inline constexpr u64 kKeyStatusOffset = 0x6a20;
inline constexpr u64 kRequestStatusOffset = 0x6a22;
inline constexpr u64 kExceptionStatusOffset = 0x6a24;
inline constexpr u64 kErrorMmc = 0xff04;
inline constexpr u64 kErrorKey = 7;
inline constexpr u64 kErrorSetKey = 0xff09;
inline constexpr u64 kErrorCounter = 0xff0a;
inline constexpr u64 kErrorTimeout = 0xff0b;

// Like Huawei's mmc_rpmb_key_status: this checks the counter response, not its
// HMAC. Secure firmware owns the key and authenticates operational responses.
struct KeyInfo {
    u64 error{};
    u32 counter{};
    u16 result{}, response{};
    bool present() const { return !error && response == 0x0200 && result == 0; }
    bool missing() const { return !error && response == 0x0200 && result == 7; }
};

struct ConfigInfo {
    u64 total_blks;
    u64 read_frame_support;
    u64 write_frame_support;
    u64 unit_size;
    u8 region_size[4];
    u8 blk_size;
    u8 read_align;
    u8 write_align;
    u8 region_enable;
};
static_assert(sizeof(ConfigInfo) == 40);
static_assert(__builtin_offsetof(ConfigInfo, region_size) == 32);
static_assert(__builtin_offsetof(ConfigInfo, blk_size) == 36);
static_assert(__builtin_offsetof(ConfigInfo, region_enable) == 39);

struct EmmcInfo {
    u8 size_mult;
    u8 wr_rel_param;
    u8 vendor_feature_support;
    u8 region_size_mult[4];
};

constexpr ConfigInfo emmc_config(const EmmcInfo& info) {
    ConfigInfo config{};
    config.unit_size = 128 * 1024;
    config.blk_size = 8;
    config.total_blks = (static_cast<u64>(info.size_mult) * config.unit_size) >> config.blk_size;
    config.read_frame_support = 0xffffffff;
    config.write_align = 1;
    config.write_frame_support = (info.wr_rel_param & 0x10) ? 0x80000003 : 3;
    config.read_align = (info.wr_rel_param & 0x10) ? 0 : 1;
    config.region_enable = (info.vendor_feature_support & 1) ? 0xf : 1;
    u8 regions = 0;
    for (unsigned i = 0; i < 4; ++i) {
        config.region_size[i] = info.region_size_mult[i];
        regions |= config.region_size[i];
    }
    if (!regions) config.region_size[0] = info.size_mult;
    return config;
}

// Size and 8K reliable-write support come from saved stock logs. Vendor
// feature/region bytes remain assumptions until an EXT_CSD capture exists.
inline constexpr ConfigInfo kAnneConfig = emmc_config({0x20, 0x10, 0, {0, 0, 0, 0}});

// Subtraction avoids accepting wrapped physical addresses or partial writes
// across the end of a shared window.
constexpr bool contains(u64 base, u64 size, u64 address, u64 length) {
    return address >= base && length <= size && address - base <= size - length;
}
} // namespace anne::rpmb
