// The v6 transport embedded in full Anko. No heap, locks or constructors.
#include "boot_diagnostics.h"
#include "debug_io.h"
#include "anko/platform_clock.h"
#include "anko/interrupt_mask.h"
#include "midix/abi/cpu_limits.h"
#include "anko/sched.h"
#include "midix/abi/diagnostics.h"
#if defined(ANKO_ANNE_DISPLAY_CAPTURE) || defined(ANKO_ANNE_FRAMEBUFFER)
#include "display_diagnostics.h"
#endif
namespace drivers::timer { u64 cpu_tick_count(u32 cpu); }

namespace {
// Entry calls this before clearing BSS. Explicit .data placement preserves the
// state across that clear, including faults during constructors.
struct State {
    u64 magic;
    u32 cursor[5];
    u32 flushed[5];
    u32 stage;
    u32 ready;
    u32 tick_seen;
    u32 services_match;
    u32 syscall_seen;
    u64 capture_start;
    u32 lock_ready;
    u32 lock_owner;
    u32 cpu_stage[MX_HOST_MAX_CPUS];
    u32 screen_ready;
    u32 screen_suppressed;
    u32 retention_waiting;
};
__attribute__((section(".data.anne_diag"))) State state{0x414e4b4f44494147, {}, {}, 0, 0, 0, 0, 0, 0, 0, 0, {}, 0, 0, 0};
u32 cpu_id() {
    u64 id; asm volatile("mrs %0, tpidr_el1" : "=r"(id));
    return id < MX_HOST_MAX_CPUS ? id : 0;
}
constexpr const char* kStageNames[] = {
    "invalid", "entry", "bss.begin", "bss.done", "ctors.begin", "ctors.done",
    "kernel_main", "console.ready", "probe.begin", "probe.done", "physical.begin", "physical.done",
    "reservations.done", "heap.begin", "heap.done", "mmu.begin", "mmu.enable", "mmu.done",
    "iommu.begin", "iommu.done", "gic.begin", "gic.done", "timer.begin", "timer.done",
    "caps.done", "ipc.done", "vm.done", "scheduler.done", "smp.done",
    "bundle.begin", "bundle.authenticated", "init.spawn.begin", "init.spawn.done", "init.caps.done",
    "scheduler.enter", "timer.first_tick", "services.ready",
    "caps.enter", "caps.bootpage", "caps.slab", "caps.slot0", "bundle.loaded",
    "caps.arena", "caps.manifest", "caps.copy", "caps.grants"
};
struct IrqGuard {
    u64 saved;
    bool owns = false;
    IrqGuard() {
        asm volatile("mrs %0, daif\n msr daifset, #2" : "=r"(saved) :: "memory");
        u64 sctlr; asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        // No exclusive accesses before normal-memory attributes are installed.
        if (!state.lock_ready || !(sctlr & 1)) return;
        const u32 me = cpu_id() + 1;
        if (__atomic_load_n(&state.lock_owner, __ATOMIC_ACQUIRE) == me) return;
        u32 empty = 0;
        while (!__atomic_compare_exchange_n(&state.lock_owner, &empty, me, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            empty = 0; asm volatile("yield" ::: "memory");
        }
        owns = true;
    }
    ~IrqGuard() {
        if (owns) __atomic_store_n(&state.lock_owner, 0u, __ATOMIC_RELEASE);
        asm volatile("msr daif, %0" :: "r"(kernel::host_interrupt_state(saved)) : "memory");
    }
};
void raw_putc(char c) {
    if (!state.ready) return;
#ifdef ANKO_ANNE_FRAMEBUFFER
    // This flag lives in initialized .data: never touch the screen's BSS
    // object while entry logging still runs before the BSS clear.
    if (state.screen_ready && !state.screen_suppressed) anne::native_display::console_putchar(c);
#endif
    for (u32 i = 0; i < 5; ++i) {
        if (state.cursor[i] >= anne::kDebugZoneSizes[i] - 12) continue;
        auto* header = reinterpret_cast<volatile u32*>(anne::kDebugZones[i]);
        auto* bytes = reinterpret_cast<volatile u8*>(anne::kDebugZones[i] + 12);
        bytes[state.cursor[i]++] = static_cast<u8>(c);
        header[1] = state.cursor[i];
        header[2] = state.cursor[i];
    }
}
void text(const char* s) { while (*s) raw_putc(*s++); }
void hex(u64 value) {
    text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        const u32 digit = (value >> shift) & 15;
        raw_putc(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
}
void number(u64 value) {
    char digits[20]; u32 n = 0;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) raw_putc(digits[--n]);
}
void cache_snapshot() {
    u64 midr, sctlr, mair, tcr;
    asm volatile("mrs %0, midr_el1" : "=r"(midr));
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    asm volatile("mrs %0, mair_el1" : "=r"(mair));
    asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
    text("\nCACHE cpu="); number(cpu_id()); text(" midr="); hex(midr);
    text(" sctlr="); hex(sctlr); text(" mair="); hex(mair); text(" tcr="); hex(tcr);
    // Cortex-A53 TRM DDI 0500J 4.3.79: this register is readable at EL1;
    // writes depend on firmware permissions. Observe SMPEN without changing
    // retention settings or assuming a generic CPU implements this register.
    if ((midr >> 24) == 0x41 && ((midr >> 4) & 0xfff) == 0xd03) {
        u64 extended;
        asm volatile("mrs %0, S3_1_C15_C2_1" : "=r"(extended));
        text(" cpuectlr="); hex(extended);
    }
    text("\n");
}
void led_hold(char led) {
    // Set and record the LED, but do not dwell. The old 3-second blocking
    // holds cost ~6 s before readiness (plus one after it and one per reset);
    // the watchdog is fed by checkpoints and auth progress, so nothing here
    // needs the loop. The colour transition remains visible to a human.
    anne::debug_led(led);
    text("LED "); raw_putc(led); text(" ctl="); hex(anne::debug_pmic_read(0x14d));
    text(" out="); hex(anne::debug_pmic_read(0x14e)); text("\n");
    anne::boot_log_flush();
}
void firmware_fiq_snapshot(const char* phase) {
    // This is the same reserved normal-world source word the vendor fatal FIQ
    // path reads. Every alias is Normal NC after MMU setup; no firmware writes.
    asm volatile("dsb sy" ::: "memory");
    const u64 word = *reinterpret_cast<volatile const u64*>(anne::kFirmwareFiqSource);
    text("\nFIQ_SOURCE phase="); text(phase); text(" word="); hex(word);
    text(" tick="); hex(anne::minimal::counter()); text("\n");
    anne::boot_log_flush();
}
[[maybe_unused, noreturn]] void capture_reset(const char* reason) {
    asm volatile("msr daifset, #15" ::: "memory");
    text("\nCAPTURE reason="); text(reason); text(" last="); number(state.stage);
    text("\nRESET nmi=0x4\n"); anne::boot_log_flush();
    anne_diag_reset();
}
void capture_poll() {
#if ANKO_ANNE_DIAG_CAPTURE_SECONDS > 0
    if (state.retention_waiting) return;
    u64 reported;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(reported));
    if (!ANKO_ANNE_DIAG_RESET_AT_READY && state.stage < static_cast<u32>(anne::BootStage::ServicesReady)) {
        // Runtime observation begins at readiness. Boot/display work on a
        // loaded host can outlast a short runtime interval; retain a separate
        // bounded startup failure capture instead of reporting it as runtime.
        constexpr u64 boot_seconds = ANKO_ANNE_DIAG_CAPTURE_SECONDS > 30 ? ANKO_ANNE_DIAG_CAPTURE_SECONDS : 30;
        if (state.capture_start && anne::minimal::elapsed(state.capture_start, anne::minimal::counter(),
                boot_seconds * platform::counter_frequency(reported))) capture_reset("timeout");
        return;
    }
    if (state.capture_start && anne::minimal::elapsed(state.capture_start, anne::minimal::counter(),
            static_cast<u64>(ANKO_ANNE_DIAG_CAPTURE_SECONDS) * platform::counter_frequency(reported))) {
        if (!ANKO_ANNE_DIAG_RESET_AT_READY) {
            u64 daif; asm volatile("mrs %0, daif" : "=r"(daif));
            text("\nRUNTIME observed seconds="); number(ANKO_ANNE_DIAG_CAPTURE_SECONDS);
            text(" daif="); hex(daif); text("\n");
            const u64 mask = sched::online_cpu_mask();
            for (u32 cpu = 0; cpu < MX_HOST_MAX_CPUS; ++cpu) {
                if (!(mask & (1ull << cpu))) continue;
                text("RUNTIME cpu="); number(cpu); text(" ticks="); number(drivers::timer::cpu_tick_count(cpu));
                text(" switches="); number(sched::cpu_context_switch_count(cpu)); text("\n");
            }
#ifdef ANKO_ANNE_WATCHDOG
            anne::watchdog::report("observed");
#endif
#if ANKO_ANNE_DIAG_WATCHDOG_RETENTION
            if (anne::watchdog::pause_for_capture()) {
                state.retention_waiting = 1;
                text("\nCAPTURE reason=runtime.observed last="); number(state.stage);
                text("\nRETENTION watchdog.expiry deliberately_requested=1\n");
                anne::boot_log_flush();
                return;
            }
#endif
            capture_reset("runtime.observed");
        }
        capture_reset("timeout");
    }
#endif
}
}

