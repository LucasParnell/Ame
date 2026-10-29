// Hi6250 Midgard mediation. Linux owns jobs, scheduling and virtual mappings;
// Anko owns physical MMU roots, resource lifetime and the LPM3 power mailbox.
// Every hardware number lives in boards/huawei-anne/gpu.h with its vendor or
// upstream source; nothing here restates a register offset.
#include "anko/mali.h"
#include "anko/mali_shadow.h"
#include "anko/vm.h"
#include "anko/platform_control.h"
#include "anko/irq.h"
#include "anko/spinlock.h"
#include "anko/vm_progress.h"
#include "anko/log.h"
#include "midix/abi/anne_gpu.h"
#include "libk/dma.h"
#include "../../boards/huawei-anne/gpu.h"

namespace mali {
namespace {
namespace hw = ::anne::gpu;
constexpr u32 kHostIrqs[] = {hw::kHostIrqJob, hw::kHostIrqMmu, hw::kHostIrqGpu};
constexpr u32 kGuestIrqs[] = {AME_MALI_JOB_IRQ, AME_MALI_MMU_IRQ, AME_MALI_GPU_IRQ};
static_assert(hw::kIrqCount == sizeof(kHostIrqs) / sizeof(kHostIrqs[0]),
              "host IRQ table must match the vendor interrupt list");
struct Device {
    vm::Vm* owner;
    vm::Vcpu* cpu;
    Shadow shadow[2];
    u64 guest_root;
    u32 active;
    object::Record* shared_pins[vm::kMaxSharedArenas];
    u32 shared_pin_count;
    // Raw GPU_INT_RAWSTAT bits the host owns rather than forwarding. Panfrost
    // polls GPU_INT_RAWSTAT for GPU_IRQ_RESET_COMPLETED (panfrost_gpu.c:71),
    // so the completion must be visible as a latched register bit and not only
    // as an interrupt: a guest whose GIC line is not yet wired must still
    // finish probe.
    u32 synthetic_rawstat;
    bool ready, quarantined, suppressed[3], enabled;
};
Device gpu{};
kernel::SpinLock lock{};

class KernelMap {
    u64 saved_;

