// kernel/kernel_main.cc
//
// Kernel C++ entry point, called from arch/arm64/entry.S after:
//   stack live, BSS zeroed, EL1h, MMU off (Phase 0 / Phase 1 will turn it on).
//
// Startup order follows spec §5.1 and §8 V0 Boot Flow:
//
//   phase 0  early UART, libk putchar, "anko\n"   ← THIS COMMIT
//   phase 1  DTB parse, page tables + MMU, vectors, phys allocator, GIC, timer
//   phase 2  cap table, IPC, scheduler, init process, enter EL0

#include "anko/cap.h"
#include "anko/console.h"
#include "anko/fat_log.h"
#include "anko/hw_info.h"
#include "anko/ipc.h"
#include "anko/irq.h"
#include "anko/mm.h"
#include "anko/platform.h"
#include "anko/sched.h"
#include "anko/smp.h"
#include "anko/syscall.h"
#include "anko/vm.h"
#include "anko/log.h"
#include "anko/init.h"
#include "anko/fdt.h"
#include "libk/string.h"
#include "libk/printf.h"

namespace drivers::uart  { void putchar_thunk(char c); }
namespace drivers::gic   { void init(PhysAddr dist_base, PhysAddr cpu_base); }
namespace drivers::timer { void init(u32 irq_id); }
namespace mm             { void phys_init(PhysAddr ram_base, usize ram_size);
                           void heap_init(); void vm_init(); }
namespace cap            { void init(); }
namespace sched          { [[noreturn]] void enter(); void init(); }
namespace ipc            { void init(); }
namespace vm             { void init(); }
namespace smp            { void init(PhysAddr dtb_phys); }
namespace kernel::init   { void create_init_process(); }
extern "C" void vectors();

// DTB physical address — saved by entry.S from x0 before calling us.
extern "C" PhysAddr g_dtb_phys;

namespace {

u64 read_cells_be(const u8* data, u32 cells, u32 len) {
    if (!data || cells == 0 || len < cells * 4u) return 0;
    u64 value = 0;
    for (u32 i = 0; i < cells; ++i) {
        value = (value << 32) | libk::be32(data + i * 4u);
    }
    return value;
}

struct ReserveCtx {
    u32 count;
    u8 kind[16];
};

bool reserve_memrsv_entry(void* v, u64 address, u64 size) {
    auto& ctx = *static_cast<ReserveCtx*>(v);
    mm::reserve_phys_range(address, size);
    ++ctx.count;
    return true;
}

bool reserve_begin(void* v, const char* name, u32 depth) {
    auto& ctx = *static_cast<ReserveCtx*>(v);
    const u8 parent = depth > 0 && depth - 1 < 16 ? ctx.kind[depth - 1] : 0;
    u8 kind = 0;
    if (depth == 1 && libk::strcmp(name, "reserved-memory") == 0) {
        kind = 1;
    } else if (parent == 1 || parent == 2) {
        kind = 2;
    }
    if (depth < 16) ctx.kind[depth] = kind;
    return true;
}

void reserve_end(void*, u32) {}

void reserve_prop(void* v, const char* name, const u8* data, u32 len, u32 depth) {
    auto& ctx = *static_cast<ReserveCtx*>(v);
    const u8 kind = depth < 16 ? ctx.kind[depth] : 0;
    if (kind != 2 || libk::strcmp(name, "reg") != 0) return;

    constexpr u32 kAddressCells = 2;
    constexpr u32 kSizeCells = 2;
    constexpr u32 kTupleBytes = (kAddressCells + kSizeCells) * 4u;
    for (u32 off = 0; off + kTupleBytes <= len; off += kTupleBytes) {
        const u64 base = read_cells_be(data + off, kAddressCells, len - off);
        const u64 size = read_cells_be(data + off + kAddressCells * 4u,
                                       kSizeCells,
                                       len - off - kAddressCells * 4u);
        if (base == 0 || size == 0) continue;
        mm::reserve_phys_range(base, size);
        ++ctx.count;
    }
}

void reserve_dtb_regions(PhysAddr dtb_phys) {
    if (!dtb_phys) return;
    const auto* dtb = reinterpret_cast<const u8*>(dtb_phys);
    if (libk::be32(dtb) != fdt::kMagic) return;

    ReserveCtx memrsv{};
    if (fdt::walk_memreserve(dtb, &memrsv, reserve_memrsv_entry) && memrsv.count) {
        ANKO_LOGF_INFO("kernel/dtb", "reserved memreserve entries=%u", memrsv.count);
    }

    ReserveCtx reserved{};
    if (fdt::walk(dtb, {&reserved, reserve_begin, reserve_end, reserve_prop}) &&
        reserved.count) {
        ANKO_LOGF_INFO("kernel/dtb", "reserved-memory regs=%u", reserved.count);
    }
}

// Paint the full T210 framebuffer with `color` (XRGB8888) as a boot-stage
// progress indicator.  No-op on QEMU (has_t210_dc = false).
// Stage palette: red=entry.S, green=probe done, blue=GIC+timer done.
static void fb_stage(u32 color) {
    if (!g_hw.has_t210_dc || !g_hw.t210_dc_fb_phys) return;
    auto* fb = reinterpret_cast<volatile u32*>(g_hw.t210_dc_fb_phys);
    const u64 pixels = static_cast<u64>(g_hw.t210_dc_fb_width) *
                       static_cast<u64>(g_hw.t210_dc_fb_height);
    for (u64 i = 0; i < pixels; ++i) fb[i] = color;
    asm volatile("dsb sy" ::: "memory");
}

} // namespace

