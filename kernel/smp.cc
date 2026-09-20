// kernel/smp.cc — minimal QEMU virt SMP bring-up and IPI handling.

#include "anko/smp.h"
#include "anko/mm.h"
#include "anko/hw_info.h"
#include "anko/platform.h"
#include "anko/sched.h"
#include "anko/log.h"
#include "libk/printf.h"
#include "libk/string.h"
#include "anko/fdt.h"

namespace drivers::gic { void init_cpu(); void send_sgi(u32 cpu_id, u32 sgi_id); }
namespace drivers::timer { void init_cpu(u32 irq_id); }

extern "C" void vectors();
extern "C" void secondary_start();
extern "C" u64 g_boot_el;
extern "C" u64 psci_hvc_call(u64 fn, u64 a0, u64 a1, u64 a2);
extern "C" u64 psci_smc_call(u64 fn, u64 a0, u64 a1, u64 a2);

namespace smp {

namespace {

constexpr u32 kWakeSgi = 1;
constexpr u64 kPsciCpuOn64 = 0xC4000003ull;

struct CpuInfo {
    u32 count;
    u64 mpidr[kMaxCpus];
    bool psci_hvc;
    u64 psci_cpu_on;
};

static CpuInfo g_info{1, {0, 1, 2, 3}, true, kPsciCpuOn64};
static volatile u32 g_cpu_started[kMaxCpus]{};

static bool streq(const char* a, const char* b) {
    return libk::strcmp(a, b) == 0;
}

static bool starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

static u64 read_reg_cells(const u8* data, u32 len) {
    if (len >= 8) {
        return (static_cast<u64>(libk::be32(data)) << 32) | libk::be32(data + 4);
    }
    if (len >= 4) return libk::be32(data);
    return 0;
}

struct SmpVisitorCtx {
    CpuInfo& info;
    u8 kind_stack[16]{};
    u32 parsed_cpus{};
};

static bool smp_begin_node(void* v, const char* name, u32 depth) {
    auto& ctx = *static_cast<SmpVisitorCtx*>(v);
    const u8 parent = depth > 0 ? ctx.kind_stack[depth - 1] : 0;
    u8 kind = 0;
    if      (depth == 1 && streq(name, "psci"))            kind = 1;
    else if (depth == 1 && streq(name, "cpus"))            kind = 2;
    else if (parent == 2 && starts_with(name, "cpu@"))     kind = 3;
    else kind = (parent == 1) ? 1 : 0;
    if (depth < 16) ctx.kind_stack[depth] = kind;
    return true;
}

static void smp_end_node(void*, u32) {}

static void smp_prop(void* v, const char* name, const u8* data, u32 len, u32 depth) {
    auto& ctx = *static_cast<SmpVisitorCtx*>(v);
    const u8 kind = depth < 16 ? ctx.kind_stack[depth] : 0;
    if (kind == 1 && streq(name, "method") && len >= 4)
        ctx.info.psci_hvc = !streq(reinterpret_cast<const char*>(data), "smc");
    else if (kind == 1 && streq(name, "cpu_on") && len >= 4)
        ctx.info.psci_cpu_on = read_reg_cells(data, len);
    else if (kind == 3 && streq(name, "reg") && ctx.parsed_cpus < kMaxCpus)
        ctx.info.mpidr[ctx.parsed_cpus++] = read_reg_cells(data, len);
}

static CpuInfo parse_dtb(PhysAddr dtb_phys) {
    CpuInfo info{};
    info.count = 1;
    info.mpidr[0] = 0;
    info.mpidr[1] = 1;
    info.psci_hvc = true;
    info.psci_cpu_on = kPsciCpuOn64;

    if (!dtb_phys) return info;
    const auto* dtb = reinterpret_cast<const u8*>(dtb_phys);
    SmpVisitorCtx vctx{info};
    fdt::walk(dtb, {&vctx, smp_begin_node, smp_end_node, smp_prop});
    if (vctx.parsed_cpus > 0) info.count = vctx.parsed_cpus;
    if (info.count > kMaxCpus) info.count = kMaxCpus;
    return info;
}

static u64 psci_call(u64 fn, u64 a0, u64 a1, u64 a2) {
    if (g_info.psci_hvc) return psci_hvc_call(fn, a0, a1, a2);
    else                  return psci_smc_call(fn, a0, a1, a2);
}

} // namespace

void init(PhysAddr dtb_phys) {
    g_info = parse_dtb(dtb_phys);
    if (g_boot_el >= 2) g_info.psci_hvc = false;
    if (g_info.count > kMaxCpus) g_info.count = kMaxCpus;

    g_cpu_started[0] = 1;

    for (u32 cpu = 1; cpu < g_info.count; ++cpu) {
        const u64 rc = psci_call(g_info.psci_cpu_on,
                                 g_info.mpidr[cpu],
                                 reinterpret_cast<u64>(&secondary_start),
                                 cpu);
        if (rc != 0) {
            ANKO_LOGF_WARN("kernel/smp", "cpu%u start failed rc=%llu", cpu,
                           static_cast<unsigned long long>(rc));
            continue;
        }

        for (u32 spin = 0; spin < 10000000; ++spin) {
            if (__atomic_load_n(&g_cpu_started[cpu], __ATOMIC_ACQUIRE)) break;
            asm volatile("yield" ::: "memory");
        }
    }

    u32 online = 1;
    for (u32 cpu = 1; cpu < g_info.count; ++cpu) {
        if (__atomic_load_n(&g_cpu_started[cpu], __ATOMIC_ACQUIRE)) online = cpu + 1;
    }
    sched::set_online_cpu_count(online);
    if (online >= 2) {
        ANKO_LOG_INFO("kernel/smp", "smp ready");
    }
}

u32 cpu_count() {
    return sched::online_cpu_count();
}

void send_wake_ipi(u32 cpu_id) {
    sched::note_ipi_sent(cpu_id);
    drivers::gic::send_sgi(cpu_id, kWakeSgi);
    asm volatile("sev" ::: "memory");
}

void handle_ipi() {
    sched::note_ipi_handled();
    sched::drain_pending_wakes();
    asm volatile("sev" ::: "memory");
}

extern "C" [[noreturn]] void secondary_kernel_main(u64 cpu_id) {
    if (cpu_id >= kMaxCpus) cpu_id = 0;
    asm volatile("msr TPIDR_EL1, %0\n"
                 "isb\n"
                 :
                 : "r"(cpu_id)
                 : "memory");
    mm::vm_enable_kernel();
    asm volatile(
        "msr VBAR_EL1, %0\n"
        "isb\n"
        :
        : "r"(&vectors)
        : "memory");
    drivers::gic::init_cpu();
    drivers::timer::init_cpu(g_hw.timer_irq);
    sched::mark_cpu_online(static_cast<u32>(cpu_id));
    __atomic_store_n(&g_cpu_started[cpu_id], 1u, __ATOMIC_RELEASE);
    sched::enter_secondary(static_cast<u32>(cpu_id));
}

} // namespace smp
