#pragma once
// Dango paravirtual appliance channel ABI (spec §11).
//
// The channel is generic Dango infrastructure (not a GPU device): it carries
// control rings between `drv.gpu.linux` and the Linux guest, plus the fixed
// shared staging window. This header reuses the byte-exact Midix ring contract
// from midix/abi/ring.h so the same structures compile in native Mochi, the
// Mochi bridge, and the Linux `dango_channel` guest driver (spec §11.2).
//
// Direction:
//   host -> guest : ring + virtual IRQ notification
//   guest -> host : ring + Dango HVC notification
//
// Ring slots keep the Midix envelope: `type` is the opcode, `seq` is the
// correlation field, and `length` bounds the inline payload.

#include "midix/abi/ring.h"
#include "midix/abi/dango_channel_guest.h"
#include "midix/abi/status.h"
#include "midix/abi/types.h"

constexpr mx_u32_t MX_PROTOCOL_DANGO_CHANNEL = 0x4443'4800u; // 'DCH\0'

// Wire identity is owned by the guest ABI header (midix/abi/dango_channel_guest.h),
// which the Linux driver, the manager and the DTS generator all consume. Alias
// rather than retype so the numeric values cannot drift apart.
constexpr mx_u32_t MX_DANGO_CHANNEL_MAGIC = DANGO_CH_MAGIC; // 'DCHN'

constexpr mx_u16_t MX_DANGO_CHANNEL_VERSION_MAJOR = DANGO_CH_VERSION;
constexpr mx_u16_t MX_DANGO_CHANNEL_VERSION_MINOR = DANGO_CH_VER_MINOR;

// The rings follow the Midix ring contract: one ring page header plus a
// power-of-two slot count fixed by MX_DEFAULT_RING_ORDER.
constexpr mx_u32_t MX_DANGO_CHANNEL_RING_ORDER_DEFAULT = DANGO_CH_DEFAULT_RING_ORDER;
constexpr mx_u32_t MX_DANGO_CHANNEL_SLOTS = DANGO_RING_SLOTS(MX_DANGO_CHANNEL_RING_ORDER_DEFAULT);
static_assert(MX_DANGO_CHANNEL_RING_ORDER_DEFAULT == MX_DEFAULT_RING_ORDER,
              "channel ring order must match the Midix ring default");

enum MxDangoChannelOpcode : mx_u32_t {
    DANGO_CHANNEL_QUERY = 0x4443'4801u,
    DANGO_CHANNEL_QUERY_REPLY = 0x4443'4802u,
    DANGO_CHANNEL_NOTIFY = 0x4443'4803u,
    DANGO_CHANNEL_ERROR = 0x4443'48FFu,
};

enum MxDangoChannelDirection : mx_u32_t {
    MX_DANGO_DIR_HOST_TO_GUEST = 0,
    MX_DANGO_DIR_GUEST_TO_HOST = 1,
};

enum MxDangoChannelFlags : mx_u32_t {
    MX_DANGO_CHANNEL_FLAG_NONE = 0,
    MX_DANGO_CHANNEL_FLAG_SHARED_STAGING = 1u << 0,
    MX_DANGO_CHANNEL_FLAG_ARM_SUPPORTED = DANGO_CH_ARM_SUPPORTED,
};

// Reason carried by GPU/appliance-level notifications over this channel.
enum MxDangoChannelNotifyReason : mx_u32_t {
    MX_DANGO_NOTIFY_RING_READY = 1,
    MX_DANGO_NOTIFY_RING_PUBLISH = 2,
};

// --- Fixed channel descriptor (spec §11.2) ---------------------------------
//
// Describes both rings and the shared staging window. It lives in the guest
// DTB / reserved-memory table and in the bridge, so it is a fixed 64-byte
// structure, not a ring payload.
using MxDangoChannelDesc = dango_channel_desc;
static_assert(sizeof(MxDangoChannelDesc) == 64, "channel descriptor must be 64 bytes");

// --- DANGO_CHANNEL_QUERY: version + ring-order query (spec §11.4) -----------

struct MxDangoChannelQueryRequest {
    mx_u32_t query_flags; // reserved; must be 0 today
    mx_u32_t channel_id;
};
static_assert(sizeof(MxDangoChannelQueryRequest) <= MX_RING_INLINE_PAYLOAD,
              "channel query request must fit one payload");
static_assert(sizeof(MxDangoChannelQueryRequest) == 8, "channel query request size drift");

