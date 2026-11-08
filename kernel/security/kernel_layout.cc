// Early shared TTBR1 image mapping. Runs with IRQs masked before constructors.
#include "anko/mm.h"
#include "anko/random.h"
#include "libk/string.h"
extern "C" char __text_start[], __text_end[], __rodata_start[], __rodata_end[], __kernel_end[];
extern "C" unsigned char kaslr_table[];
namespace {
constexpr u64 page = 4096, mask = 0x0000fffffffff000ull;
alignas(4096) u64 early_tables[64][512];
u64 used;
u64 early_low_root, early_high_root;
u64 image_delta;
[[noreturn]] void stop() { for (;;) asm volatile("wfe"); }
u64* table() { if (used == 64) stop(); auto* p = early_tables[used++]; libk::memset(p, 0, page); return p; }
void map(u64* root, u64 va, u64 pa, u64 attributes) {
    auto* node = root;
    for (unsigned level = 0; level < 3; ++level) {
        auto& entry = node[(va >> (39 - 9 * level)) & 511];
        if (!entry) entry = reinterpret_cast<u64>(table()) | 3;
        node = reinterpret_cast<u64*>(entry & mask);
    }
    node[(va >> 12) & 511] = pa | attributes | 3;
}
void enable() {
    constexpr u64 mair = 0x44'00'ff; // Normal WB, Device, Normal NC
    constexpr u64 tcr = 16 | (1ull << 8) | (1ull << 10) | (3ull << 12) |
        (16ull << 16) | (1ull << 24) | (1ull << 26) | (3ull << 28) | (2ull << 30) | (2ull << 32);
    asm volatile("dsb sy; msr mair_el1, %0; msr tcr_el1, %1; msr ttbr0_el1, %2; msr ttbr1_el1, %3;"
                 "isb; tlbi vmalle1; dsb sy; isb; mrs x9, sctlr_el1; orr x9, x9, #1;"
                 "orr x9, x9, #4; orr x9, x9, #0x1000; msr sctlr_el1, x9; isb"
                 :: "r"(mair), "r"(tcr), "r"(early_low_root), "r"(early_high_root) : "x9", "memory");
}
}
extern "C" u64 kernel_bootstrap_virtual(u64 dtb) {
    const u64 physical = reinterpret_cast<u64>(__text_start);
    const u64 end = reinterpret_cast<u64>(__kernel_end);
    // Linux Image header and file positions remain physical; code/data pointers
    // are linked high and rebased using the audited post-link relocation table.
    struct Header { u32 magic, count; u64 linked, bytes; } header;
    libk::memcpy(&header, kaslr_table, sizeof(header));
    if (header.magic != 0x4b41534c || header.count > 8192 || header.bytes != end - physical) stop();
    // C/C++ constant tables (including crypto constants and weak-symbol GOT
    // entries) must be usable during the MMU-off entropy collection too.
    for (u32 i = 0; i < header.count; ++i) {
        u32 record[2]; libk::memcpy(record, kaslr_table + sizeof(header) + i * 8, 8);
        if ((record[1] != 8 && record[1] != 4) || record[0] > header.bytes - record[1]) stop();
        u64 value = 0; libk::memcpy(&value, reinterpret_cast<void*>(physical + record[0]), record[1]);
        value += physical - header.linked;
        libk::memcpy(reinterpret_cast<void*>(physical + record[0]), &value, record[1]);
    }
    const bool entropy = security::seed_boot_random(dtb);
    const u64 virtual_base = 0xffff800000000000ull + (entropy ? security::random_below(1ull << 19) : 0) * 0x200000;
    image_delta = virtual_base - physical;
    const u64 rebase = virtual_base - physical;
    for (u32 i = 0; i < header.count; ++i) {
        u32 record[2]; libk::memcpy(record, kaslr_table + sizeof(header) + i * 8, 8);
        if ((record[1] != 8 && record[1] != 4) || record[0] > header.bytes - record[1]) stop();
        u64 value = 0; libk::memcpy(&value, reinterpret_cast<void*>(physical + record[0]), record[1]);
        value += rebase;
        if (record[1] == 4 && value > 0xffffffff) stop();
        libk::memcpy(reinterpret_cast<void*>(physical + record[0]), &value, record[1]);
    }
    used = 0; auto* low = table(); auto* high = table();
    early_low_root = reinterpret_cast<u64>(low); early_high_root = reinterpret_cast<u64>(high);
    auto* l1 = table(); low[0] = reinterpret_cast<u64>(l1) | 3;
    // Temporary bootstrap identity map. Permanent TTBR0 roots remove execute
    // permission from the physical image after all CPUs enter through TTBR1.
    for (u64 gigabyte = 0; gigabyte < 4; ++gigabyte) {
        auto* l2 = table(); l1[gigabyte] = reinterpret_cast<u64>(l2) | 3;
        for (u64 n = 0; n < 512; ++n) {
            const u64 pa = (gigabyte << 30) | (n << 21);
            bool normal = (pa <= physical && physical - pa < 0x200000) ||
                (pa >= (physical & ~u64{0x1fffff}) && pa < end);
#ifdef ANKO_BOARD_HUAWEI_ANNE
            normal |= pa < 0x08000000;
#else
            normal |= pa >= (physical & ~u64{0x3fffffff}) && pa < (physical & ~u64{0x3fffffff}) + 0x20000000;
#endif
            l2[n] = pa | 1 | (1ull << 10) | (1ull << 54) |
                (normal ? (3ull << 8) : (1ull << 2) | (1ull << 53));
        }
    }
    const u64 text_end = reinterpret_cast<u64>(__text_end);
    const u64 rodata_start = reinterpret_cast<u64>(__rodata_start), rodata_end = reinterpret_cast<u64>(__rodata_end);
    for (u64 pa = physical; pa < end; pa += page) {
        u64 attributes = (1ull << 10) | (3ull << 8) | (1ull << 54);
        if (pa < text_end) attributes |= 1ull << 7;
        else attributes |= 1ull << 53;
        if (pa >= rodata_start && pa < rodata_end) attributes |= 1ull << 7;
        map(high, pa + image_delta, pa, attributes);
    }
    mm::kernel_image_delta = image_delta;
    mm::kernel_image_physical = physical;
    mm::kernel_image_bytes = end - physical;
    mm::kernel_ttbr1 = early_high_root;
    // Instructions may contain absolute literals patched above. Complete both
    // cache sides before either virtual alias is used for instruction fetch.
    asm volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
    enable(); return image_delta;
}
extern "C" u64 kernel_bootstrap_secondary() { enable(); return image_delta; }
