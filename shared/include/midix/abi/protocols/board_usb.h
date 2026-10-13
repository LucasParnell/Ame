#pragma once
#include "midix/abi/protocols/usb_transfer.h"
#include "midix/abi/status.h"
// Cold service operations only. Distinct protocols keep power/PHY/role policy
// independent of the controller data plane and of HID/input consumers.
constexpr mx_u32_t MX_TYPEC_QUERY = 0x54594331, MX_TYPEC_REPLY = 0x54594332;
constexpr mx_u32_t MX_POWER_SET = 0x50575231, MX_POWER_REPLY = 0x50575232;
constexpr mx_u32_t MX_PHY_SET = 0x50485931, MX_PHY_REPLY = 0x50485932;
constexpr mx_u32_t MX_ROLE_SET = 0x524f4c31, MX_ROLE_REPLY = 0x524f4c32;
struct MxBoardRequest {
    mx_u64_t id;
    mx_u32_t value, reserved;
};
struct MxBoardReply {
    mx_u64_t id;
    mx_status_t status;
    mx_u32_t value, flags, reserved;
};
enum MxTypeCFlags : mx_u32_t {
    MX_TYPEC_ATTACHED = 1,
    MX_TYPEC_VBUS = 2,
    MX_TYPEC_CC1 = 4,
    MX_TYPEC_CC2 = 8
};