extern "C" {
__attribute__((section(".data.anne_diag"))) volatile u32 anne_diag_fault_active[MX_HOST_MAX_CPUS]{};
// Hand-shake state for the fault-tolerant probe hook in boot_diagnostics.S.
// [0]=armed [1]=faults [2]=esr [3]=elr [4]=far [5]=owning cpu.
__attribute__((section(".data.anne_diag"))) volatile u64 anne_diag_probe_state[6]{};
}

namespace anne {
void boot_log_info(u64* size, u64* capacity) {
    IrqGuard guard;
    *size = state.ready ? state.cursor[0] : 0;
    *capacity = kDebugZoneSizes[0] - 12;
}
usize boot_log_read(u64 offset, char* out, usize size) {
    IrqGuard guard;
    if (!state.ready || !out || offset >= state.cursor[0]) return 0;
    const u64 remaining = state.cursor[0] - offset;
    if (size > MX_DIAG_READ_MAX) size = MX_DIAG_READ_MAX;
    if (size > remaining) size = static_cast<usize>(remaining);
    // The reserved log has a Normal NC mapping. The writer lock orders its
    // cursor and bytes; do not invalidate or expose the physical mapping.
    const auto* source = reinterpret_cast<const volatile char*>(kDebugZones[0] + 12);
    for (usize i = 0; i < size; ++i) out[i] = source[offset + i];
    return size;
}
void boot_log_flush() {
    if (!state.ready) return;
    IrqGuard guard;
    for (u32 i = 0; i < 5; ++i) {
        debug_clean(kDebugZones[i], 12);
        if (state.cursor[i] > state.flushed[i])
            debug_clean(kDebugZones[i] + 12 + state.flushed[i], state.cursor[i] - state.flushed[i]);
        state.flushed[i] = state.cursor[i];
    }
#ifdef ANKO_ANNE_FRAMEBUFFER
    if (state.screen_ready) native_display::console_flush();
#endif
}
void boot_checkpoint(BootStage stage) {
    if (stage == BootStage::MmuDone) state.lock_ready = 1;
    IrqGuard guard;
#ifdef ANKO_ANNE_WATCHDOG
    watchdog::poll();
#endif
    const u32 id = cpu_id(), current = static_cast<u32>(stage);
    state.cpu_stage[id] = current;
    if ((!id || stage == BootStage::ServicesReady) && current > state.stage) state.stage = current;
    text("\nBOOT "); number(current); raw_putc(' ');
    text(current < sizeof(kStageNames) / sizeof(kStageNames[0]) ? kStageNames[current] : "unknown");
    text(" tick="); hex(minimal::counter()); text(" cpu="); number(id); text("\n");
    boot_log_flush();
#ifdef ANKO_ANNE_DIAG_TEST_FAULT_STAGE
    if (current == ANKO_ANNE_DIAG_TEST_FAULT_STAGE) asm volatile("brk #0");
#endif
    if (stage == BootStage::Entry) led_hold('r');
    if (stage == BootStage::KernelMain) led_hold('b');
    if (stage == BootStage::MmuDone) {
        cache_snapshot();
        firmware_fiq_snapshot("mmu.done");
    }
#ifdef ANKO_ANNE_DISPLAY_CAPTURE
    if (stage == BootStage::MmuDone) native_display::snapshot("mmu.done", true);
#endif
#ifdef ANKO_ANNE_FRAMEBUFFER
    if (stage == BootStage::MmuDone && native_display::bind_console()) {
        state.screen_ready = 1;
        // Replay all retained early output; the screen renderer filters raw
        // register/constructor metadata and keeps the boot stages and messages.
        const auto* bytes = reinterpret_cast<const char*>(kDebugZones[0] + 12);
        const u32 count = state.cursor[0];
        for (u32 i = 0; i < count; ++i) native_display::console_putchar(bytes[i]);
        native_display::console_flush();
        text("DISPLAY console.replayed bytes="); number(count); text("\n"); boot_log_flush();
    }
#endif
    if (stage == BootStage::ServicesReady) {
        const u64 mask = sched::online_cpu_mask();
        for (u32 cpu = 0; cpu < MX_HOST_MAX_CPUS; ++cpu) {
            if (!(mask & (1ull << cpu))) continue;
            text("RESSHA cpu="); number(cpu); text(" switches="); number(sched::cpu_context_switch_count(cpu));
            text(" preempts="); number(sched::cpu_timer_preempt_count(cpu));
            text(" ticks="); number(drivers::timer::cpu_tick_count(cpu));
            text(" ipis="); number(sched::cpu_ipi_handled_count(cpu)); text("\n");
        }
        led_hold('g');
#ifdef ANKO_ANNE_FRAMEBUFFER
        if (state.screen_ready) {
            text("DISPLAY console.services_ready characters="); number(native_display::console_characters());
            text("\n"); boot_log_flush();
        }
#endif
#if ANKO_ANNE_DIAG_CAPTURE_SECONDS > 0 && ANKO_ANNE_DIAG_READY_HOLD_SECONDS > 0
        text("DISPLAY console.hold seconds="); number(ANKO_ANNE_DIAG_READY_HOLD_SECONDS); text("\n"); boot_log_flush();
#ifndef ANKO_ANNE_DIAG_TEST
        minimal::wait_ticks(static_cast<u64>(ANKO_ANNE_DIAG_READY_HOLD_SECONDS) * minimal::kCounterHz);
#endif
#endif
    }
#if ANKO_ANNE_DIAG_CAPTURE_SECONDS > 0
    if (stage == BootStage::SchedulerEnter) state.capture_start = minimal::counter();
    if (stage == BootStage::ServicesReady && !ANKO_ANNE_DIAG_RESET_AT_READY)
        state.capture_start = minimal::counter();
    if (stage == BootStage::ServicesReady && ANKO_ANNE_DIAG_RESET_AT_READY) capture_reset("services.ready");
#endif
#ifdef ANKO_ANNE_FRAMEBUFFER
    // Only once the console is bound: the lock and the console objects live in
    // .bss, which is not safe to touch at the entry checkpoints, and drawing
    // before the scanout is verified would go nowhere.
    if (state.screen_ready)
        native_display::console_status(current < sizeof(kStageNames) / sizeof(kStageNames[0])
                                           ? kStageNames[current] : "unknown");
#endif
}
void boot_log_putchar(char c) {
    IrqGuard guard;
    raw_putc(c);
    // This recognizes the existing init readiness line without changing its ABI.
    constexpr const char ready[] = "init: all services ready";
    if (c == ready[state.services_match]) ++state.services_match;
    else state.services_match = c == ready[0] ? 1 : 0;
    if (state.services_match == sizeof(ready) - 1) {
        state.services_match = 0;
        boot_checkpoint(BootStage::ServicesReady);
    }
    if (c == '\n') boot_log_flush();
}
void boot_log_write(const char* bytes, usize size, bool mirror_screen) {
    IrqGuard guard;
    if (!mirror_screen) ++state.screen_suppressed;
    for (usize i = 0; i < size; ++i) boot_log_putchar(bytes[i]);
    if (!mirror_screen) --state.screen_suppressed;
}
void boot_log_early(char level, const char* component, const char* message) {
    IrqGuard guard;
    text("[early] ["); raw_putc(level); text("] cpu"); number(cpu_id()); raw_putc(' ');
    text(component ? component : "kernel"); text(": ");
    const char* p = message ? message : "(null)";
    u32 n = 0; while (n < 192 && p[n]) raw_putc(p[n++]);
    if (!n || p[n - 1] != '\n') raw_putc('\n');
    boot_log_flush();
}
void boot_secondary_online(u32 cpu, u64 mpidr) {
    IrqGuard guard;
    state.cpu_stage[cpu] = static_cast<u32>(BootStage::SmpDone);
    text("\nCPU "); number(cpu); text(" secondary.entry mpidr="); hex(mpidr);
    cache_snapshot();
    text("\n"); boot_log_flush();
}
void boot_log_fault(u64 esr, u64 pc, u64 far, u64 sp) {
    IrqGuard guard;
    text("\n!FAULT last="); number(state.cpu_stage[cpu_id()]); text(" esr="); hex(esr);
    text(" pc="); hex(pc); text(" far="); hex(far); text(" sp="); hex(sp);
    text(" cpu="); number(cpu_id()); text("\n"); boot_log_flush();
}
void boot_log_panic(const char* message) {
    IrqGuard guard;
    text("\n!PANIC last="); number(state.stage); raw_putc(' ');
    text(message ? message : "unknown"); text("\n"); boot_log_flush();
}
[[noreturn]] void boot_fault_reset() {
    asm volatile("msr daifset, #15" ::: "memory");
    led_hold('w'); text("RESET nmi=0x4\n"); boot_log_flush();
    anne_diag_reset();
}
void boot_timer_tick() {
    const u32 bit = 1u << cpu_id();
#if ANKO_ANNE_DIAG_CAPTURE_SECONDS == 0
    // IRQ entry already excludes local interrupts. Completed one-time records
    // need no global diagnostic lock; watchdog ownership remains on CPU zero.
    if (__atomic_load_n(&state.tick_seen, __ATOMIC_ACQUIRE) & bit) {
#ifdef ANKO_ANNE_WATCHDOG
        watchdog::poll();
#endif
        return;
    }
#endif
    IrqGuard guard;
#ifdef ANKO_ANNE_WATCHDOG
    watchdog::poll();
#endif
    capture_poll();
    if (state.tick_seen & bit) return;
    __atomic_store_n(&state.tick_seen, state.tick_seen | bit, __ATOMIC_RELEASE);
    boot_checkpoint(BootStage::FirstTimerTick);
}
void boot_syscall(u64 nr) {
#if ANKO_ANNE_DIAG_CAPTURE_SECONDS == 0
    // Native steady-state IPC must not serialize all eight A53 cores through
    // the boot logger. SVC entry masks IRQs, so CPU-zero watchdog polling can
    // continue without the logger's cross-CPU lock.
    if (__atomic_load_n(&state.syscall_seen, __ATOMIC_ACQUIRE)) {
#ifdef ANKO_ANNE_WATCHDOG
        watchdog::poll();
#endif
        return;
    }
#endif
    IrqGuard guard;
#ifdef ANKO_ANNE_WATCHDOG
    watchdog::poll();
#endif
    if (!state.syscall_seen) {
        u64 saved_pstate; asm volatile("mrs %0, spsr_el1" : "=r"(saved_pstate));
        text("\nEL0 first_svc="); number(nr); text(" saved_pstate="); hex(saved_pstate); text("\n"); boot_log_flush();
        __atomic_store_n(&state.syscall_seen, 1u, __ATOMIC_RELEASE);
    }
    capture_poll();
}
// Fault-tolerant single-u64 read. Arms the board fault hook for exactly one
// load; when the address is denied the abort is captured, the instruction is
// skipped in the handler, and the failure is reported here instead of
// resetting the kernel. Returns true when the value was read.
bool probe_read(u64 addr, u64* out_value, u64* out_esr, u64* out_far, u64* out_elr,
                u64* out_faults) {
    if (out_value) *out_value = 0;
    if (out_esr) *out_esr = 0;
    if (out_far) *out_far = 0;
    if (out_elr) *out_elr = 0;
    if (out_faults) *out_faults = 0;
    u64 cpu = 0;
    asm volatile("mrs %0, tpidr_el1" : "=r"(cpu));
    if (cpu >= MX_HOST_MAX_CPUS) cpu = 0;
    IrqGuard guard;
    anne_diag_probe_state[1] = 0;
    anne_diag_probe_state[5] = cpu;
    asm volatile("dmb sy" ::: "memory");
    anne_diag_probe_state[0] = 1;
    asm volatile("dmb sy" ::: "memory");
    u64 value = 0;
    asm volatile("ldr %0, [%1]" : "=r"(value) : "r"(addr) : "memory");
    asm volatile("dmb sy" ::: "memory");
    anne_diag_probe_state[0] = 0;
    const u64 faults = anne_diag_probe_state[1];
    if (out_faults) *out_faults = faults;
    if (faults != 0) {
        if (out_esr) *out_esr = anne_diag_probe_state[2];
        if (out_elr) *out_elr = anne_diag_probe_state[3];
        if (out_far) *out_far = anne_diag_probe_state[4];
        return false;
    }
    if (out_value) *out_value = value;
    return true;
}

void boot_user_fault(u64 esr, u64 pc, u64 far, u64 sp, u32 pid, u32 tid) {
    IrqGuard guard;
    text("\n!USERFAULT pid="); number(pid); text(" tid="); number(tid);
    text(" esr="); hex(esr); text(" pc="); hex(pc);
    text(" far="); hex(far); text(" sp="); hex(sp); text("\n"); boot_log_flush();
#if ANKO_ANNE_DIAG_CAPTURE_SECONDS > 0
    capture_reset("el0.fault");
#endif
}
}

