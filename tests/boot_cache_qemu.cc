// Execute the actual privileged setup/restore around the production parser.
#include "anko/boot_cache.h"
#include "anko/hw_info.h"
#include "libk/string.h"

HwInfo g_hw{};
extern "C" { u64 g_boot_el = 1; }
static volatile u64 witness;

[[noreturn]] static void finish(u64 code) {
    const u64 args[]{0x20026, code};
    register u64 operation asm("x0") = 0x20; // SYS_EXIT_EXTENDED
    register const u64* data asm("x1") = args;
    asm volatile("hlt #0xf000" : "+r"(operation) : "r"(data) : "memory");
    while (true) asm volatile("wfe");
}
static void require(bool condition, u64 code) { if (!condition) finish(code); }
static void registers(u64 (&state)[5]) {
    asm volatile("mrs %0, sctlr_el1; mrs %1, tcr_el1; mrs %2, mair_el1; "
                 "mrs %3, ttbr0_el1; mrs %4, ttbr1_el1"
                 : "=r"(state[0]), "=r"(state[1]), "=r"(state[2]),
                   "=r"(state[3]), "=r"(state[4]) :: "memory");
}
extern "C" void boot_cache_test(PhysAddr dtb) {
    u64 before[5], after[5];
    registers(before);
    require((before[0] & 5) == 0, 1);
    // Repeated entry catches stale tables and accidentally retained state.
    for (u32 i = 0; i < 3; ++i) {
        volatile u64 stack_witness = i;
        {
            mm::BootDtbCache cache(dtb);
            registers(after);
            require((after[0] & 0x1005) == 0x1005, 2);
            require(platform::probe_dtb(reinterpret_cast<const u8*>(dtb), 0, g_hw), 3);
            require(g_hw.ram_base == 0x40000000 && g_hw.console_base == 0x9000000, 4);
            witness = 0x1234567800000000ull + i;
            stack_witness = witness;
        }
        registers(after);
        require(libk::memcmp(before, after, sizeof(before)) == 0, 5);
        require(witness == 0x1234567800000000ull + i && stack_witness == witness, 6);
        require(g_hw.ram_base == 0x40000000 && g_hw.console_base == 0x9000000, 7);
    }
    {
        mm::BootDtbCache absent(0);
        registers(after);
        require(libk::memcmp(before, after, sizeof(before)) == 0, 8);
    }
    finish(0);
}
