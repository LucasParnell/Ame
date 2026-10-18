#include "anko/vm.h"

extern "C" u64 vm_el2_run(vm::El2RunState*);
extern "C" void test_simd_seed(u64, u64, u64);
extern "C" void test_simd_capture(fpsimd::State*);
extern "C" void simd_guest_no_fp();
alignas(4096) static u64 stage2[512];
alignas(16) static u8 guest_stacks[2][4096];
static vm::El2RunState states[2];
static fpsimd::State observed[2];

[[noreturn]] static void finish(u64 code) {
    const u64 args[]{0x20026, code};
    register u64 operation asm("x0") = 0x20;
    register const u64* data asm("x1") = args;
    asm volatile("hlt #0xf000" : "+r"(operation) : "r"(data) : "memory");
    for (;;) asm volatile("wfe");
}
static void require(bool condition, u64 code) { if (!condition) finish(code); }
static void check(const fpsimd::State& state, u64 seed, u32 fpcr, u32 fpsr, u64 code) {
    for (u32 i = 0; i < 32; ++i) {
        const auto expected = ((seed + i) & 255) * 0x0101010101010101ull;
        require(state.v[i][0] == expected && state.v[i][1] == expected, code);
    }
    require(state.fpcr == fpcr && state.fpsr == fpsr, code + 1);
}
static void guest(u64 id) {
    for (u32 round = 0;; ++round) {
        test_simd_capture(&observed[id]);
        test_simd_seed(128 + id * 32 + round, ((round + id) % 4) << 22, 1u << (round % 5));
        asm volatile("hvc #0" ::: "memory");
    }
}
extern "C" void simd_el2_test() {
    fpsimd::enable();
    // A single level-1 block maps the QEMU RAM identically for both guests.
    stage2[1] = 0x40000000ull | 1 | (15 << 2) | (3 << 6) | (3 << 8) | (1 << 10);
    for (u32 id = 0; id < 2; ++id) {
        auto& state = states[id];
        state.vttbr = reinterpret_cast<u64>(stage2) | (u64{id + 1} << 48);
        state.vtcr = 25 | (1 << 6) | (1 << 8) | (1 << 10) | (3 << 12) | (2 << 16);
        state.pc = reinterpret_cast<u64>(&guest);
        state.regs[0] = id;
        state.pstate = 0x3c5;
        state.guest_sctlr = 0x30d00800;
        state.guest_cpacr_el1 = 3 << 20;
        state.guest_sp_el1 = reinterpret_cast<u64>(guest_stacks[id] + sizeof(guest_stacks[id]));
        state.guest_cntp_cval = state.guest_cntv_cval = ~0ull;
        // Initial vCPU state is zero, just like freshly allocated production vCPUs.
    }
    for (u32 round = 0; round < 64; ++round) for (u32 id = 0; id < 2; ++id) {
        const auto host_seed = 1 + round;
        test_simd_seed(host_seed, (round % 4) << 22, 0x1f);
        vm_el2_run(&states[id]);
        fpsimd::State host;
        test_simd_capture(&host);
        check(host, host_seed, (round % 4) << 22, 0x1f, 10);
        require(states[id].exit_kind == 0 && states[id].exit_esr >> 26 == 0x16, 12);
        if (!round) {
            for (const auto& reg : observed[id].v) require(!reg[0] && !reg[1], 13);
            require(!observed[id].fpcr && !observed[id].fpsr, 14);
        } else {
            check(observed[id], 128 + id * 32 + round - 1,
                  ((round - 1 + id) % 4) << 22, 1u << ((round - 1) % 5), 15);
        }
        check(states[id].guest_fpsimd, 128 + id * 32 + round,
              ((round + id) % 4) << 22, 1u << (round % 5), 17);
    }
    // CPACR_EL1 is guest-controlled. EL2 must still preserve vector state when
    // the guest has disabled FP access for itself.
    states[0].pc = reinterpret_cast<u64>(&simd_guest_no_fp);
    states[0].guest_cpacr_el1 = 0;
    test_simd_seed(23, 1u << 22, 0x15);
    vm_el2_run(&states[0]);
    fpsimd::State host;
    test_simd_capture(&host);
    check(host, 23, 1u << 22, 0x15, 19);
    finish(0);
}
