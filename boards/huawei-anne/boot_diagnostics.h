#pragma once
#include "anko/types.h"
#include "minimal/timing.h"
#ifdef ANKO_ANNE_WATCHDOG
#include "watchdog.h"
#endif

namespace anne {
#ifdef ANKO_ANNE_DIAG_TEST
constexpr u64 kFirmwareFiqSource = 0x47400000;
#else
// Hi6250 global_ddr_map.h, normal-world diagnostic word read by
// hisi_ddr_secprotect.c. Read only these eight bytes, not other shared buffers.
constexpr u64 kFirmwareFiqSource = 0x34a21000;
#endif
enum class BootStage : u32 {
    Entry = 1, BssBegin, BssDone, ConstructorsBegin, ConstructorsDone,
    KernelMain, ConsoleReady, ProbeBegin, ProbeDone, PhysicalBegin, PhysicalDone,
    ReservationsDone, HeapBegin, HeapDone, MmuBegin, MmuEnable, MmuDone,
    IommuBegin, IommuDone, GicBegin, GicDone, TimerBegin, TimerDone,
    CapsDone, IpcDone, VmDone, SchedulerDone, SmpDone,
    BundleBegin, BundleAuthenticated, InitSpawnBegin, InitSpawnDone, InitCapsDone,
    SchedulerEnter, FirstTimerTick, ServicesReady
};
void boot_checkpoint(BootStage stage);
void boot_log_putchar(char c);
void boot_log_write(const char* text, usize size, bool mirror_screen = true);
void boot_log_early(char level, const char* component, const char* message);
void boot_secondary_online(u32 cpu, u64 mpidr);
void boot_log_flush();
void boot_log_fault(u64 esr, u64 pc, u64 far, u64 sp);
void boot_log_panic(const char* message);
[[noreturn]] void boot_fault_reset();
void boot_timer_tick();
void boot_syscall(u64 nr);
void boot_user_fault(u64 esr, u64 pc, u64 far, u64 sp, u32 pid, u32 tid);
}
extern "C" {
void anne_diag_init(u64 entry_el, u64 dtb, u64 sctlr);
void anne_diag_stage(u64 stage);
void anne_diag_constructor(u64 pc);
[[noreturn]] void anne_diag_early_fault(u64 esr, u64 pc, u64 far, u64 sp, u64 slot);
[[noreturn]] void anne_diag_reset();
void anne_diag_install_early_vectors();
}
