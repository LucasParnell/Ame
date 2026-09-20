// tests/smmuv3_probe.cc — host regression for the pure SMMUv3 encoders.
//
// Compiles anko/smmuv3_enc.h on the host and checks every record the backend
// hands to the hardware: the stage-2 stream-table entry, the default-deny
// entry, the command-queue records (SYNC / CFGI_STE / TLBI), the TLBI range
// NUM/SCALE encoding, and the queue index arithmetic.  These encodings are the
// contract the SMMUv3 backend relies on, so a drift here must fail before it
// can reach QEMU.

#include "anko/smmuv3_enc.h"

#include <cassert>
#include <cstdio>

using namespace iommu;
using namespace iommu::smmuv3;

namespace {

void test_ste_sizes_and_default_deny() {
    static_assert(sizeof(Ste) == 64);
    static_assert(sizeof(Cmd) == 16);
    static_assert(sizeof(Evt) == 32);

    const Ste invalid = make_ste_invalid();
    for (u32 i = 0; i < 16; ++i) {
        assert(invalid.w[i] == 0); // Valid = 0 -> C_BAD_STE, never bypass
    }
    assert((invalid.w[0] & 1u) == 0);

    const Ste abort = make_ste_abort();
    assert((abort.w[0] & 1u) == 1u);         // Valid
    assert(((abort.w[0] >> 1) & 0x7u) == 0); // Config = abort
}

void test_ste_stage2() {
    constexpr PhysAddr kRoot = 0x0000'0000'4142'6000ull;
    const Ste ste = make_ste_stage2(kRoot, 3, 25, kS2Sl0Level1, kS2Ps44, true);

    assert((ste.w[0] & 1u) == 1u);                       // Valid
    assert(((ste.w[0] >> 1) & 0x7u) == kCfgS2Translate); // Config = stage-2
    assert((ste.w[4] & 0xFFFFu) == 3u);                  // S2VMID
    assert((ste.w[5] & 0x3Fu) == 25u);                   // S2T0SZ
    assert(((ste.w[5] >> 6) & 0x3u) == kS2Sl0Level1);
    assert(((ste.w[5] >> 14) & 0x3u) == kS2Tg4Kb);
    assert(((ste.w[5] >> 16) & 0x7u) == kS2Ps44);
    assert(((ste.w[5] >> 19) & 1u) == 1u); // S2AA64
    assert(((ste.w[5] >> 26) & 1u) == 1u); // S2R (record faults)

    // TTB round-trips through the split low/high fields.
    const PhysAddr rebuilt = (static_cast<PhysAddr>(ste.w[6] & 0xFFFF'FFF0ull)) |
                             (static_cast<PhysAddr>(ste.w[7] & 0x000F'FFFFull) << 32);
    assert(rebuilt == kRoot);

    const Ste norec = make_ste_stage2(kRoot, 1, 25, kS2Sl0Level1, kS2Ps44, false);
    assert(((norec.w[5] >> 26) & 1u) == 0u);
}

void test_sync_and_cfgi() {
    const Cmd sync = make_cmd_sync();
    assert((sync.w[0] & 0xFFu) == kCmdSync);
    assert(sync.w[1] == 0 && sync.w[2] == 0 && sync.w[3] == 0);

    const Cmd cfgi = make_cmd_cfgi_ste(0x20);
    assert((cfgi.w[0] & 0xFFu) == kCmdCfgiSte);
    assert(cfgi.w[1] == 0x20);
}

void test_tlbi() {
    const Cmd vmall = make_cmd_tlbi_s12_vmall(7);
    assert((vmall.w[0] & 0xFFu) == kCmdTlbiS12Vmall);
    assert((vmall.w[1] & 0xFFFFu) == 7u);

    // A 4 KiB page range encodes TG = 4KB, the address in word2/3, and a
    // NUM/SCALE pair that covers at least the requested pages.
    const Cmd ipa = make_cmd_tlbi_s2_ipa(2, 0x1'2345'6000ull, 1);
    assert((ipa.w[0] & 0xFFu) == kCmdTlbiS2Ipa);
    assert(((ipa.w[0] >> 12) & 0x1Fu) == 0u); // NUM = 0 (1 page)
    assert(((ipa.w[0] >> 20) & 0x1Fu) == 0u); // SCALE = 0
    assert(((ipa.w[2] >> 10) & 0x3u) == 1u);  // TG = 4KB
    const u64 addr =
        (static_cast<u64>(ipa.w[3]) << 32) | (static_cast<u64>(ipa.w[2] & 0xFFFF'F000u));
    assert(addr == 0x1'2345'6000ull);

    // 512 pages -> SCALE 4, NUM 31 -> 32 << 4 = 512 pages covered.
    u32 num = 0;
    u32 scale = 0;
    range_num_scale(512, &num, &scale);
    assert(scale == 4u && num == 31u);
    assert(static_cast<u64>(num + 1) << scale >= 512u);

    // A large range never under-covers.
    const u32 cases[] = {1u, 2u, 31u, 32u, 33u, 1000u, 65536u};
    for (u32 pages : cases) {
        u32 n = 0;
        u32 s = 0;
        range_num_scale(pages, &n, &s);
        assert(n <= 31u && s <= 31u);
        assert((static_cast<u64>(n + 1) << s) >= pages);
    }
}

void test_queue_math() {
    constexpr u32 kLog2 = 8;
    assert(q_index(0, kLog2) == 0);
    assert(q_index(255, kLog2) == 255);
    assert(q_index(256, kLog2) == 0);
    assert(q_wrap_index(256, kLog2) == 256); // wrap bit set
    assert(q_wrap_index(511, kLog2) == 511);
    assert(q_wrap_index(512, kLog2) == 0);
    assert(q_entries(kLog2) == 256);
}

void test_evt_decode() {
    Evt e{};
    e.w[0] = kEvtFTranslation;      // type 0x10
    e.w[1] = 0x20;                  // SID
    e.w[3] = (1u << 3) | (1u << 7); // RNW (write), S2
    e.w[4] = 0x4142'4000u;          // ADDR low
    e.w[5] = 0;                     // ADDR high

    assert(evt_word0_type(e.w[0]) == kEvtFTranslation);
    assert(!evt_word0_overflow(e.w[0]));
    assert(evt_sid(e) == 0x20);
    assert(evt_rnw(e));
    assert(evt_s2(e));
    assert(evt_addr(e) == 0x4142'4000ull);
    assert(evt_class(e) == 0);

    Evt over{};
    over.w[0] = kEvtFPermission | kEvtOverflow;
    assert(evt_word0_overflow(over.w[0]));
    assert(evt_word0_type(over.w[0]) == kEvtFPermission);

    assert(evt_type_name(kEvtFTranslation)[0] == 't');
}

void test_idr_and_base_cfg() {
    const u32 idr1 = (16u << 0) | (19u << 16) | (19u << 21);
    assert(idr1_sid_size(idr1) == 16u);
    assert(idr1_eventq_size(idr1) == 19u);
    assert(idr1_cmdq_size(idr1) == 19u);
    assert(idr5_oas(0x74u) == 4u);

    const u32 cfg = make_strtab_base_cfg_linear(8);
    assert((cfg & kStrTabLog2SizeMask) == 8u);
    assert(((cfg >> kStrTabFmtShift) & 0x3u) == kStrTabFmtLinear);

    const u64 base = make_queue_base(0x0000'0041'4280'0000ull, 8);
    assert((base & ~kBaseAddrMask) == 8u);
    assert((base & kBaseAddrMask) == 0x0000'0041'4280'0000ull);
}

} // namespace

int main() {
    test_ste_sizes_and_default_deny();
    test_ste_stage2();
    test_sync_and_cfgi();
    test_tlbi();
    test_queue_math();
    test_evt_decode();
    test_idr_and_base_cfg();
    std::printf("smmuv3 encoder probe ok\n");
    return 0;
}
