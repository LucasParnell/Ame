// Execute production TTBR activation, ASID shootdowns, TCR setup and leaf
// attributes on an ARMv8.0 CPU. Unused VM/allocator paths are link-time GC'd.
#include "../kernel/mm/vm_space.cc"

HwInfo g_hw{};
struct alignas(4096) Tables { u64 root[512], l1[512], l2[512], l3[512]; } tables[3];
alignas(4096) static u64 payload[2][512];
static void (*volatile activate)(PhysAddr) = mm::vm_activate;

[[noreturn]] static void finish(u64 code) {
    const u64 args[]{0x20026, code};
    register u64 operation asm("x0") = 0x20;
    register const u64* data asm("x1") = args;
    asm volatile("hlt #0xf000" : "+r"(operation) : "r"(data) : "memory");
    for (;;) asm volatile("wfe");
}
static void require(bool value, u64 code) { if (!value) finish(code); }
static PhysAddr address(const void* pointer) { return reinterpret_cast<PhysAddr>(pointer); }

extern "C" void boot_cache_test(PhysAddr) {
    constexpr u64 va = 0x20000000;
    constexpr u64 first = 0x123456789abcdef0, second = 0xfedcba9876543210;
    payload[0][0] = first; payload[1][0] = second;
    const u64 attrs = mm::kernel_page_attrs(false, address(payload));
    require(attrs & mm::PTE_NG, 1);
    for (u32 i = 0; i < 3; ++i) {
        auto& t = tables[i];
        t.root[0] = address(t.l1) | mm::DESC_TABLE;
        // Identity-map RAM, code, stacks and tables with the production attrs.
        t.l1[1] = 0x40000000 | attrs | mm::PTE_VALID;
        t.l1[0] = address(t.l2) | mm::DESC_TABLE;
        t.l2[(va >> 21) & 511] = address(t.l3) | mm::DESC_TABLE;
        t.l3[(va >> 12) & 511] = address(payload[i == 2 ? 1 : 0]) | attrs | mm::DESC_PAGE;
    }
    asm volatile("dsb sy" ::: "memory");
    mm::enable_mmu(address(tables[0].root));
    auto* value = reinterpret_cast<volatile u64*>(va);
    require(*value == first, 2);
    for (u32 round = 0; round < 1000; ++round) {
        activate(mm::vm_context(address(tables[1].root), 1));
        require(*value == first, 3);
        activate(mm::vm_context(address(tables[2].root), 2));
        require(*value == second, 4);
    }
    // Reuse every process ASID for a different root, including cached aliases.
    for (u32 generation = 0; generation < 3; ++generation) {
        for (u32 pid = 1; pid <= 64; ++pid) {
            const bool odd = (generation + pid) & 1;
            mm::tlbi_space(pid);
            activate(mm::vm_context(address(tables[odd ? 2 : 1].root), pid));
            require(*value == (odd ? second : first), 5);
        }
    }
    finish(0);
}