extern "C" void anne_diag_init(u64 el, u64 dtb, u64 sctlr) {
    state.ready = 0; state.stage = 0; state.tick_seen = 0; state.services_match = 0;
    state.syscall_seen = 0; state.capture_start = 0;
    state.lock_ready = 0; state.lock_owner = 0;
    state.screen_ready = 0; state.screen_suppressed = 0;
    for (u32 cpu = 0; cpu < MX_HOST_MAX_CPUS; ++cpu) {
        anne_diag_fault_active[cpu] = 0; state.cpu_stage[cpu] = 0;
    }
    for (u32 i = 0; i < 5; ++i) {
        state.cursor[i] = 0; state.flushed[i] = 0;
        auto* header = reinterpret_cast<volatile u32*>(anne::kDebugZones[i]);
        header[0] = 0x43474244; header[1] = 0; header[2] = 0;
        if (i >= 2) {
            auto* bytes = reinterpret_cast<volatile u8*>(anne::kDebugZones[i] + 12);
            const char* prefix = "====0.0-D\n";
            while (*prefix) bytes[state.cursor[i]++] = *prefix++;
            header[1] = state.cursor[i]; header[2] = state.cursor[i];
        }
    }
    state.ready = 1;
    text("AMEANKO1 el="); number(el); text(" x0="); hex(dtb);
    text(" sctlr="); hex(sctlr); text(" counter_hz=1920000\n");
    // Read only registers available at the recorded EL1/EL2 entry. This logs
    // CPU capability without attempting to call an unknown firmware monitor.
    u64 pfr, midr, mpidr;
    asm volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(pfr));
    asm volatile("mrs %0, MIDR_EL1" : "=r"(midr));
    asm volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    text("CPU_FEATURES midr="); hex(midr); text(" mpidr="); hex(mpidr);
    text(" pfr0="); hex(pfr); text(" el2_field="); number((pfr >> 8) & 0xf);
    text("\n");
    text("RUN tag="); text(ANKO_ANNE_DIAG_RUN_TAG); text("\n");