  public:
    KernelMap() {
        asm volatile("mrs %0, ttbr0_el1" : "=r"(saved_));
        mm::vm_activate(0);
    }
    ~KernelMap() {
        mm::vm_activate(saved_);
    }
};
u32 read(u64 base, u32 offset) {
    vm::note_progress(MX_VM_PROGRESS_MALI_READ, base + offset);
    const u32 value = *reinterpret_cast<volatile u32*>(base + offset);
    libk::dma::order_mmio();
    vm::note_progress(MX_VM_PROGRESS_MALI_READ_DONE, base + offset, value);
    return value;
}
void write(u64 base, u32 offset, u32 value) {
    vm::note_progress(MX_VM_PROGRESS_MALI_WRITE, base + offset, value);
    libk::dma::order_mmio();
    *reinterpret_cast<volatile u32*>(base + offset) = value;
    libk::dma::order_mmio();
    vm::note_progress(MX_VM_PROGRESS_MALI_WRITE_DONE, base + offset, value);
}
bool wait(u64 base, u32 offset, u32 mask, u32 expected, u64 timeout_ns) {
    const u64 start = sched::sched_clock_ns();
    // The iteration cap also bounds broken/stopped timekeeping.
    for (u32 n = 0; n < hw::kPollLimit; ++n) {
        if ((read(base, offset) & mask) == expected) {
            return true;
        }
        if (sched::sched_clock_ns() - start >= timeout_ns) {
            return false;
        }
        asm volatile("yield");
    }
    return false;
}

bool power(bool on) {
    // Only the dedicated g3d regulator mailbox is touched; no firmware or
    // voltage guess is made anywhere in this path.
    constexpr u32 b = hw::kG3dMailbox * hw::kMailboxStride;
    constexpr u32 source = hw::kSourceGic2, destination = hw::kDestinationLpm3;
    const u32 command = on ? hw::kCommandEnable : hw::kCommandDisable;
    write(hw::kIpcBase, hw::kMboxUnlock, hw::kMboxUnlockKey);
    if (read(hw::kIpcBase, hw::kMboxUnlock) != 0 ||
        !wait(hw::kIpcBase, b + hw::kMboxStatus, hw::kMboxStatusIdle, hw::kMboxStatusIdle,
              hw::kPowerTimeoutNs)) {
        return false;
    }
    write(hw::kIpcBase, b + hw::kMboxSource, source);
    if (read(hw::kIpcBase, b + hw::kMboxSource) != source) {
        return false;
    }
    write(hw::kIpcBase, b + hw::kMboxSourceRelease, ~(source | destination));
    write(hw::kIpcBase, b + hw::kMboxAckMask, destination);
    write(hw::kIpcBase, b + hw::kMboxStatus, 0);
    for (u32 i = 0; i < hw::kMboxDataWords; ++i) {
        write(hw::kIpcBase, b + hw::kMboxData + i * 4, i == 0 ? command : 0);
    }
    write(hw::kIpcBase, b + hw::kMboxTrigger, source);
    // On timeout leave the occupied mailbox alone; stealing it could race a
    // delayed firmware ACK. The caller quarantines instead of claiming power.
    if (!wait(hw::kIpcBase, b + hw::kMboxStatus, hw::kMboxStatusAck, hw::kMboxStatusAck,
              hw::kPowerTimeoutNs)) {
        return false;
    }
    // The vendor regulator driver's success test, verbatim.
    const bool ok = read(hw::kIpcBase, b + hw::kMboxData) == command &&
                    !(read(hw::kIpcBase, b + hw::kMboxData + 4) >> hw::kAckStatusShift);
    write(hw::kIpcBase, b + hw::kMboxAck, source);
    write(hw::kIpcBase, b + hw::kMboxSourceRelease, ~0U);
    write(hw::kIpcBase, b + hw::kMboxSource, source); // release SOURCE ownership
    return ok;
}

bool reset() {
    write(hw::kBase, hw::kRegIntMask, 0);
    write(hw::kBase, hw::kRegJobIntMask, 0);
    write(hw::kBase, hw::kRegMmuIntMask, 0);
    write(hw::kBase, hw::kRegIntClear, hw::kIrqResetCompleted); // stale completion
    gpu.synthetic_rawstat &= ~hw::kIrqResetCompleted;
    write(hw::kBase, hw::kRegCmd, hw::kCmdHardReset);
    if (!wait(hw::kBase, hw::kRegIntRawstat, hw::kIrqResetCompleted, hw::kIrqResetCompleted,
              hw::kResetTimeoutNs)) {
        return false;
    }
    // Latch it for the guest. The hardware latches the same bit, but the guest
    // reads it through this mediator, which answers from the register plus this
    // shadow, so probe's poll cannot miss a completion that already happened.
    gpu.synthetic_rawstat |= hw::kIrqResetCompleted;
    return true;
}
// Issue one AS command and wait for the slot to report idle. Every caller uses
// this to place a barrier between a table update and the next walk.
bool command(u32 cmd) {
    if (!wait(hw::kBase, hw::kAsStatus, 1, 0, hw::kCommandTimeoutNs)) {
        return false;
    }
    write(hw::kBase, hw::kAsCommand, cmd);
    return wait(hw::kBase, hw::kAsStatus, 1, 0, hw::kCommandTimeoutNs);
}
bool translate_ram(void* context, u64 guest, bool writable, u64* physical) {
    auto& owner = *static_cast<vm::Vm*>(context);
    if (guest & 4095) {
        return false;
    }
    for (u32 i = 0; i < owner.memory_count; ++i) {
        const auto* m = owner.memory_objects[i];
        if (!m || m->dead || m->shared_staging || !m->mapped_stage2 || !m->pages ||
            !(m->rights & RIGHT_READ) || (writable && !(m->rights & RIGHT_WRITE)) ||
            guest < m->guest_pa_base || guest - m->guest_pa_base >= m->mapped_size) {
            continue;
        }
        const u64 index = (guest - m->guest_pa_base) / PAGE_SIZE;
        if (index >= m->page_count || !m->pages[index]) {
            return false;
        }
        *physical = m->pages[index];
        return true;
    }
    return false;
}
bool translate(void* context, u64 guest, bool writable, u64* physical) {
    if (translate_ram(context, guest, writable, physical)) {
        return true;
    }
    auto& owner = *static_cast<vm::Vm*>(context);
    if (guest & (PAGE_SIZE - 1)) {
        return false;
    }
    for (u32 i = 0; i < owner.shared_arena_count; ++i) {
        auto* region = owner.shared_arenas[i];
        if (!region || region->dead || !region->mapped_stage2 || region->owner_vm != &owner ||
            region->dma_class != iommu::VmDmaClass::SharedGpu || !(region->rights & RIGHT_READ) ||
            !(region->dma_perms & 1) ||
            (writable && (!(region->rights & RIGHT_WRITE) || !(region->dma_perms & 2))) ||
            guest < region->guest_pa_base || guest - region->guest_pa_base >= region->size) {
            continue;
        }
        // Keep authorized backing and its stage-2 mapping alive until GPU reset,
        // even if the native owner closes its attachment capability early.
        bool pinned = false;
        for (u32 n = 0; n < gpu.shared_pin_count; ++n) {
            if (gpu.shared_pins[n] == region->lifetime) {
                pinned = true;
            }
        }
        if (!pinned) {
            if (gpu.shared_pin_count == vm::kMaxSharedArenas) {
                return false;
            }
            object::retain(region->lifetime);
            gpu.shared_pins[gpu.shared_pin_count++] = region->lifetime;
        }
        *physical = region->host_phys_base + guest - region->guest_pa_base;
        return true;
    }
    return false;
}
const u64* map_table(void* context, u64 guest_page) {
    u64 physical = 0;
    // Page tables may only live in owned ordinary VM RAM, never SharedGpu.
    if ((guest_page & (PAGE_SIZE - 1)) || !translate_ram(context, guest_page, false, &physical)) {
        return nullptr;
    }
    return reinterpret_cast<const u64*>(physical);
}
u64* allocate(void*, u64* physical) {
    *physical = mm::alloc_page();
    return reinterpret_cast<u64*>(*physical);
}
void free_table(void*, u64* table) {
    mm::free_page(reinterpret_cast<u64>(table));
}
void publish(void*, u64* table, u32 bytes) {
    u64 ctr = 0;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 line = 4ULL << ((ctr >> 16) & 15);
    for (u64 p = reinterpret_cast<u64>(table); p < reinterpret_cast<u64>(table) + bytes;
         p += line) {
        asm volatile("dc cvac, %0" ::"r"(p) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
}
ShadowOps ops() {
    return {gpu.owner, nullptr, translate, allocate, free_table, publish, map_table};
}
void contain() {
    gpu.ready = false;
    gpu.quarantined = true;
    for (u32 irq : kHostIrqs) {
        drivers::gic::spi_disable(irq);
    }
    // Reset completion is the only permission to reclaim old physical tables.
    if (reset()) {
        gpu.enabled = false;
    }
    ANKO_LOG_ERROR("kernel/mali", "GPU quarantined; VM RAM and shadow tables pinned");
}
bool update() {
    const u32 next = gpu.active ^ 1;
    auto callbacks = ops();
    shadow_recycle(gpu.shadow[next]); // retired only after prior sync
    vm::note_progress(MX_VM_PROGRESS_SHADOW, gpu.guest_root);
    if ((gpu.guest_root & 3) != 3 ||
        !shadow_build(callbacks, gpu.guest_root & kPteAddressMask, gpu.shadow[next])) {
        return false;
    }
    vm::note_progress(MX_VM_PROGRESS_SHADOW_DONE, gpu.shadow[next].leaves, gpu.shadow[next].count);
    // Stop walks and drain accesses through the old tree before changing roots.
    if (!command(hw::kAsCmdFlushMem)) {
        return false;
    }
    write(hw::kBase, hw::kAsTranstabLo, static_cast<u32>(gpu.shadow[next].root) | 7);
    write(hw::kBase, hw::kAsTranstabHi, static_cast<u32>(gpu.shadow[next].root >> 32));
    write(hw::kBase, hw::kAsMemattrLo, hw::kMemattr);
    write(hw::kBase, hw::kAsMemattrHi, 0);
    if (!command(hw::kAsCmdUpdate)) {
        return false;
    }
    shadow_recycle(gpu.shadow[gpu.active]);
    gpu.active = next;
    gpu.enabled = true;
    return true;
}
} // namespace

SysError attach(Process& caller, CapHandle vmcap, CapHandle cpucap, CapHandle authority) {
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    auto cap = cap::lookup(*caller.cap_table, authority, CAP_PLATFORM);
    if (!cap || !(cap->rights_mask & RIGHT_WRITE) ||
        cap->target != platform::board_control(platform::Domain::Gpu)) {
        return E_PERM;
    }
    SysError rc = E_OK;
    auto owner = vm::vm_from_cap(caller, vmcap, RIGHT_VM_CONTROL, &rc);
    if (!owner) {
        return rc;
    }
    auto cpu = vm::vcpu_from_cap(caller, cpucap, RIGHT_VCPU_CONTROL, &rc);
    if (!cpu) {
        return rc;
    }
    if (cpu->owner_vm_id != owner->vm_id || owner->state == vm::VM_DEAD ||
        cpu->state == vm::VCPU_RUNNING || owner->vcpu_count != 1) {
        return E_INVALID;
    }
    kernel::LockGuard guard(lock);
    if (gpu.owner) {
        return E_BUSY;
    }
    KernelMap mapping;
    // The register window is the vendor DTS range; the mailbox window is one
    // 4 KiB page because the vendor also maps ipc@e896b000 as 0x1000.
    static_assert(hw::kBytes == AME_MALI_REG_BYTES,
                  "guest register window must equal the vendor range");
    if (!mm::map_kernel_device_mmio(hw::kBase, hw::kBytes) ||
        !mm::map_kernel_device_mmio(hw::kIpcBase, 4096)) {
        return E_NOMEM;
    }
    // SCTRL is already a Device identity mapping in the Anne bootstrap plan
    // (debug/reset support). Mapping it twice is rejected, not an allocation.
    gpu.owner = &*owner;
    gpu.cpu = &*cpu;
    // Hold both objects through IRQ injection and any quarantine retry.
    object::retain(gpu.owner->lifetime);
    object::retain(gpu.cpu->lifetime);
    for (u32 irq : kHostIrqs) {
        drivers::gic::spi_disable(irq);
    }
    // Identify the core before trusting anything else: a T830 reports
    // GPU_ID_PI_T83X in bits 16..31. A different core means this mediation
    // (one address space, T830 register layout) does not apply.
    if (!power(true) ||
        (read(hw::kBase, hw::kRegId) >> hw::kProductIdShift) != hw::kProductIdT830 || !reset()) {
        gpu.quarantined = true;
        return E_FAULT;
    }
    gpu.ready = true;
    gpu.quarantined = false;
    for (u32 irq : kHostIrqs) {
        drivers::gic::spi_configure(irq, 0, 0);
        drivers::gic::spi_clear_pending(irq);
        drivers::gic::spi_enable(irq);
    }
    ANKO_LOG_INFO("kernel/mali", "Mali-T830 assigned with private shadow MMU");
    return E_OK;
#else
    (void)caller;
    (void)vmcap;
    (void)cpucap;
    (void)authority;
    return E_NOTIMPL;
#endif
}

SysError performance(u32 mhz) {
    kernel::LockGuard guard(lock);
    if (!gpu.owner || !gpu.ready || gpu.quarantined) {
        return E_DEAD;
    }
    KernelMap mapping;
    auto observed = []() -> u32 {
        const u32 index = (read(hw::kSctrlBase, hw::kFrequencyState) >> 12) & 15;
        return index < sizeof(hw::kOperatingMhz) / sizeof(hw::kOperatingMhz[0])
                   ? hw::kOperatingMhz[index]
                   : 0;
    };
    if (!mhz) {
        return static_cast<SysError>(observed());
    }
    // Firmware owns voltage and thermal policy. Expose only lower vendor OPPs;
    // raw PLL, voltage and arbitrary mailbox writes are never delegated.
    if (mhz != 120 && mhz != 240 && mhz != 360 && mhz != 480) {
        return E_INVALID;
    }
    if (observed() == mhz) {
        return E_OK;
    }
    constexpr u32 b = hw::kClockMailbox * hw::kMailboxStride;
    constexpr u32 source = hw::kClockSource, destination = hw::kDestinationLpm3;
    write(hw::kIpcBase, hw::kMboxUnlock, hw::kMboxUnlockKey);
    if (!wait(hw::kIpcBase, b + hw::kMboxStatus, hw::kMboxStatusIdle, hw::kMboxStatusIdle,
              hw::kPowerTimeoutNs)) {
        return E_BUSY;
    }
    write(hw::kIpcBase, b + hw::kMboxSource, source);
    if (read(hw::kIpcBase, b + hw::kMboxSource) != source) {
        return E_BUSY;
    }
    write(hw::kIpcBase, b + hw::kMboxSourceRelease, ~(source | destination));
    write(hw::kIpcBase, b + hw::kMboxAckMask, destination);
    write(hw::kIpcBase, b + hw::kMboxStatus, 0);
    for (u32 i = 0; i < hw::kMboxDataWords; ++i) {
        write(hw::kIpcBase, b + hw::kMboxData + 4 * i,
              i == 0   ? hw::kClockSetRate
              : i == 1 ? mhz
                       : 0);
    }
    write(hw::kIpcBase, b + hw::kMboxTrigger, source);
    if (!wait(hw::kIpcBase, b + hw::kMboxStatus, hw::kMboxStatusAck, hw::kMboxStatusAck,
              hw::kPowerTimeoutNs)) {
        return E_BUSY;
    }
    write(hw::kIpcBase, b + hw::kMboxAck, source);
    write(hw::kIpcBase, b + hw::kMboxSourceRelease, ~0U);
    write(hw::kIpcBase, b + hw::kMboxSource, source);
    // Firmware may acknowledge the mailbox before committing SCBAKDATA.
    // Observe a bounded transition instead of reporting the previous OPP.
    const u64 started = sched::sched_clock_ns();
    for (u32 n = 0; n < hw::kPollLimit && observed() != mhz; ++n) {
        if (sched::sched_clock_ns() - started >= 20000000ull) {
            break;
        }
        asm volatile("yield");
    }
    const u32 actual = observed();
    ANKO_LOGF_INFO("kernel/mali", "GPU firmware DVFS requested=%u observed=%u MHz", mhz, actual);
    // A firmware ACK is not proof of a resulting rate: report the observed OPP.
    return actual == mhz ? E_OK : E_BUSY;
}

bool owns(const vm::Vm& owner) {
    return gpu.owner == &owner;
}

bool release(vm::Vm& owner) {
    kernel::LockGuard guard(lock);
    if (gpu.owner != &owner) {
        return true;
    }
    KernelMap mapping;
    for (u32 irq : kHostIrqs) {
        drivers::gic::spi_disable(irq);
    }
    gpu.ready = false;
    if (!reset()) {
        gpu.quarantined = true;
        return false;
    }
    auto callbacks = ops();
    shadow_destroy(callbacks, gpu.shadow[0]);
    shadow_destroy(callbacks, gpu.shadow[1]);
    // Reset has stopped DMA even if firmware refuses a subsequent power-off.
    const bool off = power(false);
    auto* cpu_lifetime = gpu.cpu->lifetime;
    auto* vm_lifetime = gpu.owner->lifetime;
    object::Record* pins[vm::kMaxSharedArenas]{};
    const u32 pin_count = gpu.shared_pin_count;
    for (u32 i = 0; i < pin_count; ++i) {
        pins[i] = gpu.shared_pins[i];
    }
    gpu = {};
    for (u32 i = 0; i < pin_count; ++i) {
        object::release(pins[i]);
    }
    object::release(cpu_lifetime);
    object::release(vm_lifetime);
    if (!off) {
        ANKO_LOG_ERROR("kernel/mali", "GPU reset; LPM3 power-off not acknowledged");
    }
    return true;
}

bool emulate(vm::Vm& owner, u64 ipa, u32 size, bool store, u64* value) {
    if (ipa < AME_MALI_GUEST_BASE || ipa >= AME_MALI_GUEST_BASE + AME_MALI_REG_BYTES) {
        return false;
    }
    vm::note_progress(MX_VM_PROGRESS_MALI_LOCK, ipa, store);
    kernel::LockGuard guard(lock);
    vm::note_progress(MX_VM_PROGRESS_MALI_LOCKED, ipa, store);
    if (gpu.owner != &owner || !gpu.ready || size != 4 || (ipa & 3)) {
        return false;
    }
    KernelMap mapping;
    const u32 off = ipa - AME_MALI_GUEST_BASE;
    const u32 data = static_cast<u32>(*value);
    if (!store) {
        // Fixed, host-owned identity of the synthetic device.
        // One slot is advertised. Panfrost allocates from this mask
        // (panfrost_mmu.c:192 `BIT(as) & features.as_present`), so it will keep
        // choosing AS0 while a single MMU/file is open. The shadow builder owns
        // exactly one root, so a second concurrent client that forced the
        // allocator onto AS1 would be contained rather than silently mis-mapped.
        if (off == hw::kRegAsPresent) {
            *value = 1;
        } else if (off == hw::kRegMmuFeatures) {
            *value = (read(hw::kBase, off) & ~hw::kMmuFeatureIasMask) | hw::kMmuInputAddressBits;
        } else if (off == hw::kAsTranstabLo) {
            *value = static_cast<u32>(gpu.guest_root);
        } else if (off == hw::kAsTranstabHi) {
            *value = static_cast<u32>(gpu.guest_root >> 32);
        }
        // The reset completion is latched here, so a guest poll cannot miss it.
        // Other bits are the hardware's own and are reported unchanged.
        else if (off == hw::kRegIntRawstat) {
            *value = read(hw::kBase, off) | gpu.synthetic_rawstat;
        } else {
            *value = read(hw::kBase, off);
        }
        return true;
    }
    if (off >= hw::kMmuBase && off < hw::kMmuBase + hw::kMmuBytes) {
        // Only the single advertised address space is reachable; a write into
        // any other slot would let the guest retarget a physical MMU root.
        if (off >= hw::kAsOffset + hw::kAsStride) {
            contain();
            return false;
        }
        if (off == hw::kAsTranstabLo) {
            gpu.guest_root = (gpu.guest_root & ~kWordMask) | data;
        } else if (off == hw::kAsTranstabHi) {
            gpu.guest_root = (gpu.guest_root & kWordMask) | (u64(data) << 32);
        } else if (off == hw::kAsMemattrLo ||
                   off == hw::kAsMemattrHi) { /* host canonical MEMATTR */
        } else if (off == hw::kAsCommand) {
            if (data == hw::kAsCmdUpdate || data == hw::kAsCmdFlush || data == hw::kAsCmdFlushMem) {
                if (!gpu.guest_root) {
                    if (!command(hw::kAsCmdFlushMem)) {
                        contain();
                        return false;
                    }
                    write(hw::kBase, hw::kAsTranstabLo, 0);
                    write(hw::kBase, hw::kAsTranstabHi, 0);
                    if (!command(hw::kAsCmdUpdate)) {
                        contain();
                        return false;
                    }
                    gpu.enabled = false;
                } else if (!update()) {
                    contain();
                    return false;
                }
            } else if (data <= hw::kAsCmdUnlock) {
                if (!command(data)) {
                    contain();
                    return false;
                }
            } else {
                contain();
                return false;
            }
        } else if (off == hw::kAsLockaddrLo || off == hw::kAsLockaddrHi) {
            write(hw::kBase, off, data);
        } else {
            contain();
            return false;
        }
        return true;
    }
    // Never permit DMA-producing operations with MMU bypass or another AS.
    if (off >= hw::kJsBase && off < hw::kJsBase + hw::kJsSlotBytes) {
        const u32 reg = (off - hw::kJsBase) & hw::kJsRegisterMask;
        // The slot must target the one address space this VM owns. START_MMU is
        // deliberately not required: it is the HW_ISSUE_10649 workaround, which
        // a T830 does not carry, so demanding it would reject honest jobs.
        if ((reg == hw::kJsRegConfig || reg == hw::kJsRegConfigNext) &&
            (data & hw::kJsConfigAsMask)) {
            contain();
            return false;
        }
        // A slot may not be started while no shadow root is published.
        if ((reg == hw::kJsRegCommand || reg == hw::kJsRegCommandNext) &&
            data == hw::kJsCommandStart && !gpu.enabled) {
            contain();
            return false;
        }
    } else if (off == hw::kRegCmd) {
        if (data == hw::kCmdSoftReset || data == hw::kCmdHardReset) {
            if (!reset()) {
                contain();
                return false;
            }
            gpu.enabled = false;
            gpu.guest_root = 0;
            return true;
        }
        if (data != hw::kCmdPerfcntClear && data != hw::kCmdPerfcntSample &&
            data != hw::kCmdCycleCountStart && data != hw::kCmdCycleCountStop &&
            data != hw::kCmdCleanCaches && data != hw::kCmdCleanInvCaches) {
            return false;
        }
    } else if (off >= hw::kRegPerfcntLo && off <= hw::kRegPerfcntHi) {
        // Perf-counter DMA is deliberately unavailable to this appliance.
        if (data) {
            return false;
        }
    } else if (off == hw::kRegIntClear) {
        // Write-one-to-clear, exactly as the hardware behaves: the guest must be
        // able to retire the completion it just observed.
        gpu.synthetic_rawstat &= ~data;
        write(hw::kBase, off, data);
        return true;
    } else {
        // Explicit register allow-list: interrupt controls, power requests and
        // Panfrost's documented core-quirk registers. Other writes are denied.
        const bool allowed = off == hw::kRegIntClear || off == hw::kRegIntMask ||
                             off == hw::kRegPwrKey || off == hw::kRegPwrOverride1 ||
                             off == hw::kRegJobIntClear || off == hw::kRegJobIntMask ||
                             off == hw::kRegMmuIntClear || off == hw::kRegMmuIntMask ||
                             off == hw::kRegJmConfig || off == hw::kRegShaderConfig ||
                             off == hw::kRegTilerConfig || off == hw::kRegL2MmuConfig ||
                             (off >= hw::kRegCorePowerLo && off <= hw::kRegCorePowerHi);
        if (!allowed) {
            return false;
        }
    }
    write(hw::kBase, off, data);
    return true;
}

void deliver(u32 host_irq) {
    kernel::LockGuard guard(lock);
    if (!gpu.ready || !gpu.cpu) {
        return;
    }
    for (u32 i = 0; i < 3; ++i) {
        if (host_irq == kHostIrqs[i]) {
            if (gpu.suppressed[i]) {
                return;
            }
            drivers::gic::spi_disable(host_irq);
            gpu.suppressed[i] = true;
            vm::vgic_pending_set(*gpu.cpu, kGuestIrqs[i]);
            vm::vcpu_wake_irq(*gpu.cpu);
        }
    }
}
void guest_eoi(vm::Vcpu& cpu, u32 guest_irq) {
    kernel::LockGuard guard(lock);
    if (!gpu.ready || gpu.cpu != &cpu) {
        return;
    }
    for (u32 i = 0; i < 3; ++i) {
        if (guest_irq == kGuestIrqs[i] && gpu.suppressed[i]) {
            gpu.suppressed[i] = false;
            drivers::gic::spi_enable(kHostIrqs[i]);
        }
    }
}
} // namespace mali
