#include "anko/pci.h"
#include "anko/arm64_esr.h"
#include <cassert>
#include <cstring>
#include <initializer_list>

using namespace pci;
static void put(u8* p, unsigned off, u32 v) {
    for (unsigned i = 0; i < 4; ++i) {
        p[off + i] = v >> (8 * i);
    }
}
static void vendor_cap(u8* host, u32 off, u8 type, u8 next, u32 start, u32 bytes) {
    host[off] = 9;
    host[off + 1] = next;
    host[off + 2] = type == 2 ? 20 : 16;
    host[off + 3] = type;
    host[off + 4] = 0;
    put(host, off + 8, start);
    put(host, off + 12, bytes);
}
int main() {
    GuestConfig cfg{};
    cfg.present[0] = true;
    cfg.fn_count = 1;
    put(cfg.space[0], 0, 0x10501af4);
    EcamAddress addr{};
    for (u32 off = 0; off < kGuestEcamSize; off += 4) {
        assert(decode_ecam(off, 4, &addr));
        assert(addr.assigned_device == (off >= 0x8000 && off < 0x10000));
        assert(addr.function == ((off >> 12) & 7));
        assert(addr.offset == (off & 0xfff));
    }
    assert(!decode_ecam(kGuestEcamSize, 4, &addr));
    assert(!decode_ecam(0x8001, 2, &addr));
    assert(!decode_ecam(0x8000, 8, &addr));
    assert(!decode_ecam(~0ull, 1, &addr));
    assert(!decode_ecam(0, 0, &addr));
    assert(!decode_ecam(0, 4, nullptr));
    assert(config_read(cfg, 0, 0, 4) == 0x10501af4);
    assert(config_read(cfg, 0, 1, 1) == 0x1a);
    assert(config_read(cfg, 0, 2, 2) == 0x1050);
    assert(config_read(cfg, 1, 0, 4) == 0xffffffff);
    assert(config_read(cfg, 0, 256, 4) == 0);
    assert(config_read(cfg, 0, 255, 2) == 0xffffffff);
    PciBar bars[kMaxBars]{};
    bars[0] = {0x10000000, 0x4000, BarType::Mmio64, true};
    assert(config_set_bar(cfg, 0, 0, 0x150000000ull, bars[0]));
    assert(!config_set_bar(cfg, 0, 1, 0x150000000ull, bars[0]));
    assert(config_read(cfg, 0, 0x10, 4) == 0x5000000c);
    assert(config_read(cfg, 0, 0x14, 4) == 1);
    assert(config_bar_write(cfg, 0, 0x10, 0xffffffff, 4));
    assert(config_bar_write(cfg, 0, 0x14, 0xffffffff, 4));
    assert(config_read(cfg, 0, 0x10, 4) == 0xffffc00c);
    assert(config_read(cfg, 0, 0x12, 2) == 0xffff);
    assert(config_read(cfg, 0, 0x14, 4) == 0xffffffff);
    assert(!config_bar_write(cfg, 0, 0x10, 0x6000000c, 4));
    assert(!config_bar_write(cfg, 0, 0x11, 0, 1));
    assert(config_bar_write(cfg, 0, 0x10, 0x5000000c, 4));
    assert(config_bar_write(cfg, 0, 0x14, 1, 4));
    assert(config_read(cfg, 0, 0x10, 4) == 0x5000000c);
    assert(config_bar_write(cfg, 0, 0x18, 0xffffffff, 4));
    assert(config_read(cfg, 0, 0x18, 4) == 0); // unimplemented BAR
    assert(config_bar_write(cfg, 0, 0x18, 0, 4));
    PciBar narrow{0x20000000, 0x1000, BarType::Mmio32, false};
    assert(!config_set_bar(cfg, 0, 2, 0x100000000ull, narrow));
    assert(!config_set_bar(cfg, 0, 2, 0x50000001, narrow));
    assert(config_set_bar(cfg, 0, 2, 0x50004000, narrow));
    assert(config_bar_write(cfg, 0, 0x18, 0xffffffff, 4));
    assert(config_read(cfg, 0, 0x18, 4) == 0xfffff000);

    for (u32 off = 0; off < 4096; ++off) {
        for (u32 width : {0u, 1u, 2u, 3u, 4u, 8u, 0xffffffffu}) {
            const auto action = classify_config_write(cfg, 0, off, width);
            const bool valid = (width == 1 || width == 2 || width == 4) && !(off & (width - 1));
            const bool command = valid && (off == 4 || off == 5);
            const bool benign =
                valid && ((off >= 12 && off + width <= 14) || (off == 60 && width == 1));
            assert(action == (command  ? CfgWriteAction::Mediate
                              : benign ? CfgWriteAction::Accept
                                       : CfgWriteAction::Reject));
            (void)config_read(cfg, 0, off, width);
        }
    }
    assert(classify_config_write(cfg, 0, ~0u, 4) == CfgWriteAction::Reject);

    u8 host[kCfgSpaceBytes]{};
    host[6] = 0x10;
    host[0x34] = 0x40;
    host[0x40] = 0x11;
    host[0x41] = 0x50; // hide MSI-X
    vendor_cap(host, 0x50, 1, 0x60, 0, 56);
    vendor_cap(host, 0x60, 2, 0x80, 0x1000, 0x1000);
    put(host, 0x70, 4);                 // notify multiplier
    vendor_cap(host, 0x80, 5, 0, 0, 4); // hide PCI_CFG indirect access
    assert(config_filter_virtio_caps(cfg, 0, host));
    assert(cfg.space[0][0x34] == 0x50 && cfg.space[0][0x51] == 0x60);
    assert(cfg.space[0][0x61] == 0 && cfg.space[0][0x40] == 0 && cfg.space[0][0x80] == 0);
    assert(cfg.msi_cap_off[0] == 0);
    const auto good = cfg;
    host[0x81] = 0x40; // cycle must fail without partial publication
    assert(!config_filter_virtio_caps(cfg, 0, host));
    assert(std::memcmp(&good, &cfg, sizeof(cfg)) == 0);
    host[0x81] = 0;
    put(host, 0x58, 0x3fff); // common config overruns BAR
    assert(!config_filter_virtio_caps(cfg, 0, host));
    put(host, 0x58, 0);
    host[0x51] = 0x54; // next header in retained payload
    assert(!config_filter_virtio_caps(cfg, 0, host));
    host[0x51] = 0x60;
    host[0x64] = 1; // upper BAR half
    assert(!config_filter_virtio_caps(cfg, 0, host));
    host[0x64] = 0;
    host[0x62] = 16; // truncated notify cap
    assert(!config_filter_virtio_caps(cfg, 0, host));
    host[0x62] = 20;
    for (u32 offset = 0; offset < 256; ++offset) {
        auto malformed = good;
        host[0x34] = offset;
        (void)config_filter_virtio_caps(malformed, 0, host);
    }

    std::memset(host, 0, sizeof(host));
    assert(bar_range_safe(host, bars, 0, 0, 4096));
    host[6] = 0x10;
    host[0x34] = 0x40;
    host[0x40] = 0x11;
    put(host, 0x44, 0x800);  // table in page zero
    put(host, 0x48, 0x2000); // PBA in page two
    assert(!bar_range_safe(host, bars, 0, 0, 4096));
    assert(bar_range_safe(host, bars, 0, 4096, 4096));
    assert(!bar_range_safe(host, bars, 0, 8192, 4096));
    assert(!bar_range_safe(host, bars, 0, 0, 0x4000));
    assert(!bar_range_safe(host, bars, 0, 0, ~0ull));
    put(host, 0x44, 7); // invalid BIR
    assert(!bar_range_safe(host, bars, 0, 4096, 4096));
    put(host, 0x44, 0x800);
    host[0x41] = 0x40;
    assert(!bar_range_safe(host, bars, 0, 4096, 4096));

    arm64::MmioAccess access{};
    const u64 syndrome = (arm64::kEcDataAbortLower << 26) | (1ull << 25) | (1ull << 24) | 7;
    assert(arm64::decode_mmio(syndrome, &access));
    assert(access.size == 1 && !access.write);
    assert(arm64::mmio_load_value(access, ~0ull) == 0xff);
    assert(arm64::decode_mmio(syndrome | (1ull << 21) | (1ull << 15), &access));
    assert(arm64::mmio_load_value(access, 0x80) == 0xffffffffffffff80ull);
    assert(arm64::decode_mmio(syndrome | (1ull << 21), &access));
    assert(arm64::mmio_load_value(access, 0x80) == 0xffffff80);
    for (u64 forbidden : {1ull << 7, 1ull << 8, 1ull << 10}) {
        assert(!arm64::decode_mmio(syndrome | forbidden, &access));
    }
    assert(!arm64::decode_mmio(syndrome & ~(1ull << 24), &access));
    assert(!arm64::decode_mmio((syndrome & ~63ull) | 0x10, &access));
    assert(!arm64::decode_mmio(syndrome | (1ull << 21) | (1ull << 6), &access));
}
