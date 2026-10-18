#pragma once
#include "midix/abi/types.h"

namespace drivers::emmc {
using u8 = mx_u8_t;
using u16 = mx_u16_t;
using u32 = mx_u32_t;
using u64 = mx_u64_t;
using usize = mx_size_t;
constexpr u32 kSectorSize = 512;
constexpr u32 kMaxFrames = 32;
constexpr u32 kMaxUserSectors = 128;
constexpr u32 kDmaDataOffset = 4096;
constexpr u32 kDmaArenaSize = kDmaDataOffset + kMaxUserSectors * kSectorSize;
// Permanently pinned, contiguous, uniformly Normal Non-Cacheable memory. The
// caller owns its lifetime and must fence the controller before releasing it.
// The first page contains descriptors; remaining pages hold transfer data.
struct DmaArena {
    u8 *cpu{};
    u64 physical{}, size{};
};
struct Frame {
    u8 bytes[kSectorSize];
};
static_assert(sizeof(Frame) == 512);

enum class Error : u32 {
    Ok,
    Invalid,
    NotReady,
    Timeout,
    Controller,
    Card,
    Unsupported,
    RestoreFailed,
    Busy,
};
struct Failure {
    Error error{};
    u32 command{}, interrupt{}, card_status{};
};
// Widths are bytes (1, 2, 4). MMIO must preserve access width, especially CMD.
struct Io {
    void *context;
    u32 (*read)(void *, u32 offset, unsigned width);
    void (*write)(void *, u32 offset, unsigned width, u32 value);
    u64 (*now_us)(void *);
};
struct CardInfo {
    u32 sectors{};
    u8 ext_csd[512]{};
    u32 bus_width{1};
};

// Synchronous, allocation-free SDHCI PIO/ADMA2. The caller serializes every call,
// including the entire counter -> authenticated-write sequence. Never use in IRQ
// context. initialize() takes over an already powered/clocked boot controller.
class Driver {
  public:
    explicit Driver(Io io) : io_(io) {}
    Error initialize(u32 input_clock_hz, bool arasan_phy = false, u32 bus_width = 1);
    // Enable ADMA2 only after initialization. Invalid configuration never
    // changes the active configuration. Unsupported hosts retain PIO.
    Error enable_dma(DmaArena arena);
    bool dma_enabled() const { return dma_.cpu != nullptr; }
    bool ready() const { return ready_; }
    const CardInfo &info() const { return info_; }
    const Failure &failure() const { return failure_; }
    // User area only; count is 1..128. Buffers need not be aligned.
    Error read(u32 lba, u32 count, void *buffer);
    Error write(u32 lba, u32 count, const void *buffer);
    // Serialized block services batch writes behind an explicit BLK_FLUSH.
    // write() retains its durable-on-return contract for board diagnostics.
    Error write_buffered(u32 lba, u32 count, const void *buffer);
    Error flush();
    // Opaque RPMB frames. Types 1/2/3/4 cover key programming, counter,
    // authenticated write, and authenticated read. Key programming is exposed
    // only through the explicit provisioning path; normal request servicing
    // still rejects state RPMB_STATE_KEY.
    // Transport success preserves the card's result/MAC/nonce for its verifier.
    Error rpmb(const Frame *request, u32 request_count, Frame *response, u32 response_count);

  private:
    Io io_;
    CardInfo info_{};
    Failure failure_{};
    bool ready_{};
    u8 partition_{};
    DmaArena dma_{};
    u32 rd(u32 offset, unsigned width = 4) const;
    void wr(u32 offset, u32 value, unsigned width = 4);
    Error fail(Error error, u32 interrupt = 0, u32 status = 0);
    Error wait(u32 offset, unsigned width, u32 mask, u32 value, u64 timeout);
    Error interrupt(u32 mask, u64 timeout);
    Error reset(u8 mask);
    Error clock(u32 input, u32 target);
    void delay(u64 us);
    Error command(u32 index, u32 arg, u16 flags, u32 *response = nullptr);
    Error ready_status();
    Error select(u8 config);
    Error transfer(u32 index, u32 arg, u32 count, u8 *buffer, bool write);
    Error transfer_dma(u32 index, u32 arg, u32 count, u8 *buffer, bool write);
    Error blocks(u32 lba, u32 count, u8 *buffer, bool write);
    void recover();
};
} // namespace drivers::emmc
