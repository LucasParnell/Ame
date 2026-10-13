#pragma once
// DesignWare USB 2 OTG register encodings, without register access or policy.
// References: Hi6250 vendor dwc_otg_regs.h at
// 65e545cbaa11d132e243287b150146bc2a51c45e and Linux v6.12 dwc2/hw.h.
// The host and device banks are mutually exclusive views of the same core.
#include "midix/abi/protocols/usb_transfer.h"

namespace midix::usb::dwc2 {

constexpr mx_u32_t kAhbConfig = 0x008, kUsbConfig = 0x00c, kReset = 0x010;
constexpr mx_u32_t kInterruptStatus = 0x014, kInterruptMask = 0x018;
constexpr mx_u32_t kRxStatusPop = 0x020, kRxFifoSize = 0x024;
constexpr mx_u32_t kNonperiodicTxSize = 0x028, kNonperiodicTxStatus = 0x02c;
constexpr mx_u32_t kCoreId = 0x040, kHardware1 = 0x044, kHardware2 = 0x048;
constexpr mx_u32_t kHardware3 = 0x04c, kHardware4 = 0x050;
constexpr mx_u32_t kPeriodicTxSize = 0x100;
constexpr mx_u32_t kHostConfig = 0x400, kHostFrameInterval = 0x404;
constexpr mx_u32_t kHostFrame = 0x408, kPeriodicTxStatus = 0x410;
constexpr mx_u32_t kHostInterrupts = 0x414, kHostInterruptMask = 0x418;
constexpr mx_u32_t kHostPort = 0x440, kChannelBase = 0x500, kChannelStride = 0x20;
constexpr mx_u32_t kDeviceConfig = 0x800, kDeviceControl = 0x804, kDeviceStatus = 0x808;
constexpr mx_u32_t kDeviceInMask = 0x810, kDeviceOutMask = 0x814;
constexpr mx_u32_t kDeviceInterrupts = 0x818, kDeviceInterruptMask = 0x81c;
constexpr mx_u32_t kInEndpointBase = 0x900, kOutEndpointBase = 0xb00;
constexpr mx_u32_t kEndpointStride = 0x20, kFifoBase = 0x1000, kFifoStride = 0x1000;
constexpr mx_u32_t kOtgControl = 0x000, kPowerClock = 0xe00;
constexpr mx_u32_t kPhyStatus = 0x044; // AHB bc_sts2: vbus/session/dp/dm/id

constexpr mx_u32_t kGlobalInterruptEnable = 1u << 0, kDmaEnable = 1u << 5;
constexpr mx_u32_t kHostMode = 1u << 0, kSof = 1u << 3, kRxAvailable = 1u << 4;
constexpr mx_u32_t kPortInterrupt = 1u << 24, kChannelInterrupt = 1u << 25;
constexpr mx_u32_t kAhbIdle = 1u << 31, kSoftReset = 1u << 0;
constexpr mx_u32_t kRxFlush = 1u << 4, kTxFlush = 1u << 5;

constexpr mx_u32_t kPortConnected = 1u << 0, kPortEnabled = 1u << 2;
constexpr mx_u32_t kPortOvercurrent = 1u << 4, kPortResume = 1u << 6;
constexpr mx_u32_t kPortSuspend = 1u << 7, kPortReset = 1u << 8, kPortPower = 1u << 12;
constexpr mx_u32_t kPortChanges = (1u << 1) | (1u << 3) | (1u << 5);
constexpr mx_u32_t kPortControls = kPortResume | kPortSuspend | kPortReset | kPortPower;

constexpr mx_u32_t kChannelComplete = 1u << 0, kChannelHalted = 1u << 1;
constexpr mx_u32_t kChannelAhbError = 1u << 2, kChannelStall = 1u << 3;
constexpr mx_u32_t kChannelNak = 1u << 4, kChannelAck = 1u << 5, kChannelNyet = 1u << 6;
constexpr mx_u32_t kChannelTransactionError = 1u << 7, kChannelBabble = 1u << 8;
constexpr mx_u32_t kChannelFrameOverrun = 1u << 9, kChannelToggleError = 1u << 10;
constexpr mx_u32_t kChannelOwnedInterrupts = 0x7ff;
constexpr mx_u32_t kChannelDisable = 1u << 30, kChannelEnable = 1u << 31;
constexpr mx_u32_t kCharacteristics = 0x00, kSplit = 0x04, kChannelStatus = 0x08;
constexpr mx_u32_t kChannelMask = 0x0c, kTransferSize = 0x10;

enum class Pid : mx_u8_t { Data0 = 0, Data2 = 1, Data1 = 2, Setup = 3 };
enum class Architecture : mx_u8_t { Slave = 0, ExternalDma = 1, InternalDma = 2 };

struct Geometry {
    mx_u32_t core_id;
    mx_u32_t endpoint_directions;
    mx_u32_t max_transfer_bytes;
    mx_u16_t fifo_words;
    mx_u16_t max_packets;
    mx_u8_t host_channels;
    mx_u8_t device_endpoints; // EP0 plus nonzero endpoints
    mx_u8_t in_endpoints;    // nonzero IN endpoints
    mx_u8_t high_speed_phy;
    mx_u8_t utmi_width; // GHWCFG4 encoding: 0=8, 1=16, 2=either
    Architecture architecture;
    bool host, device, dynamic_fifo, dedicated_fifos;
};

inline bool decode_geometry(mx_u32_t id, mx_u32_t hw1, mx_u32_t hw2,
                            mx_u32_t hw3, mx_u32_t hw4, Geometry* out) {
    if (!out || ((id & 0xfffff000u) != 0x4f542000u &&
                 (id & 0xfffff000u) != 0x4f543000u)) return false;
    const auto mode = hw2 & 7u, architecture = (hw2 >> 3) & 3u;
    const auto transfer_width = (hw3 & 15u) + 11u;
    const auto packet_width = ((hw3 >> 4) & 7u) + 4u;
    const auto fifo_words = hw3 >> 16;
    if (mode > 6 || architecture > 2 || transfer_width > 19 || packet_width > 10 ||
        fifo_words < 16) return false;
    Geometry geometry{};
    geometry.core_id = id;
    geometry.endpoint_directions = hw1;
    geometry.max_transfer_bytes = (1u << transfer_width) - 1;
    geometry.max_packets = static_cast<mx_u16_t>((1u << packet_width) - 1);
    geometry.fifo_words = static_cast<mx_u16_t>(fifo_words);
    geometry.host_channels = static_cast<mx_u8_t>(1 + ((hw2 >> 14) & 15u));
    geometry.device_endpoints = static_cast<mx_u8_t>(1 + ((hw2 >> 10) & 15u));
    geometry.in_endpoints = static_cast<mx_u8_t>((hw4 >> 26) & 15u);
    geometry.utmi_width = static_cast<mx_u8_t>((hw4 >> 14) & 3u);
    if(geometry.utmi_width==3) return false;
    geometry.high_speed_phy = static_cast<mx_u8_t>((hw2 >> 6) & 3u);
    geometry.architecture = static_cast<Architecture>(architecture);
    geometry.host = mode <= 2 || mode >= 5;
    geometry.device = mode <= 4;
    geometry.dynamic_fifo = (hw2 & (1u << 19)) != 0;
    geometry.dedicated_fifos = (hw4 & (1u << 25)) != 0;
    if (geometry.in_endpoints >= geometry.device_endpoints) return false;
    *out = geometry;
    return true;
}

struct FifoRegion { mx_u16_t start, words; };
struct HostFifos { FifoRegion rx, nonperiodic, periodic; };
inline bool valid_region(FifoRegion region, mx_u32_t capacity) {
    return region.words >= 16 && region.start <= capacity &&
           region.words <= capacity - region.start;
}
inline bool disjoint(FifoRegion a, FifoRegion b) {
    return static_cast<mx_u32_t>(a.start) + a.words <= b.start ||
           static_cast<mx_u32_t>(b.start) + b.words <= a.start;
}
inline bool valid_host_fifos(const HostFifos& layout, mx_u32_t capacity) {
    return layout.rx.start == 0 && valid_region(layout.rx, capacity) &&
           valid_region(layout.nonperiodic, capacity) && valid_region(layout.periodic, capacity) &&
           disjoint(layout.rx, layout.nonperiodic) && disjoint(layout.rx, layout.periodic) &&
           disjoint(layout.nonperiodic, layout.periodic);
}
inline mx_u32_t fifo_size(FifoRegion region) {
    return region.start | (static_cast<mx_u32_t>(region.words) << 16);
}

// HPRT.PRTENA is write-one-to-disable, and change flags are write-one-to-clear.
// A generic read/modify/write would disable the port or lose unrelated changes.
inline bool port_write(mx_u32_t observed, mx_u32_t set, mx_u32_t clear,
                       mx_u32_t acknowledge, mx_u32_t* out) {
    if (!out || ((set | clear) & ~kPortControls) || (set & clear) ||
        (acknowledge & ~kPortChanges)) return false;
    *out = (((observed & kPortControls) | set) & ~clear) | acknowledge;
    return true;
}
inline MxUsbSpeed port_speed(mx_u32_t port) {
    const auto speed = (port >> 17) & 3u;
    return speed == 0 ? MxUsbSpeed::High : speed == 1 ? MxUsbSpeed::Full :
           speed == 2 ? MxUsbSpeed::Low : static_cast<MxUsbSpeed>(0);
}
inline mx_u32_t channel_register(mx_u32_t channel, mx_u32_t reg) {
    return kChannelBase + channel * kChannelStride + reg;
}
inline mx_u32_t fifo_register(mx_u32_t fifo) { return kFifoBase + fifo * kFifoStride; }

} // namespace midix::usb::dwc2
