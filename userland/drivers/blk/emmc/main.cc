// Native eMMC block service. The process owns the controller; io.fs receives
// only a bounded partition view and a shared transfer arena.
#include "midix/drivers/emmc.h"
#include "midix/abi/platform.h"
#include "mochi/driver.h"
#include "mochi/fs.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "libk/endian.h"
#include "libk/hash.h"

namespace {
using namespace drivers::emmc;
using namespace mochi::va::blk;
constexpr u64 kState = 0x23000000;
constexpr u64 kDma = 0x24000000;
constexpr u32 kMaxSectors = 128;
struct Runtime {
    Driver driver;
    volatile u8* mmio;
    u8* buffer;
    u64 buffer_size, first, sectors;
    bool mounted, writable;
    alignas(4) u8 sector[512];
    alignas(4) u8 entries[65536];
};
u32 read_reg(void* context, u32 offset, unsigned width) {
    const auto* p = static_cast<Runtime*>(context)->mmio + offset;
    // The FIFO is Device memory and its word accesses remain ordered. The
    // status polls on both sides of every sector provide the outer-shareable
    // barriers; avoid two expensive DMBs for each of its 128 words. PIO uses
    // coherent CPU mappings and needs no non-coherent DMA cache maintenance.
    if (offset == 0x20 && width == 4) return *reinterpret_cast<const volatile u32*>(p);
    asm volatile("dmb osh" ::: "memory");
    u32 value = width == 1 ? *p : width == 2 ? *reinterpret_cast<const volatile u16*>(p)
                                           : *reinterpret_cast<const volatile u32*>(p);
    asm volatile("dmb osh" ::: "memory");
    return value;
}
void write_reg(void* context, u32 offset, unsigned width, u32 value) {
    auto* p = static_cast<Runtime*>(context)->mmio + offset;
    if (offset == 0x20 && width == 4) { *reinterpret_cast<volatile u32*>(p) = value; return; }
    asm volatile("dmb osh" ::: "memory");
    if (width == 1) *p = u8(value);
    else if (width == 2) *reinterpret_cast<volatile u16*>(p) = u16(value);
    else *reinterpret_cast<volatile u32*>(p) = value;
    asm volatile("dmb osh" ::: "memory");
}
u64 now_us(void*) { return mx_clock_ns() / 1000; }
void note(const char* name, u64 first, u64 sectors) {
    char line[128];
    libk::snprintf(line, sizeof(line), "drv.blk.emmc: %s lba=%llu sectors=%llu\n", name,
                  static_cast<unsigned long long>(first), static_cast<unsigned long long>(sectors));
    mochi::log_raw_marker(line);
}
bool name_eq(const u8* utf16, const char* name) {
    for (unsigned i = 0; i < 36; ++i) {
        if (utf16[2*i] != u8(name[i]) || utf16[2*i+1]) return false;
        if (!name[i]) return true;
    }
    return false;
}
bool read_sectors(Runtime& r, u32 lba, u32 count, u8* out) {
    while (count) {
        const u32 chunk = count < kMaxUserSectors ? count : kMaxUserSectors;
        if (r.driver.read(lba, chunk, out) != Error::Ok) return false;
        lba += chunk; count -= chunk; out += chunk * 512;
    }
    return true;
}
bool select_partition(Runtime& r, u32 selected) {
    if (!read_sectors(r, 1, 1, r.sector)) return false;
    constexpr char signature[] = "EFI PART";
    for (unsigned i = 0; i < 8; ++i) if (r.sector[i] != u8(signature[i])) return false;
    const auto header_size = libk::load_le<u32>(r.sector + 12);
    const auto expected_crc = libk::load_le<u32>(r.sector + 16);
    if (header_size < 92 || header_size > 512) return false;
    for (unsigned i = 16; i < 20; ++i) r.sector[i] = 0;
    if (libk::crc32_ieee(libk::as_bytes(r.sector, header_size)) != expected_crc) return false;
    const auto entries_lba = libk::load_le<u64>(r.sector + 72);
    const auto count = libk::load_le<u32>(r.sector + 80);
    const auto stride = libk::load_le<u32>(r.sector + 84);
    const auto entries_crc = libk::load_le<u32>(r.sector + 88);
    const auto usable_first = libk::load_le<u64>(r.sector + 40);
    const auto usable_last = libk::load_le<u64>(r.sector + 48);
    const u64 bytes = u64(count) * stride;
    const u64 sectors = (bytes + 511) / 512;
    const u64 capacity = r.driver.info().sectors;
    if (!count || stride < 128 || stride > 512 || bytes > sizeof(r.entries) || !entries_lba ||
        entries_lba >= capacity || sectors > capacity - entries_lba || usable_first > usable_last ||
        usable_last >= capacity || selected > count) return false;
    if (!read_sectors(r, u32(entries_lba), u32(sectors), r.entries)) return false;
    if (libk::crc32_ieee(libk::as_bytes(r.entries, usize(bytes))) != entries_crc) return false;
    bool found = false;
    for (u32 i = 0; i < count; ++i) {
        const auto* entry = r.entries + i * stride;
        u8 type = 0;
        for (unsigned b = 0; b < 16; ++b) type |= entry[b];
        if (!type) continue;
        const u64 first = libk::load_le<u64>(entry + 32), last = libk::load_le<u64>(entry + 40);
        if (first < usable_first || last > usable_last || last < first) return false;
        if (name_eq(entry + 56, "cache")) note("cache", first, last - first + 1);
        if ((selected && i + 1 == selected) || (!selected && name_eq(entry + 56, "AMEROOT"))) {
            if (found) return false;
            r.first = first; r.sectors = last - first + 1; found = true;
        }
    }
    if (!found) return false;
    note("selected", r.first, r.sectors);
    // Writes are permitted only after an AmeFS volume was explicitly installed.
    // Neither startup nor an invalid mount formats arbitrary Android storage.
    if (!read_sectors(r, u32(r.first), 1, r.sector)) return false;
    r.writable = libk::load_le<u32>(r.sector) == FS_MAGIC &&
                 libk::load_le<u32>(r.sector + 4) == FS_SUPERBLOCK_VERSION &&
                 libk::load_le<u64>(r.sector + 16) == FS_BLOCK_SIZE &&
                 libk::load_le<u64>(r.sector + 8) <= r.sectors / (FS_BLOCK_SIZE / 512);
    return true;
}
mx_i32_t status_of(Error error) {
    if (error == Error::Ok) return MX_OK;
    if (error == Error::Timeout) return MX_ERR_WOULD_BLOCK;
    if (error == Error::Invalid) return MX_ERR_INVALID;
    return MX_ERR_FAULT;
}
void handle(Runtime& r, const mochi::fs::BlkServer& server, const RingSlot& slot) {
    BlkRequest request{};
    if (!mochi::message_get(slot, &request, slot.type) || request.type != slot.type ||
        request.flags || request.reserved || request.reserved2) {
        server.send_reply(MX_ERR_BAD_PROTOCOL, 0); return;
    }
    if (!r.mounted) { server.send_reply(MX_ERR_SERVICE_UNAVAILABLE, 0); return; }
    if (slot.type == BLK_FLUSH) {
        if (request.sector || request.sector_count || request.arena_cap || request.arena_offset) {
            server.send_reply(MX_ERR_INVALID, 0); return;
        }
        server.send_reply(status_of(r.driver.flush()), 0); return;
    }
    if (slot.type != BLK_READ && slot.type != BLK_WRITE) { server.send_reply(MX_ERR_NOT_IMPLEMENTED, 0); return; }
    const u64 bytes = u64(request.sector_count) * 512;
    if (!request.sector_count || request.sector_count > kMaxSectors ||
        request.sector >= r.sectors || request.sector_count > r.sectors - request.sector ||
        request.arena_cap || request.arena_offset < BLK_DMA_CLIENT_OFFSET || request.arena_offset > r.buffer_size ||
        bytes > r.buffer_size - request.arena_offset) { server.send_reply(MX_ERR_INVALID, 0); return; }
    if (slot.type == BLK_WRITE && !r.writable) { server.send_reply(MX_ERR_PERM, 0); return; }
    Error result = Error::Ok;
    u32 completed = 0;
    while (completed < request.sector_count) {
        const u32 left = request.sector_count - completed;
        const u32 chunk = left < kMaxUserSectors ? left : kMaxUserSectors;
        const u32 lba = u32(r.first + request.sector + completed);
        auto* buffer = r.buffer + request.arena_offset + completed * 512;
        result = slot.type == BLK_READ ? r.driver.read(lba, chunk, buffer) : r.driver.write_buffered(lba, chunk, buffer);
        if (result != Error::Ok) break;
        completed += chunk;
    }
    server.send_reply(status_of(result), completed);
}
}
extern "C" [[noreturn]] void _start() {
    constexpr auto name = "drv.blk.emmc";
    const auto* boot = mochi::boot::map(kVaBootInfo);
    mochi::log_map_from_boot(boot, kVaLogRing, name);
    auto state = mochi::make_state(kState, 128 * 1024);
    auto* runtime = state.alloc<Runtime>();
    if (!runtime) mochi::driver::startup_failed(name, "state", MX_ERR_NOMEM);
    const auto control = mochi::boot::require_handle(boot, "emmc.control");
    const auto claimed = mx_platform_control(control, MX_PLATFORM_CLAIM);
    if (claimed != MX_OK) mochi::driver::startup_failed(name, "controller ownership", claimed);
    const auto mmio = mochi::boot::require_handle(boot, "emmc.mmio");
    auto mapped = mx_map_mmio(mmio, kVaVirtioMmio, MX_RIGHT_READ | MX_RIGHT_WRITE);
    if (mapped != MX_OK) mochi::driver::startup_failed(name, "MMIO", mapped);
    runtime->mmio = reinterpret_cast<volatile u8*>(kVaVirtioMmio);
    runtime->driver = Driver({runtime, read_reg, write_reg, now_us});
    const auto initialized = runtime->driver.initialize(200000000, true,
        mochi::driver::value(boot, "emmc.bus_width"));
    if (initialized != Error::Ok) mochi::driver::startup_failed(name, "card initialization", status_of(initialized));
    note("bus width", runtime->driver.info().bus_width, 0);
    const auto dma = mochi::boot::require_handle(boot, "emmc.dma");
    MxCapInfo dma_info{};
    if (mx_cap_info(dma, &dma_info) != MX_OK || dma_info.type != MX_CAP_ARENA ||
        dma_info.attr != MX_ARENA_MEMORY_NONCACHEABLE || dma_info.size < kDmaArenaSize ||
        mx_map_page(dma, kDma, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK)
        mochi::driver::startup_failed(name, "DMA arena");
    const auto dma_status = runtime->driver.enable_dma({reinterpret_cast<u8*>(kDma), dma_info.base, dma_info.size});
    if (dma_status != Error::Ok && dma_status != Error::Unsupported)
        mochi::driver::startup_failed(name, "ADMA2 configuration", status_of(dma_status));
    mochi::log_raw_marker(runtime->driver.dma_enabled() ? "drv.blk.emmc: ADMA2 32-bit, uncached 64 KiB arena\n"
                                                       : "drv.blk.emmc: ADMA2 unavailable; PIO\n");
    const auto buffer = mochi::boot::require_handle(boot, "blk.buffer");
    MxCapInfo buffer_info{};
    if (mx_cap_info(buffer, &buffer_info) != MX_OK || buffer_info.size < BLK_DMA_CLIENT_OFFSET + 65536 ||
        mx_map_page(buffer, kVaVirtioDma, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK)
        mochi::driver::startup_failed(name, "transfer arena");
    runtime->buffer = reinterpret_cast<u8*>(kVaVirtioDma);
    runtime->buffer_size = buffer_info.size;
    runtime->mounted = select_partition(*runtime, mochi::driver::value(boot, "emmc.partition"));
    mochi::log_raw_marker(runtime->mounted ? (runtime->writable ? "drv.blk.emmc: AmeFS partition ready\n"
        : "drv.blk.emmc: selected partition is read-only; no AmeFS installed\n")
        : "drv.blk.emmc: no selected AmeFS partition\n");
    const mochi::fs::BlkServer server{mochi::boot::map_endpoint(boot,
        {"blk.reply.ring", kVaReplyRing, "blk.reply.doorbell"},
        {"blk.req.ring", kVaReqRing, "blk.req.doorbell"})};
    if (!server.valid()) mochi::driver::startup_failed(name, "block endpoint");
    mochi::boot::ready(boot);
    for (;;) {
        RingSlot slot{};
        if (server.wait_recv(&slot)) handle(*runtime, server, slot);
        else mx_yield();
    }
}