#ifdef ANKO_ANNE_WATCHDOG
    anne::watchdog::adopt();
#endif
    firmware_fiq_snapshot("entry");
    anne::boot_checkpoint(anne::BootStage::Entry);
#ifdef ANKO_ANNE_DISPLAY_CAPTURE
    anne::native_display::snapshot("entry", false);
#endif
}
extern "C" void anne_diag_stage(u64 stage) { anne::boot_checkpoint(static_cast<anne::BootStage>(stage)); }
extern "C" void anne_diag_constructor(u64 pc) {
    IrqGuard guard; text("CTOR pc="); hex(pc); text("\n"); anne::boot_log_flush();
}
extern "C" [[noreturn]] void anne_diag_early_fault(u64 esr, u64 pc, u64 far, u64 sp, u64 slot) {
    u64 el, saved_pstate;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    if (el == 8) asm volatile("mrs %0, spsr_el2" : "=r"(saved_pstate));
    else asm volatile("mrs %0, spsr_el1" : "=r"(saved_pstate));
    text("\nVECTOR "); number(slot); text(" saved_pstate="); hex(saved_pstate); text("\n");
    if (slot % 4 == 2) {
        firmware_fiq_snapshot("fault");
#ifdef ANKO_ANNE_WATCHDOG
        anne::watchdog::report("fault");
#endif
    }
    anne::boot_log_fault(esr, pc, far, sp);
    anne::boot_fault_reset();
}