struct MxDangoChannelQueryReply {
    mx_status_t status;
    mx_u16_t version_major;
    mx_u16_t version_minor;
    mx_u32_t ring_order;
    mx_u32_t flags;
    mx_u64_t boot_generation;
    mx_u32_t slot_count;
    mx_u32_t reserved0;
};
static_assert(sizeof(MxDangoChannelQueryReply) <= MX_RING_INLINE_PAYLOAD,
              "channel query reply must fit one payload");
static_assert(sizeof(MxDangoChannelQueryReply) == 40, "channel query reply size drift");

// --- Notification payloads (mirror the HVC argument shapes) -----------------

struct MxDangoChannelNotify {
    mx_u32_t channel_id;
    mx_u32_t reason;  // MxDangoChannelNotifyReason
    mx_u64_t counter; // observed ring counter published by the notifier
};
static_assert(sizeof(MxDangoChannelNotify) <= MX_RING_INLINE_PAYLOAD,
              "channel notify must fit one payload");
static_assert(sizeof(MxDangoChannelNotify) == 16, "channel notify size drift");

struct MxDangoChannelArm {
    mx_u32_t channel_id;
    mx_u32_t reserved0;
    mx_u64_t observed_counter; // ring counter the guest has consumed to
};
static_assert(sizeof(MxDangoChannelArm) <= MX_RING_INLINE_PAYLOAD,
              "channel arm must fit one payload");
static_assert(sizeof(MxDangoChannelArm) == 16, "channel arm size drift");

// The channel rings reuse the Midix ring slot verbatim.
using MxDangoChannelFrame = MxRingSlot;
static_assert(sizeof(MxDangoChannelFrame) == 64, "channel frame must be one ring slot");

// --- Guest -> host HVC function ids (spec §11.3) ----------------------------
//
// SMCCC assigns each SMC/HVC function id a service owner by range. PSCI lives in
// the Standard Secure Service range (0x8400_0000..0x8400_FFFF and
// 0xC400_0000..0xC400_FFFF). The Dango channel calls occupy the 64-bit Vendor
// Hypervisor Service range (0xC600_0000..0xC600_FFFF), which is disjoint from
// PSCI and reserved for exactly this kind of vendor-provided hypervisor service.
constexpr mx_u32_t MX_DANGO_HVC_BASE = 0xC600'0000u; // Vendor Hypervisor Service (64-bit)
constexpr mx_u32_t MX_DANGO_HVC_VENDOR_LIMIT = 0xC600'FFFFu;
constexpr mx_u32_t MX_PSCI_FUNC_BASE32 = 0x8400'0000u;
constexpr mx_u32_t MX_PSCI_FUNC_LIMIT32 = 0x8400'FFFFu;
constexpr mx_u32_t MX_PSCI_FUNC_BASE64 = 0xC400'0000u;
constexpr mx_u32_t MX_PSCI_FUNC_LIMIT64 = 0xC400'FFFFu;

constexpr mx_u32_t HVC_DANGO_CHANNEL_NOTIFY = DANGO_CH_HVC_NOTIFY;
constexpr mx_u32_t HVC_DANGO_CHANNEL_ARM = DANGO_CH_HVC_ARM;

constexpr bool mx_dango_hvc_avoid_psci(mx_u32_t fn) {
    const bool in_psci32 = fn >= MX_PSCI_FUNC_BASE32 && fn <= MX_PSCI_FUNC_LIMIT32;
    const bool in_psci64 = fn >= MX_PSCI_FUNC_BASE64 && fn <= MX_PSCI_FUNC_LIMIT64;
    return !in_psci32 && !in_psci64;
}
constexpr bool mx_dango_hvc_in_vendor_range(mx_u32_t fn) {
    return fn >= MX_DANGO_HVC_BASE && fn <= MX_DANGO_HVC_VENDOR_LIMIT;
}

static_assert(mx_dango_hvc_avoid_psci(HVC_DANGO_CHANNEL_NOTIFY),
              "HVC_DANGO_CHANNEL_NOTIFY must not collide with PSCI");
static_assert(mx_dango_hvc_avoid_psci(HVC_DANGO_CHANNEL_ARM),
              "HVC_DANGO_CHANNEL_ARM must not collide with PSCI");
static_assert(mx_dango_hvc_in_vendor_range(HVC_DANGO_CHANNEL_NOTIFY) &&
                  mx_dango_hvc_in_vendor_range(HVC_DANGO_CHANNEL_ARM),
              "Dango HVC ids must stay in the vendor hypervisor range");
static_assert(HVC_DANGO_CHANNEL_NOTIFY != HVC_DANGO_CHANNEL_ARM,
              "Dango HVC function ids must be unique");
static_assert(MX_PSCI_FUNC_LIMIT64 < MX_DANGO_HVC_BASE,
              "Dango HVC base must sit above the PSCI 64-bit range");
