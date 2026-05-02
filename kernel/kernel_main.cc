// kernel/kernel_main.cc
//
// Kernel C++ entry point, called from arch/arm64/entry.S after:
//   stack live, BSS zeroed, EL1h, MMU off (Phase 0 / Phase 1 will turn it on).
//
// Startup order follows spec §5.1 and §8 V0 Boot Flow:
//
//   phase 0  early UART, libk putchar, "ANKO\n"   ← THIS COMMIT
//   phase 1  DTB parse, page tables + MMU, vectors, phys allocator, GIC, timer
//   phase 2  cap table, IPC, scheduler, init process, enter EL0

#include "anko/cap.h"
#include "anko/ipc.h"
#include "anko/mm.h"
#include "anko/platform.h"
#include "anko/sched.h"
#include "anko/smp.h"
#include "anko/syscall.h"
#include "anko/vm.h"
#include "anko/log.h"
#include "libk/printf.h"

namespace drivers::uart  { void early_init(PhysAddr base);
                           void putchar_thunk(char c); }
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

extern "C" [[noreturn]] void kernel_main() {
    // -----------------------------------------------------------------------
    // Phase 0: early UART — first output before anything can fail silently.
    // -----------------------------------------------------------------------
    drivers::uart::early_init(platform::kUartBase);
    anko::log::early_init();
    libk::set_putchar(drivers::uart::putchar_thunk);
    ANKO_LOG_INFO("kernel/boot", "ANKO");
    (void)g_dtb_phys;

    // Install exception vectors early so EL0 SVC has a defined handler.
    asm volatile(
        "msr VBAR_EL1, %0\n"
        "isb\n"
        :
        : "r"(&vectors)
        : "memory");
    ANKO_LOG_INFO("kernel/boot", "vectors set");

    // Direct-QEMU bootstrap: use the RAM size expected by scripts/run-qemu.sh.
    // Linux VM bring-up needs enough physical pages for the embedded initrd,
    // kernel heap, stage-2 metadata, and guest RAM. Keep this in sync with
    // QEMU_MEM default in scripts/run-qemu.sh until DTB RAM discovery replaces
    // the fixed bootstrap size.
    mm::phys_init(platform::kRamBase, 512ull * 1024ull * 1024ull);
    ANKO_LOG_INFO("kernel/boot", "phys init");
    mm::heap_init();
    ANKO_LOG_INFO("kernel/boot", "heap init");
    mm::vm_init();
    ANKO_LOG_INFO("kernel/boot", "mmu init");
    drivers::gic::init(platform::kGicDistBase, platform::kGicCpuBase);
    drivers::timer::init(platform::kTimerIrqPhysNS);
    ANKO_LOG_INFO("kernel/boot", "irq init");

    cap::init();
    ANKO_LOG_INFO("kernel/boot", "cap init");
    ipc::init();
    ANKO_LOG_INFO("kernel/boot", "ipc init");
    vm::init();
    ANKO_LOG_INFO("kernel/boot", "vm init");
    sched::init();
    ANKO_LOG_INFO("kernel/boot", "sched init");
    smp::init(g_dtb_phys);
    ANKO_LOG_INFO("kernel/boot", "creating init");
    kernel::init::create_init_process();
    ANKO_LOG_INFO("kernel/boot", "entering scheduler");
    sched::enter();
}