extern "C" [[noreturn]] void kernel_main() {
    {
        const u64 boot_cpu_id = 0;
        asm volatile("msr TPIDR_EL1, %0\n"
                     "isb\n"
                     :
                     : "r"(boot_cpu_id)
                     : "memory");
    }
    // -----------------------------------------------------------------------
    // Phase 0: early UART — first output before anything can fail silently.
    // g_hw is pre-initialised from board_fallback.cc constants; probe() will
    // refine it from the DTB after early output is live.
    // -----------------------------------------------------------------------
    console::bind(g_hw.console_kind, g_hw.console_base);

    // Install exception vectors before the first formatted log line.  The log
    // path touches counters, stack buffers, and MMIO; if any of that faults
    // before VBAR_EL1 is live, EL1 vectors through address 0x0 and lands at
    // 0x200 on QEMU, which looks like a silent boot hang.
    asm volatile(
        "msr VBAR_EL1, %0\n"
        "isb\n"
        :
        : "r"(&vectors)
        : "memory");

    anko::log::early_init();
    libk::set_putchar(drivers::uart::putchar_thunk);
    ANKO_LOG_INFO("kernel/boot", "anko booting");

    // -----------------------------------------------------------------------
    // SD card log: initialise SDMMC1 and locate /ame/log/boot.log.
    // Installs a secondary putchar sink — all subsequent UART output is also
    // buffered for fat_log_flush().  Failure is non-fatal (UART still works).
    // -----------------------------------------------------------------------
#ifdef ANKO_BOARD_SWITCH_T210
    {
        const bool sd_ok = fat_log::init();
        ANKO_LOGF_INFO("kernel/fatlog", "sd log init %s", sd_ok ? "OK" : "FAIL");
        if (sd_ok) fat_log::write("[early] anko booting\n");
    }
#endif

    // -----------------------------------------------------------------------
    // Phase 1: probe DTB → update g_hw with discovered addresses/devices.
    // Must happen before phys_init so ram_base/ram_size are correct.
    // -----------------------------------------------------------------------
    // Reject unaligned DTB (can happen if U-Boot relocates it onto an odd boundary).
    ANKO_LOGF_INFO("kernel/probe", "dtb_phys=0x%llx align=%u", (unsigned long long)g_dtb_phys, (unsigned)(g_dtb_phys & 3));
    if (g_dtb_phys & 3) g_dtb_phys = 0;

    platform::probe(g_dtb_phys);
    // probe() painted the screen green if has_t210_dc.  Paint blue now to
    // signal we returned from probe() and are continuing init.
    fb_stage(0x000000FFu); // blue XRGB
    ANKO_LOGF_INFO("kernel/probe",
                   "boot_el=%u ram=0x%llx+0x%llx console=0x%llx gic=0x%llx/0x%llx fwcfg=%d pcie=%d vblk=%d",
                   g_hw.boot_el,
                   static_cast<unsigned long long>(g_hw.ram_base),
                   static_cast<unsigned long long>(g_hw.ram_size),
                   static_cast<unsigned long long>(g_hw.console_base),
                   static_cast<unsigned long long>(g_hw.gic_dist_base),
                   static_cast<unsigned long long>(g_hw.gic_cpu_base),
                   g_hw.has_fwcfg,
                   g_hw.has_pcie,
                   g_hw.has_virtio_blk);

    mm::phys_init(g_hw.ram_base, g_hw.ram_size);
    if (g_dtb_phys && libk::be32(reinterpret_cast<const void*>(g_dtb_phys)) == fdt::kMagic) {
        const u32 dtb_size = libk::be32(reinterpret_cast<const u8*>(g_dtb_phys) + 4);
        mm::reserve_phys_range(g_dtb_phys, dtb_size);
    }
    reserve_dtb_regions(g_dtb_phys);
    {
        PhysAddr initrd_base = 0;
        usize initrd_size = 0;
        if (kernel::init::discover_initrd(g_dtb_phys, &initrd_base, &initrd_size)) {
            mm::reserve_phys_range(initrd_base, initrd_size);
        }
    }
    if (g_hw.has_t210_dc) {
        const u64 fb_size = static_cast<u64>(g_hw.t210_dc_fb_width) *
                            static_cast<u64>(g_hw.t210_dc_fb_height) * 4ull;
        if (fb_size && fb_size <= 64ull * 1024ull * 1024ull) {
            mm::reserve_phys_range(g_hw.t210_dc_fb_phys, fb_size);
        }
    }
    mm::heap_init();
    mm::vm_init();
    drivers::gic::init(g_hw.gic_dist_base, g_hw.gic_cpu_base);
#ifdef ANKO_BUILD_DEV
    // End-to-end IRQ delivery check: raise a spare SPI and confirm it reaches a
    // doorbell. Runs with IRQs masked, before the timer is armed, so it cannot
    // perturb the timer or IPI paths.
    if (irq::run_selftest() == 1) {
        ANKO_LOG_INFO("kernel/gic", "irq delivery selftest ok");
    } else {
        ANKO_LOG_ERROR("kernel/gic", "irq delivery selftest FAILED");
    }
#endif
    drivers::timer::init(g_hw.timer_irq);
    fb_stage(0x00FFFFFFu); // white: GIC + timer done
#ifdef ANKO_BOARD_SWITCH_T210
    fat_log::flush(); // persist log up to this point
#endif

    cap::init();
    ipc::init();
    vm::init();
    sched::init();
    smp::init(g_dtb_phys);
    kernel::init::create_init_process();
    ANKO_LOG_INFO("kernel/boot", "scheduler ready");
    sched::enter();
}
