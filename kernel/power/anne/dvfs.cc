#include "dvfs.h"
#include "mailbox.h"
#include "anko/fdt.h"
#include "anko/log.h"
#include "libk/mmio.h"
#include "libk/string.h"
#include "libk/str.h"
namespace anne::dvfs {
namespace {
Mailbox mailbox;
bool wanted(const char* name, u32 length) {
    static constexpr libk::str::Literal names[] = {
        "compatible", "reg",     "unlock_key", "capability", "status",
        "src_bit",    "des_bit", "index",      "used",       "timeout",
    };
    return libk::str::one_of(name, length, names);
}
struct Probe {
    u32 depth, channel_depth, seen, channel_seen;
    bool ipc, channel, disabled, valid;
};
bool begin(void* ctx, const char* name, u32 depth) {
    auto& p = *static_cast<Probe*>(ctx);
    if (p.ipc && depth == p.depth + 1 && !libk::strcmp(name, "mailbox-13")) {
        p.channel = true;
        p.channel_depth = depth;
        p.channel_seen = 0;
    }
    return depth < 32;
}
void end(void* ctx, u32 depth) {
    auto& p = *static_cast<Probe*>(ctx);
    if (p.channel && depth == p.channel_depth) {
        p.channel = false;
        if (p.channel_seen == 31)
            p.seen |= 8;
    }
    if (p.ipc && depth == p.depth) {
        p.valid = !p.disabled && p.seen == 15;
        p.ipc = false;
    }
}
void property(void* ctx, const char* name, const u8* data, u32 len, u32 depth) {
    auto& p = *static_cast<Probe*>(ctx);
    if (!libk::strcmp(name, "compatible") && len == sizeof("hisilicon,HiIPCV230") &&
        !libk::memcmp(data, "hisilicon,HiIPCV230", len)) {
        p.ipc = true;
        p.depth = depth;
    }
    if (p.ipc && depth == p.depth) {
        if (!libk::strcmp(name, "reg") && len == 16 && !libk::be32(data) &&
            libk::be32(data + 4) == 0xe896b000 && !libk::be32(data + 8) &&
            libk::be32(data + 12) == 4096)
            p.seen |= 1;
        if (!libk::strcmp(name, "unlock_key") && len == 4 && libk::be32(data) == 0x1acce551)
            p.seen |= 2;
        if (!libk::strcmp(name, "capability") && len == 4 && libk::be32(data) == 8)
            p.seen |= 4;
        if (!libk::strcmp(name, "status") && !(len == 3 && !libk::memcmp(data, "ok", 3)) &&
            !(len == 5 && !libk::memcmp(data, "okay", 5)))
            p.disabled = true;
    }
    if (!p.channel || depth != p.channel_depth || len != 4)
        return;
    const u32 value = libk::be32(data);
    if (!libk::strcmp(name, "src_bit") && !value)
        p.channel_seen |= 1;
    if (!libk::strcmp(name, "des_bit") && value == 3)
        p.channel_seen |= 2;
    if (!libk::strcmp(name, "index") && value == 13)
        p.channel_seen |= 4;
    if (!libk::strcmp(name, "used") && value == 1)
        p.channel_seen |= 8;
    if (!libk::strcmp(name, "timeout") && value)
        p.channel_seen |= 16;
}
struct Io {
    libk::Mmio ipc{reinterpret_cast<volatile u8*>(0xe896b000), 4096};
    bool read32(u32 offset, unsigned* value) {
        return ipc.read32(offset, value);
    }
    bool write32(u32 offset, unsigned value) {
        return ipc.write32(offset, value);
    }
    bool frequency_index(unsigned cluster, unsigned& index) {
        const libk::Mmio sctrl{reinterpret_cast<volatile u8*>(0xfff0a000), 4096};
        u32 value = 0;
        if (!sctrl.read32(0x41c, &value))
            return false;
        index = (value >> (cluster * 4)) & 15;
        return true;
    }
};
} // namespace
bool init(PhysAddr dtb) {
    Probe probe{};
    mailbox = {};
    return dtb &&
           fdt::walk(reinterpret_cast<const u8*>(dtb),
                     {&probe, begin, end, property, nullptr, wanted}) &&
           probe.valid;
}
SysError request(u32 cluster, u32 khz, u32 index, u64 now) {
    Io io;
    const bool was_pending = mailbox.pending;
    const auto result = mailbox.request(io, cluster, khz, index, now);
    if (!was_pending && mailbox.pending && mailbox.completed[cluster] == 0)
        ANKO_LOGF_INFO("kernel/power", "dvfs load request cluster=%u khz=%u", cluster, khz);
    return result == Result::Done ? E_OK : result == Result::Pending ? E_BUSY : E_FAULT;
}
void statistics(u32 cluster, u32& transitions, u32& failures) {
    transitions = cluster < 2 ? mailbox.completed[cluster] : 0;
    failures = cluster < 2 ? mailbox.failures[cluster] : 0;
}
} // namespace anne::dvfs
