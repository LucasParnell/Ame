#pragma once
// USB transfer ABI v1. Distinct from the legacy boot-report protocol.
#include "midix/abi/types.h"
#include "midix/abi/ring.h"

constexpr mx_u32_t MX_PROTOCOL_USB_TRANSFER = 0x55535431u; // UST1
constexpr mx_u16_t MX_USB_ABI_VERSION = 1;
using MxUsbToken = mx_u64_t;

enum class MxUsbSpeed : mx_u8_t { Low = 1, Full = 2, High = 3, Super = 4, SuperPlus = 5 };
enum class MxUsbTransferType : mx_u8_t { Control = 0, Isochronous = 1, Bulk = 2, Interrupt = 3 };
enum class MxUsbDirection : mx_u8_t { Out = 0, In = 1 };
enum class MxUsbRole : mx_u8_t { Detached = 0, Host = 1, Device = 2 };
enum class MxUsbStatus : mx_u32_t {
    Success = 0, Short = 1, Stall = 2, Cancelled = 3, Timeout = 4,
    Disconnected = 5, ControllerLost = 6, Invalid = 7, Denied = 8,
    Unsupported = 9, QueueFull = 10, IoError = 11,
};
enum MxUsbTransferMsg : mx_u32_t {
    MX_USB_TRANSFER_SUBMIT = 1, MX_USB_TRANSFER_CANCEL = 2,
    MX_USB_TRANSFER_COMPLETE = 3, MX_USB_TRANSFER_CONTROL = 4,
};
enum MxUsbTransferFlags : mx_u32_t {
    MX_USB_TRANSFER_ALLOW_SHORT = 1u << 0,
    MX_USB_TRANSFER_SEND_ZLP = 1u << 1,
};

struct MxUsbSetup {
    mx_u8_t request_type;
    mx_u8_t request;
    mx_u16_t value;
    mx_u16_t index;
    mx_u16_t length;
};
static_assert(sizeof(MxUsbSetup) == 8);

struct MxUsbSubmit {
    mx_u64_t request_id;
    MxUsbToken endpoint;
    MxUsbToken buffer;
    mx_u32_t offset;
    mx_u32_t length;
    mx_u64_t deadline_ns; // absolute monotonic time; 0 means no client deadline
    mx_u32_t flags;
    mx_u32_t reserved;
};
struct MxUsbCancel { mx_u64_t request_id; };
struct MxUsbCompletion {
    mx_u64_t request_id;
    MxUsbToken endpoint;
    MxUsbStatus status;
    mx_u32_t actual_length;
    mx_u32_t controller_status;
    mx_u32_t reserved;
};
// A control request uses a registered buffer slice; length is setup.length.
struct MxUsbControlSubmit {
    mx_u64_t request_id;
    MxUsbToken interface;
    MxUsbToken buffer;
    mx_u64_t deadline_ns;
    MxUsbSetup setup;
    mx_u32_t offset;
    mx_u32_t flags;
};
static_assert(sizeof(MxUsbSubmit) == MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxUsbControlSubmit) == MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxUsbCompletion) <= MX_RING_INLINE_PAYLOAD);
