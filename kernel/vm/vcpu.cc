#include "anko/vm.h"
#include "anko/smmuv3.h"
#include "anko/platform.h"
#include "anko/log.h"
#include "anko/console.h"
#include "libk/printf.h"
#include "libk/string.h"
#include "anko/arm64_esr.h"

#include "anko/el2_boot.h"
extern "C" u64 vm_el2_run(vm::El2RunState* state);
extern "C" void irq_dispatch();

namespace vm {

namespace {

u32 g_next_vcpu_id = 1;

constexpr u64 kHcrExitUnsupported = 0;
using namespace arm64;
constexpr u64 kQemuUartBase = 0x0900'0000ull;
constexpr u64 kQemuGicDistBase = 0x0800'0000ull;
constexpr u64 kQemuGicCpuBase = 0x0801'0000ull;
constexpr u64 kMmioPageSize = 0x1000;

constexpr u32 kGicSpuriousIrq = 1023;
constexpr u32 kGuestPhysTimerIrq = 30;
constexpr u32 kGuestVirtTimerIrq = 27;

constexpr u32 kPsciVersion32 = 0x84000000u;
constexpr u32 kPsciCpuSuspend32 = 0x84000001u;
constexpr u32 kPsciCpuOff32 = 0x84000002u;
constexpr u32 kPsciCpuOn32 = 0x84000003u;
constexpr u32 kPsciAffinityInfo32 = 0x84000004u;
constexpr u32 kPsciMigrateInfoType32 = 0x84000006u;
constexpr u32 kPsciSystemOff32 = 0x84000008u;
constexpr u32 kPsciSystemReset32 = 0x84000009u;
constexpr u32 kPsciFeatures32 = 0x8400000Au;
constexpr u32 kPsciCpuSuspend64 = 0xC4000001u;
constexpr u32 kPsciCpuOn64 = 0xC4000003u;
constexpr u32 kPsciAffinityInfo64 = 0xC4000004u;

constexpr i64 kPsciSuccess = 0;
constexpr i64 kPsciNotSupported = -1;
constexpr i64 kPsciInvalidParameters = -2;
constexpr i64 kPsciAlreadyOn = -4;

enum class PsciAction : u32 {
    NotHandled = 0,
    Continue = 1,
    StopCurrent = 2,
    SuspendCurrent = 3,
    ShutdownVm = 4,
};

constexpr u32 GICD_CTLR       = 0x000;
constexpr u32 GICD_TYPER      = 0x004;
constexpr u32 GICD_IIDR       = 0x008;
constexpr u32 GICD_IGROUPR    = 0x080;
constexpr u32 GICD_ISENABLER  = 0x100;
constexpr u32 GICD_ICENABLER  = 0x180;
constexpr u32 GICD_ISPENDR    = 0x200;
constexpr u32 GICD_ICPENDR    = 0x280;
constexpr u32 GICD_ISACTIVER  = 0x300;
constexpr u32 GICD_ICACTIVER  = 0x380;
constexpr u32 GICD_IPRIORITYR = 0x400;
constexpr u32 GICD_ITARGETSR  = 0x800;
constexpr u32 GICD_ICFGR      = 0xC00;
constexpr u32 GICD_SGIR       = 0xF00;

constexpr u32 GICC_CTLR  = 0x000;
constexpr u32 GICC_PMR   = 0x004;
constexpr u32 GICC_BPR   = 0x008;
constexpr u32 GICC_IAR   = 0x00C;
constexpr u32 GICC_EOIR  = 0x010;
constexpr u32 GICC_RPR   = 0x014;
constexpr u32 GICC_HPPIR = 0x018;
constexpr u32 GICC_IIDR  = 0x0FC;
constexpr u32 GICC_DIR   = 0x1000;

u64 read_cntpct() {
    u64 value = 0;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(value));
    return value;
}

bool timer_enabled_unmasked(u64 ctl) {
    return (ctl & 0x1ull) && ((ctl & 0x2ull) == 0);
}

// ESR_EL2.ISS bit 0 distinguishes trapped WFE from WFI for EC=0x01.
// WFI is now normally allowed to sleep in the guest so the real timer wakes
// EL2 precisely; WFE remains trapped and is treated as a yield/nop.
bool wfx_is_wfe(u64 esr) {
    return (esr_iss(esr) & 1ull) != 0;
}

u64 data_abort_ipa(const El2RunState& state) {
    return ((state.exit_hpfar & 0xFFFF'FFFF'FFFF'F0ull) << 8) |
           (state.exit_far & 0xFFFull);
}

u64 fault_ipa(const El2RunState& state, u64 ec) {
    if (ec == kEcDataAbortLower || ec == kEcDataAbortCurrent ||
        ec == kEcInstrAbortLower || ec == kEcInstrAbortCurrent) {
        return ((state.exit_hpfar & 0xFFFF'FFFF'FFFF'F0ull) << 8) |
               (state.exit_far & 0xFFFull);
    }
    return 0;
}

u64 read_reg(const El2RunState& state, u32 reg) {
    if (reg >= 31) return 0;
    return state.regs[reg];
}

void write_reg(El2RunState& state, u32 reg, u64 value, u32 size) {
    if (reg >= 31) return;
    if (size <= 4) value &= 0xFFFF'FFFFull;
    state.regs[reg] = value;
}

void write_x0(El2RunState& state, i64 value) {
    state.regs[0] = static_cast<u64>(value);
}

u64 mask_width(u32 size) {
    if (size >= 8) return ~0ull;
    return (1ull << (size * 8)) - 1ull;
}

u64 pl011_read(u64 offset) {
    switch (offset) {
    case 0x000: return 0;
    case 0x018: return (1u << 7) | (1u << 4);
    case 0x024: return 0;
    case 0x028: return 0;
    case 0x02C: return 0;
    case 0x030: return 0;
    case 0x038: return 0;
    case 0xFE0: return 0x11;
    case 0xFE4: return 0x10;
    case 0xFE8: return 0x14;
    case 0xFEC: return 0x00;
    case 0xFF0: return 0x0D;
    case 0xFF4: return 0xF0;
    case 0xFF8: return 0x05;
    case 0xFFC: return 0xB1;
    default: return 0;
    }
}

void note_guest_console_char(char c) {
    static char line[96];
    static u32 len = 0;
    static bool saw_boot_stub = false;
    static bool saw_agent_start = false;
    static bool saw_linux_hello = false;

    if (c == '\r') return;

    if (c == '\n') {
        line[len] = '\0';
        if (!saw_boot_stub && libk::strcmp(line, "dango: vm boot stub reached") == 0) {
            saw_boot_stub = true;
            metric_inc(VM_DEBUG_GUEST_BOOT_STUB);
            ANKO_LOG_RAW("guest boot stub reached\n");
        }
        if (!saw_agent_start && libk::strcmp(line, "dango-agent: started") == 0) {
            saw_agent_start = true;
            metric_inc(VM_DEBUG_GUEST_AGENT_START);
            ANKO_LOG_RAW("guest agent started\n");
        }
        if (!saw_linux_hello && libk::strcmp(line, "dango: linux guest responded") == 0) {
            saw_linux_hello = true;
            metric_inc(VM_DEBUG_GUEST_LINUX_HELLO);
            ANKO_LOG_RAW("guest uart hello\n");
        }
        if (libk::strcmp(line, "dango: gpu probe passed") == 0 ||
            libk::strcmp(line, "dango: channel test passed") == 0) {
            (void)iommu::smmuv3_drain_events();
            ANKO_LOGF_INFO("kernel/vm", "%s proof: faults=%llu quarantines=%llu routes=%llu injected=%llu smmu=%u",
                libk::strcmp(line, "dango: channel test passed") == 0 ? "channel" : "gpu",
                static_cast<unsigned long long>(iommu::debug_counter(iommu::IOMMU_FAULTS)),
                static_cast<unsigned long long>(iommu::debug_counter(iommu::IOMMU_QUARANTINES)),
                static_cast<unsigned long long>(device_irq_route_count()),
                static_cast<unsigned long long>(debug_counter(VM_DEBUG_VM_DEVICE_IRQ_INJECTED)),
                iommu::smmuv3_ready() ? 1u : 0u);
        }
        len = 0;
        return;
    }

    if (len + 1 < sizeof(line)) {
        line[len++] = c;
    } else {
        len = 0;
    }
}

void guest_console_putchar(char c) {
    // Forward to the kernel console (g_console_ops).
    // kGuestUartBase (0x09040000 = QEMU secure UART) is only present with
    // -machine virt,secure=on; without it, direct MMIO causes an external abort.
    console::putchar(c);
}

void pl011_write(u64 offset, u64 value) {
    if (offset == 0x000) {
        const char c = static_cast<char>(value & 0xFFu);
        guest_console_putchar(c);
        note_guest_console_char(c);
    }
}

u32 lane_shift(u64 offset) {
    return static_cast<u32>((offset & 0x3ull) * 8ull);
}

u32 sized_write_bits(u64 offset, u32 size, u64 value) {
    const u32 shift = lane_shift(offset);
    if (size >= 4) return static_cast<u32>(value);
    const u32 bits = static_cast<u32>(value) & ((1u << (size * 8u)) - 1u);
    return bits << shift;
}

u32 merge_sized_write(u32 old_value, u64 offset, u32 size, u64 value) {
    const u32 shift = lane_shift(offset);
    if (size >= 4) return static_cast<u32>(value);
    const u32 mask = ((1u << (size * 8u)) - 1u) << shift;
    const u32 bits = (static_cast<u32>(value) << shift) & mask;
    return (old_value & ~mask) | bits;
}

u64 extract_sized_read(u32 value, u64 offset, u32 size) {
    const u32 shift = lane_shift(offset);
    if (size >= 4) return value;
    const u32 mask = (1u << (size * 8u)) - 1u;
    return (value >> shift) & mask;
}

u32 bitmap_word_index(u32 irq) { return irq / 32u; }
u32 bitmap_mask(u32 irq) { return 1u << (irq % 32u); }
bool irq_in_range(u32 irq) { return irq < kVgicMaxIrqs; }

bool vgic_bit_test(const u32* bitmap, u32 irq) {
    return irq_in_range(irq) && ((__atomic_load_n(&bitmap[bitmap_word_index(irq)], __ATOMIC_ACQUIRE) & bitmap_mask(irq)) != 0);
}

void vgic_bit_set(u32* bitmap, u32 irq) {
    if (irq_in_range(irq)) __atomic_fetch_or(&bitmap[bitmap_word_index(irq)], bitmap_mask(irq), __ATOMIC_RELEASE);
}

void vgic_bit_clear(u32* bitmap, u32 irq) {
    if (irq_in_range(irq)) __atomic_fetch_and(&bitmap[bitmap_word_index(irq)], ~bitmap_mask(irq), __ATOMIC_ACQ_REL);
}

u32 vgic_bitmap_word(const u32* bitmap, u32 word) {
    return (word < kVgicBitmapWords) ? __atomic_load_n(&bitmap[word], __ATOMIC_ACQUIRE) : 0;
}

void vgic_init(VgicState& gic) {
    libk::memset(&gic, 0, sizeof(gic));
    gic.cpu_pmr = 0xFF;
    gic.last_iar = kGicSpuriousIrq;
    for (u32 i = 0; i < kVgicMaxIrqs; ++i) {
        gic.priority[i] = 0xA0;
        gic.targets[i] = 0x01;
    }
}

bool vgic_irq_enabled(const VgicState& gic, u32 irq) {
    return irq_in_range(irq) &&
           ((gic.dist_ctlr & 1u) != 0) &&
           ((gic.cpu_ctlr & 1u) != 0) &&
           vgic_bit_test(gic.enabled, irq) &&
           vgic_bit_test(gic.pending, irq) &&
           !vgic_bit_test(gic.active, irq) &&
           gic.priority[irq] <= gic.cpu_pmr;
}

u32 vgic_peek_irq(const VgicState& gic) {
    u32 best_irq = kGicSpuriousIrq;
    u8 best_priority = 0xFF;
    for (u32 irq = 0; irq < kVgicMaxIrqs; ++irq) {
        if (!vgic_irq_enabled(gic, irq)) continue;
        const u8 priority = gic.priority[irq];
        if (best_irq == kGicSpuriousIrq || priority < best_priority) {
            best_irq = irq;
            best_priority = priority;
        }
    }
    return best_irq;
}

void vgic_set_level(Vcpu& vcpu, u32 irq, bool asserted) {
    if (!irq_in_range(irq)) return;
    if (asserted) {
        vgic_bit_set(vcpu.vgic.level, irq);
        if (!vgic_bit_test(vcpu.vgic.active, irq)) vgic_bit_set(vcpu.vgic.pending, irq);
    } else {
        vgic_bit_clear(vcpu.vgic.level, irq);
        if (!vgic_bit_test(vcpu.vgic.active, irq)) vgic_bit_clear(vcpu.vgic.pending, irq);
    }
}

u32 vgic_ack_irq(Vcpu& vcpu) {
    const u32 irq = vgic_peek_irq(vcpu.vgic);
    if (irq == kGicSpuriousIrq) {
        vcpu.vgic.last_iar = kGicSpuriousIrq;
        return kGicSpuriousIrq;
    }
    vgic_bit_clear(vcpu.vgic.pending, irq);
    vgic_bit_set(vcpu.vgic.active, irq);
    vcpu.vgic.last_iar = irq;
    return irq;
}

void vgic_deactivate_irq(Vcpu& vcpu, u32 irq) {
    if (!irq_in_range(irq)) return;
    vgic_bit_clear(vcpu.vgic.active, irq);
    if (vgic_bit_test(vcpu.vgic.level, irq)) vgic_bit_set(vcpu.vgic.pending, irq);
    // §7.4: a level route suppresses host delivery after injecting the virtual
    // IRQ; the guest's EOI re-arms it. Device-assignment routes are notified here.
    device_irq_on_guest_eoi(vcpu, irq);
}

u32 priority_word(const VgicState& gic, u32 index) {
    const u32 base = index * 4u;
    u32 value = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u32 irq = base + i;
        value |= static_cast<u32>(irq_in_range(irq) ? gic.priority[irq] : 0) << (i * 8u);
    }
    return value;
}

void priority_word_write(VgicState& gic, u32 index, u64 offset, u32 size, u64 value) {
    const u32 merged = merge_sized_write(priority_word(gic, index), offset, size, value);
    const u32 base = index * 4u;
    for (u32 i = 0; i < 4; ++i) {
        const u32 irq = base + i;
        if (irq_in_range(irq)) gic.priority[irq] = static_cast<u8>((merged >> (i * 8u)) & 0xFFu);
    }
}

u32 target_word(const VgicState& gic, u32 index) {
    const u32 base = index * 4u;
    u32 value = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u32 irq = base + i;
        value |= static_cast<u32>(irq_in_range(irq) ? gic.targets[irq] : 0) << (i * 8u);
    }
    return value;
}

void target_word_write(VgicState& gic, u32 index, u64 offset, u32 size, u64 value) {
    const u32 merged = merge_sized_write(target_word(gic, index), offset, size, value);
    const u32 base = index * 4u;
    for (u32 i = 0; i < 4; ++i) {
        const u32 irq = base + i;
        if (irq_in_range(irq)) gic.targets[irq] = static_cast<u8>((merged >> (i * 8u)) & 0xFFu);
    }
}

u64 gic_dist_read(const Vcpu& vcpu, u64 offset, u32 size) {
    const VgicState& gic = vcpu.vgic;
    u32 word = 0;
    if (offset >= GICD_CTLR && offset < GICD_CTLR + 4) {
        word = gic.dist_ctlr & 1u;
    } else if (offset >= GICD_TYPER && offset < GICD_TYPER + 4) {
        word = 1u; // 64 interrupts, one CPU
    } else if (offset >= GICD_IIDR && offset < GICD_IIDR + 4) {
        word = 0x0200'143Bull;
    } else if (offset >= GICD_IGROUPR && offset < GICD_IGROUPR + 0x80) {
        word = vgic_bitmap_word(gic.group, static_cast<u32>((offset - GICD_IGROUPR) / 4));
    } else if (offset >= GICD_ISENABLER && offset < GICD_ISENABLER + 0x80) {
        word = vgic_bitmap_word(gic.enabled, static_cast<u32>((offset - GICD_ISENABLER) / 4));
    } else if (offset >= GICD_ICENABLER && offset < GICD_ICENABLER + 0x80) {
        word = vgic_bitmap_word(gic.enabled, static_cast<u32>((offset - GICD_ICENABLER) / 4));
    } else if (offset >= GICD_ISPENDR && offset < GICD_ISPENDR + 0x80) {
        word = vgic_bitmap_word(gic.pending, static_cast<u32>((offset - GICD_ISPENDR) / 4));
    } else if (offset >= GICD_ICPENDR && offset < GICD_ICPENDR + 0x80) {
        word = vgic_bitmap_word(gic.pending, static_cast<u32>((offset - GICD_ICPENDR) / 4));
    } else if (offset >= GICD_ISACTIVER && offset < GICD_ISACTIVER + 0x80) {
        word = vgic_bitmap_word(gic.active, static_cast<u32>((offset - GICD_ISACTIVER) / 4));
    } else if (offset >= GICD_ICACTIVER && offset < GICD_ICACTIVER + 0x80) {
        word = vgic_bitmap_word(gic.active, static_cast<u32>((offset - GICD_ICACTIVER) / 4));
    } else if (offset >= GICD_IPRIORITYR && offset < GICD_IPRIORITYR + 0x400) {
        word = priority_word(gic, static_cast<u32>((offset - GICD_IPRIORITYR) / 4));
    } else if (offset >= GICD_ITARGETSR && offset < GICD_ITARGETSR + 0x400) {
        word = target_word(gic, static_cast<u32>((offset - GICD_ITARGETSR) / 4));
    } else if (offset >= GICD_ICFGR && offset < GICD_ICFGR + 0x100) {
        const u32 index = static_cast<u32>((offset - GICD_ICFGR) / 4);
        word = (index < (kVgicMaxIrqs / 16)) ? gic.config[index] : 0;
    }
    return extract_sized_read(word, offset, size);
}

void gic_dist_write(Vcpu& vcpu, u64 offset, u32 size, u64 value) {
    VgicState& gic = vcpu.vgic;
    const u32 bits = sized_write_bits(offset, size, value);
    if (offset >= GICD_CTLR && offset < GICD_CTLR + 4) {
        gic.dist_ctlr = merge_sized_write(gic.dist_ctlr, offset, size, value) & 1u;
    } else if (offset >= GICD_IGROUPR && offset < GICD_IGROUPR + 0x80) {
        const u32 word = static_cast<u32>((offset - GICD_IGROUPR) / 4);
        if (word < kVgicBitmapWords) gic.group[word] = merge_sized_write(gic.group[word], offset, size, value);
    } else if (offset >= GICD_ISENABLER && offset < GICD_ISENABLER + 0x80) {
        const u32 word = static_cast<u32>((offset - GICD_ISENABLER) / 4);
        if (word < kVgicBitmapWords) gic.enabled[word] |= bits;
    } else if (offset >= GICD_ICENABLER && offset < GICD_ICENABLER + 0x80) {
        const u32 word = static_cast<u32>((offset - GICD_ICENABLER) / 4);
        if (word < kVgicBitmapWords) gic.enabled[word] &= ~bits;
    } else if (offset >= GICD_ISPENDR && offset < GICD_ISPENDR + 0x80) {
        const u32 word = static_cast<u32>((offset - GICD_ISPENDR) / 4);
        if (word < kVgicBitmapWords) __atomic_fetch_or(&gic.pending[word], bits, __ATOMIC_RELEASE);
    } else if (offset >= GICD_ICPENDR && offset < GICD_ICPENDR + 0x80) {
        const u32 word = static_cast<u32>((offset - GICD_ICPENDR) / 4);
        if (word < kVgicBitmapWords) __atomic_fetch_and(&gic.pending[word], ~bits, __ATOMIC_ACQ_REL);
    } else if (offset >= GICD_ISACTIVER && offset < GICD_ISACTIVER + 0x80) {
        const u32 word = static_cast<u32>((offset - GICD_ISACTIVER) / 4);
        if (word < kVgicBitmapWords) gic.active[word] |= bits;
    } else if (offset >= GICD_ICACTIVER && offset < GICD_ICACTIVER + 0x80) {
        const u32 word = static_cast<u32>((offset - GICD_ICACTIVER) / 4);
        if (word < kVgicBitmapWords) gic.active[word] &= ~bits;
    } else if (offset >= GICD_IPRIORITYR && offset < GICD_IPRIORITYR + 0x400) {
        priority_word_write(gic, static_cast<u32>((offset - GICD_IPRIORITYR) / 4), offset, size, value);
    } else if (offset >= GICD_ITARGETSR && offset < GICD_ITARGETSR + 0x400) {
        target_word_write(gic, static_cast<u32>((offset - GICD_ITARGETSR) / 4), offset, size, value);
    } else if (offset >= GICD_ICFGR && offset < GICD_ICFGR + 0x100) {
        const u32 index = static_cast<u32>((offset - GICD_ICFGR) / 4);
        if (index < (kVgicMaxIrqs / 16)) gic.config[index] = merge_sized_write(gic.config[index], offset, size, value);
    } else if (offset >= GICD_SGIR && offset < GICD_SGIR + 4) {
        const u32 sgi = static_cast<u32>(value) & 0xFu;
        const u32 target_list = (static_cast<u32>(value) >> 16) & 0xFFu;
        const u32 target_filter = (static_cast<u32>(value) >> 24) & 0x3u;
        if (sgi < 16 && (target_filter == 1 || (target_list & 0x01u))) vgic_bit_set(gic.pending, sgi);
    }
}

u64 gic_cpu_read(Vcpu& vcpu, u64 offset, u32 size) {
    VgicState& gic = vcpu.vgic;
    u32 word = 0;
    switch (offset & ~0x3ull) {
    case GICC_CTLR:  word = gic.cpu_ctlr & 1u; break;
    case GICC_PMR:   word = gic.cpu_pmr; break;
    case GICC_BPR:   word = gic.cpu_bpr; break;
    case GICC_IAR:   word = vgic_ack_irq(vcpu); break;
    case GICC_RPR:   word = 0xFF; break;
    case GICC_HPPIR: word = vgic_peek_irq(gic); break;
    case GICC_IIDR:  word = 0x0200'143Bull; break;
    default:         word = 0; break;
    }
    return extract_sized_read(word, offset, size);
}

void gic_cpu_write(Vcpu& vcpu, u64 offset, u32 size, u64 value) {
    VgicState& gic = vcpu.vgic;
    switch (offset & ~0x3ull) {
    case GICC_CTLR: gic.cpu_ctlr = merge_sized_write(gic.cpu_ctlr, offset, size, value) & 1u; break;
    case GICC_PMR:  gic.cpu_pmr = merge_sized_write(gic.cpu_pmr, offset, size, value) & 0xFFu; break;
    case GICC_BPR:  gic.cpu_bpr = merge_sized_write(gic.cpu_bpr, offset, size, value) & 0x7u; break;
    case GICC_EOIR:
    case GICC_DIR:  vgic_deactivate_irq(vcpu, static_cast<u32>(value) & 0x3FFu); break;
    default: break;
    }
}

bool emulate_mmio(Vcpu& vcpu, Vm& owner, u64 ipa, u64 esr) {
    arm64::MmioAccess access{};
    if (!arm64::decode_mmio(esr, &access) || (ipa & (access.size - 1))) return false;
    const u32 size = access.size;
    const u32 reg = access.reg;
    const bool write = access.write;
    u64 value = write ? read_reg(vcpu.el2, reg) & mask_width(size) : 0;
    bool handled = false;
    if (device_emulate_ecam(owner, ipa, size, write, &value)) {
        handled = true;
    } else if (ipa >= kQemuUartBase && ipa < kQemuUartBase + kMmioPageSize) {
        const u64 offset = ipa - kQemuUartBase;
        if (write) pl011_write(offset, read_reg(vcpu.el2, reg) & mask_width(size));
        else value = pl011_read(offset);
        handled = true;
    } else if (ipa >= kQemuGicDistBase && ipa < kQemuGicDistBase + 0x10000) {
        const u64 offset = ipa - kQemuGicDistBase;
        if (!write) value = gic_dist_read(vcpu, offset, size);
        else gic_dist_write(vcpu, offset, size, read_reg(vcpu.el2, reg) & mask_width(size));
        handled = true;
    } else if (ipa >= kQemuGicCpuBase && ipa < kQemuGicCpuBase + 0x10000) {
        const u64 offset = ipa - kQemuGicCpuBase;
        if (!write) value = gic_cpu_read(vcpu, offset, size);
        else gic_cpu_write(vcpu, offset, size, read_reg(vcpu.el2, reg) & mask_width(size));
        handled = true;
    }
    if (!handled) return false;
    if (!write) write_reg(vcpu.el2, reg, arm64::mmio_load_value(access, value), access.wide ? 8 : 4);
    vcpu.el2.pc += 4;
    return true;
}


bool psci_function_supported(u32 fn) {
    switch (fn) {
    case kPsciVersion32:
    case kPsciCpuOff32:
    case kPsciCpuSuspend32:
    case kPsciCpuSuspend64:
    case kPsciCpuOn32:
    case kPsciCpuOn64:
    case kPsciAffinityInfo32:
    case kPsciAffinityInfo64:
    case kPsciMigrateInfoType32:
    case kPsciSystemOff32:
    case kPsciSystemReset32:
    case kPsciFeatures32:
        return true;
    default:
        return false;
    }
}

Vcpu* find_vcpu_by_mpidr(Vm& owner, u64 mpidr) {
    const u64 aff = mpidr & 0x00FF'FFFFull;
    for (u32 i = 0; i < owner.vcpu_count; ++i) {
        Vcpu* candidate = owner.vcpus[i];
        if (!candidate || candidate->state == VCPU_DEAD) continue;
        if ((candidate->virtual_mpidr & 0x00FF'FFFFull) == aff) return candidate;
    }
    return nullptr;
}

void sync_vcpu_from_el2(Vcpu& vcpu);
void wake_bound_runner(Vcpu& vcpu, WakeReason reason, u32 hints);

void reset_target_vcpu_for_psci_on(Vcpu& target, u64 entry, u64 context_id) {
    target.entry_pc = entry;
    target.arg0 = context_id;
    target.arg1 = 0;
    target.sp = 0;
    target.pc = entry;
    target.pstate = 0x3C5;
    for (u32 i = 0; i < 31; ++i) target.regs[i] = 0;
    target.regs[0] = context_id;
    libk::memset(&target.el2, 0, sizeof(target.el2));
    target.state = VCPU_READY;
    target.sched.blocked_wfi = false;
    target.sched.blocked_reason = VCPU_BLOCK_NONE;
    target.sched.virtual_irq_pending = true;
}

PsciAction emulate_psci(Vcpu& vcpu, Vm& owner, u64 ec) {
    if (ec != kEcHvc64 && ec != kEcSmc64) return PsciAction::NotHandled;

    const u32 fn = static_cast<u32>(vcpu.el2.regs[0]);
    switch (fn) {
    case kPsciVersion32:
        /*
         * Advertise PSCI 0.2, not 1.0.  Anko implements the mandatory 0.2
         * calls needed by Linux for boot/reset/hotplug.  Do not claim 1.0
         * until the optional 1.0 feature-discovery surface is complete.
         */
        write_x0(vcpu.el2, 0x0000'0002);
        break;

    case kPsciFeatures32: {
        const u32 queried = static_cast<u32>(vcpu.el2.regs[1]);
        write_x0(vcpu.el2, psci_function_supported(queried) ? kPsciSuccess : kPsciNotSupported);
        break;
    }

    case kPsciMigrateInfoType32:
        // Trusted OS not present / migration not required.
        write_x0(vcpu.el2, 2);
        break;

    case kPsciAffinityInfo32:
    case kPsciAffinityInfo64: {
        const u64 target_mpidr = vcpu.el2.regs[1];
        Vcpu* target = find_vcpu_by_mpidr(owner, target_mpidr);
        if (!target) {
            write_x0(vcpu.el2, kPsciInvalidParameters);
        } else {
            const bool on = target->state == VCPU_READY ||
                            target->state == VCPU_CONFIGURED ||
                            target->state == VCPU_RUNNING;
            write_x0(vcpu.el2, on ? 0 : 1);
        }
        break;
    }

    case kPsciCpuOn32:
    case kPsciCpuOn64: {
        const u64 target_mpidr = vcpu.el2.regs[1];
        const u64 entry = vcpu.el2.regs[2];
        const u64 context_id = vcpu.el2.regs[3];
        if ((entry & 0x3ull) != 0) {
            write_x0(vcpu.el2, kPsciInvalidParameters);
            break;
        }
        Vcpu* target = find_vcpu_by_mpidr(owner, target_mpidr);
        if (!target) {
            write_x0(vcpu.el2, kPsciInvalidParameters);
        } else if (target == &vcpu || target->state == VCPU_READY ||
                   target->state == VCPU_CONFIGURED || target->state == VCPU_RUNNING) {
            write_x0(vcpu.el2, kPsciAlreadyOn);
        } else {
            reset_target_vcpu_for_psci_on(*target, entry, context_id);
            wake_bound_runner(*target, WAKE_IRQ, IPC_HINT_LATENCY);
            write_x0(vcpu.el2, kPsciSuccess);
        }
        break;
    }

    case kPsciCpuOff32:
        write_x0(vcpu.el2, kPsciSuccess);
        return PsciAction::StopCurrent;

    case kPsciSystemOff32:
    case kPsciSystemReset32:
        write_x0(vcpu.el2, kPsciSuccess);
        return PsciAction::ShutdownVm;

    case kPsciCpuSuspend32:
    case kPsciCpuSuspend64:
        write_x0(vcpu.el2, kPsciSuccess);
        return PsciAction::SuspendCurrent;

    default:
        return PsciAction::NotHandled;
    }

    /*
     * For AArch64 HVC/SMC, ELR_EL2 already contains the architectural
     * preferred return address: the instruction after the trapping call.
     * Advancing it here skips one guest instruction and corrupts Linux's
     * PSCI probe path.
     */
    return PsciAction::Continue;
}

void log_sysreg_trap(const Vcpu& vcpu, u64 esr) {
    const u64 iss = esr_iss(esr);
    const u32 op0 = static_cast<u32>((iss >> 20) & 0x3ull);
    const u32 op2 = static_cast<u32>((iss >> 17) & 0x7ull);
    const u32 op1 = static_cast<u32>((iss >> 14) & 0x7ull);
    const u32 crn = static_cast<u32>((iss >> 10) & 0xFull);
    const u32 rt  = static_cast<u32>((iss >> 5)  & 0x1Full);
    const u32 crm = static_cast<u32>((iss >> 1)  & 0xFull);
    const bool read = (iss & 1ull) != 0;
    libk::printf("dango sysreg trap %s op0=%u op1=%u crn=%u crm=%u op2=%u rt=%u pc=%llx esr=%llx\n",
                 read ? "read" : "write", op0, op1, crn, crm, op2, rt,
                 static_cast<unsigned long long>(vcpu.el2.pc), static_cast<unsigned long long>(esr));
}

void log_unsupported_exit(const Vcpu& vcpu, const Vm& owner, u64 esr, u64 ec) {
    const u64 ipa = fault_ipa(vcpu.el2, ec);
    u32 mapped_rights = 0;
    const PhysAddr mapped_pa = stage2_lookup_page(const_cast<Stage2AddressSpace*>(&owner.stage2), ipa, &mapped_rights);
    libk::printf("dango vm exit unsupported ec=%llx iss=%llx esr=%llx pc=%llx far=%llx hpfar=%llx ipa=%llx s2pa=%llx rights=%x\n",
                 static_cast<unsigned long long>(ec), static_cast<unsigned long long>(esr_iss(esr)),
                 static_cast<unsigned long long>(esr), static_cast<unsigned long long>(vcpu.el2.pc),
                 static_cast<unsigned long long>(vcpu.el2.exit_far), static_cast<unsigned long long>(vcpu.el2.exit_hpfar),
                 static_cast<unsigned long long>(ipa), static_cast<unsigned long long>(mapped_pa), mapped_rights);
}

void update_timer_lines(Vcpu& vcpu) {
    const u64 now = read_cntpct();
    const bool phys_asserted = timer_enabled_unmasked(vcpu.el2.guest_cntp_ctl) && now >= vcpu.el2.guest_cntp_cval;
    const bool virt_asserted = timer_enabled_unmasked(vcpu.el2.guest_cntv_ctl) && now >= vcpu.el2.guest_cntv_cval;
    vgic_set_level(vcpu, kGuestPhysTimerIrq, phys_asserted);
    vgic_set_level(vcpu, kGuestVirtTimerIrq, virt_asserted);
}

bool vcpu_has_pending_virtual_irq(Vcpu& vcpu) {
    update_timer_lines(vcpu);
    return vgic_peek_irq(vcpu.vgic) != kGicSpuriousIrq;
}

u64 next_guest_timer_deadline_cnt(const Vcpu& vcpu) {
    const u64 now = read_cntpct();
    u64 deadline = 0;
    if (timer_enabled_unmasked(vcpu.el2.guest_cntp_ctl)) {
        deadline = vcpu.el2.guest_cntp_cval;
    }
    if (timer_enabled_unmasked(vcpu.el2.guest_cntv_ctl) &&
        (deadline == 0 || vcpu.el2.guest_cntv_cval < deadline)) {
        deadline = vcpu.el2.guest_cntv_cval;
    }
    if (deadline && deadline < now) deadline = now;
    return deadline;
}

void record_guest_timer_source(Vcpu& vcpu) {
    const u64 deadline = next_guest_timer_deadline_cnt(vcpu);
    vcpu.sched.next_guest_timer_cnt = deadline;
    vcpu.sched.timer_armed = deadline != 0;
}

void wake_bound_runner(Vcpu& vcpu, WakeReason reason, u32 hints) {
    vcpu.sched.blocked_wfi = false;
    vcpu.sched.blocked_reason = VCPU_BLOCK_NONE;
    vcpu.sched.virtual_irq_pending = true;
    if (vcpu.state == VCPU_BLOCKED_OFF || vcpu.state == VCPU_OFF) {
        return; // PSCI CPU_OFF stays off until CPU_ON resets the target vCPU.
    }
    if (vcpu.state == VCPU_BLOCKED_WFI || vcpu.state == VCPU_BLOCKED) {
        vcpu.state = VCPU_READY;
    }

    auto runner = sched::find_thread(vcpu.sched.runner_tid);
    if (!runner || runner->state == TS_DEAD || runner->state == TS_RUNNING ||
        runner->state == TS_RUNNABLE_QUEUED || runner->state == TS_RUNNABLE_PENDING_REMOTE) {
        return;
    }

    sched::apply_wake_metadata(runner, reason, hints);
    const u32 target = sched::target_cpu_for_wake(runner, sched::current(), hints, vcpu.sched.home_cpu);
    if (target == sched::current_cpu_id()) sched::wake_local(runner);
    else sched::wake_remote(runner, target);
}

void block_runner_for_vcpu(Vcpu& vcpu, VcpuBlockReason reason) {
    Thread* runner = sched::current();
    if (runner) {
        vcpu.sched.runner_tid = runner->tid;
        sched::block(runner);
    }
    vcpu.sched.blocked_reason = static_cast<u32>(reason);
}

void finish_vcpu_run(Vcpu& vcpu, Vm& owner, VcpuState state, VcpuExitReason reason,
                     u64 last_esr, u64 used_ns, VcpuRunResult* out) {
    sync_vcpu_from_el2(vcpu);
    vcpu.state = state;
    owner.state = (state == VCPU_RUNNING) ? VM_RUNNING : (state == VCPU_DEAD ? VM_DEAD : VM_READY);
    vcpu.last_exit_reason = last_esr ? last_esr : static_cast<u64>(reason);
    vcpu.sched.last_exit_ns = sched::sched_clock_ns();
    vcpu.sched.used_ns += used_ns;
    if (out) {
        out->exit_reason = static_cast<u32>(reason);
        out->exit_batch_count = vcpu.sched.exit_batch_count;
        out->last_esr = vcpu.last_exit_reason;
        out->used_ns = used_ns;
    }
}

bool run_budget_expired(const VcpuRunBudget& budget, u64 start_ns, u32 exits) {
    const u64 now = sched::sched_clock_ns();
    if (budget.budget_ns && now - start_ns >= budget.budget_ns) return true;
    if (budget.deadline_ns && now >= budget.deadline_ns) return true;
    if (budget.max_exits && exits >= budget.max_exits) return true;
    return false;
}



void sync_vcpu_from_el2(Vcpu& vcpu) {
    for (u32 i = 0; i < 31; ++i) vcpu.regs[i] = vcpu.el2.regs[i];
    vcpu.sp = vcpu.el2.guest_sp_el1;
    vcpu.pc = vcpu.el2.pc;
    vcpu.pstate = vcpu.el2.pstate;
}

void init_el2_state(Vcpu& vcpu, const Vm& owner) {
    if (vcpu.el2.initialized) return;

    libk::memset(&vcpu.el2, 0, sizeof(vcpu.el2));

    vcpu.el2.regs[0] = vcpu.arg0;
    vcpu.el2.regs[1] = vcpu.arg1;
    vcpu.el2.regs[2] = 0;
    vcpu.el2.regs[3] = 0;

    vcpu.el2.pc = vcpu.entry_pc;
    vcpu.el2.pstate = 0x3C5;

    vcpu.el2.guest_sctlr = 0x30D0'0800ull;
    vcpu.el2.guest_sp_el1 = 0;

    vcpu.el2.vttbr = owner.stage2.root_table_phys |
                     ((static_cast<u64>(owner.vm_id & 0xFFu)) << 48);

    vcpu.el2.vtcr =
        25ull |          // T0SZ: 39-bit IPA
        (1ull << 6) |    // SL0: level-1 root for 4 KiB granule
        (1ull << 8) |    // IRGN0: WBWA
        (1ull << 10) |   // ORGN0: WBWA
        (3ull << 12) |   // SH0: inner-shareable
        (2ull << 16);    // PS: 40-bit PA

    vcpu.el2.guest_cntp_ctl  = 0;
    vcpu.el2.guest_cntp_cval = ~0ull;
    vcpu.el2.guest_cntv_ctl  = 0;
    vcpu.el2.guest_cntv_cval = ~0ull;

    vcpu.el2.guest_irq_pending = 0;
    vcpu.el2.timer_hw_mask = 0;
    vcpu.el2.exit_kind = 0;
    vcpu.el2.guest_vmpidr = vcpu.virtual_mpidr;
    vcpu.el2.guest_vpidr = vcpu.virtual_midr ? vcpu.virtual_midr : kDefaultGuestMidr;

    vcpu.el2.host_cntp_ctl   = 0;
    vcpu.el2.host_cntp_cval  = 0;

    vcpu.el2.initialized = 1;
}

} // namespace

#ifdef ANKO_BUILD_DEV
bool vcpu_ecam_probe(Vcpu& vcpu, Vm& owner) {
    init_el2_state(vcpu, owner);
    for (u32 exits = 0; exits < 64; ++exits) {
        const u64 esr = vm_el2_run(&vcpu.el2);
        if (vcpu.el2.exit_kind == 1) continue;
        if (vcpu.el2.exit_kind != 0) return false;
        if (arm64::esr_ec(esr) == arm64::kEcHvc64) return true;
        if (!emulate_mmio(vcpu, owner, data_abort_ipa(vcpu.el2), esr)) {
            ANKO_LOGF_ERROR("vm/selftest", "ECAM probe exit esr=0x%llx pc=0x%llx",
                           static_cast<unsigned long long>(esr),
                           static_cast<unsigned long long>(vcpu.el2.pc));
            return false;
        }
    }
    return false;
}
#endif

CapHandle create_vcpu(Process& caller, CapHandle vm_cap, u64 flags, SysError* out_error) {
    SysError rc = E_OK;
    auto v = vm_from_cap(caller, vm_cap, RIGHT_VM_CONTROL, &rc);
    if (!v) {
        if (out_error) *out_error = rc;
        return kNullCap;
    }
    if (v->vcpu_count >= kMaxVcpusPerVm) {
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    Vcpu* vcpu = static_cast<Vcpu*>(mm::slab_alloc(sizeof(Vcpu), alignof(Vcpu)));
    if (!vcpu) {
        if (out_error) *out_error = E_NOMEM;
        note_error();
        return kNullCap;
    }
    libk::memset(vcpu, 0, sizeof(*vcpu));
    vgic_init(vcpu->vgic);
    vcpu->vcpu_id = g_next_vcpu_id++;
    vcpu->owner_vm_id = v->vm_id;
    vcpu->state = VCPU_CREATED;
    vcpu->flags = flags;
    vcpu->virtual_mpidr = static_cast<u64>(v->vcpu_count);
    vcpu->virtual_midr = kDefaultGuestMidr;
    vcpu->sched.home_cpu = kInvalidCpu;
    vcpu->sched.core_mask = 0;
    vcpu->sched.blocked_reason = VCPU_BLOCK_NONE;

    vcpu->owner_lifetime = v->lifetime; object::retain(vcpu->owner_lifetime);
    vcpu->lifetime = object::create(vcpu, dispose_vcpu);
    if (!vcpu->lifetime) { dispose_vcpu(vcpu); if (out_error) *out_error = E_NOMEM; return kNullCap; }
    const CapHandle cap_h = cap::alloc(*caller.cap_table, CAP_VCPU, vcpu,
                                       RIGHT_VCPU_CONTROL |
                                           RIGHT_VCPU_INSPECT |
                                           RIGHT_DUP);
    if (cap_h == kNullCap) {
        object::release(vcpu->lifetime);
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    v->vcpus[v->vcpu_count++] = vcpu;
    metric_inc(VM_DEBUG_VCPU_CREATED);
    refresh_vm_state(v);
    if (out_error) *out_error = E_OK;
    return cap_h;
}

SysError set_vcpu_entry(Process& caller, CapHandle vcpu_cap, u64 entry_pc,
                        u64 arg0, u64 arg1) {
    SysError rc = E_OK;
    auto vcpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_CONTROL, &rc);
    if (!vcpu) return rc;
    if (entry_pc == 0) {
        note_error();
        return E_INVALID;
    }

    vcpu->entry_pc = entry_pc;
    vcpu->arg0 = arg0;
    vcpu->arg1 = arg1;
    vcpu->pc = entry_pc;
    for (u32 i = 0; i < 31; ++i) vcpu->regs[i] = 0;
    vcpu->regs[0] = arg0;
    vcpu->regs[1] = arg1;
    vcpu->sp = 0;
    vcpu->pstate = 0x3C5;
    libk::memset(&vcpu->el2, 0, sizeof(vcpu->el2));
    vcpu->state = VCPU_READY;
    vcpu->sched.blocked_wfi = false;
    vcpu->sched.blocked_reason = VCPU_BLOCK_NONE;
    vcpu->sched.virtual_irq_pending = false;
    metric_inc(VM_DEBUG_VCPU_CONFIGURED);

    Vm* owner = owner_for_vcpu(vcpu);
    refresh_vm_state(owner);
    return E_OK;
}

SysError inspect_vcpu(Process& caller, CapHandle vcpu_cap, VcpuInfo* out) {
    if (!out) return E_INVALID;
    SysError rc = E_OK;
    auto vcpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_INSPECT, &rc);
    if (!vcpu) return rc;

    (void)owner_for_vcpu(vcpu);

    VcpuInfo info{};
    info.vcpu_id = vcpu->vcpu_id;
    info.owner_vm_id = vcpu->owner_vm_id;
    info.state = static_cast<u32>(vcpu->state);
    info.entry_pc = vcpu->entry_pc;
    info.arg0 = vcpu->arg0;
    info.arg1 = vcpu->arg1;
    info.run_count = vcpu->run_count;
    info.last_exit_reason = vcpu->last_exit_reason;
    info.sched_context_id = vcpu->sched.sched_context_id;
    info.runner_tid = vcpu->sched.runner_tid;
    info.core_mask = vcpu->sched.core_mask;
    info.home_cpu = vcpu->sched.home_cpu;
    info.blocked_reason = vcpu->sched.blocked_reason;
    *out = info;
    return E_OK;
}

void prepare_guest_irq_state(Vcpu& vcpu) {
    update_timer_lines(vcpu);

    vcpu.el2.guest_irq_pending =
        (vgic_peek_irq(vcpu.vgic) != kGicSpuriousIrq) ? 1ull : 0ull;

    u64 timer_mask = 0;

    if (vgic_bit_test(vcpu.vgic.pending, kGuestPhysTimerIrq) ||
        vgic_bit_test(vcpu.vgic.active, kGuestPhysTimerIrq)) {
        timer_mask |= 1ull;
    }

    if (vgic_bit_test(vcpu.vgic.pending, kGuestVirtTimerIrq) ||
        vgic_bit_test(vcpu.vgic.active, kGuestVirtTimerIrq)) {
        timer_mask |= 2ull;
    }

    vcpu.el2.timer_hw_mask = timer_mask;
}

SysError vcpu_bind_sched_context(Process& caller, CapHandle vcpu_cap, CapHandle sched_context_cap) {
    SysError rc = E_OK;
    auto vcpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_CONTROL, &rc);
    if (!vcpu) return rc;

    auto ctx = sched::sched_context_from_cap(caller, sched_context_cap, &rc);
    if (!ctx) return rc;
    if (ctx->band == SCHED_BAND_RT_NATIVE) return E_PERM; // VM RT is soft RT only.

    const bool vm_band =
        ctx->band == SCHED_BAND_VM_LATENCY ||
        ctx->band == SCHED_BAND_VM_FAIR ||
        ctx->band == SCHED_BAND_BATCH;
    const bool latency_bootstrap_runner =
        ctx->band == SCHED_BAND_LATENCY &&
        ((ctx->flags & SCHED_CTX_VM_RUNNER) != 0);

    // Dango's userland runner thread may be temporarily bootstrapped in the
    // native latency band so it can get its first dispatch while the manager is
    // still in its latency-band service loop.  It is still explicitly marked as
    // a VM runner, and hard RT remains forbidden.
    if (!vm_band && !latency_bootstrap_runner) {
        return E_INVALID;
    }

    auto* next_context = object::acquire(ctx.get());
    object::release(vcpu->sched.context_lifetime); vcpu->sched.context_lifetime = next_context;
    vcpu->sched.sched_context_id = ctx->id;
    vcpu->sched.home_cpu = ctx->home_cpu;
    vcpu->sched.core_mask = ctx->core_mask;
    vcpu->sched.budget_ns = ctx->budget_ns;
    vcpu->sched.period_ns = ctx->period_ns;
    vcpu->sched.deadline_ns = ctx->deadline_ns;
    ctx->owner_thread_or_vcpu = vcpu->vcpu_id;
    ctx->flags |= SCHED_CTX_VM_RUNNER;
    if ((ctx->flags & SCHED_CTX_STEALABLE) == 0) ctx->flags |= SCHED_CTX_NO_STEAL;
    return E_OK;
}

SysError vcpu_set_affinity(Process& caller, CapHandle vcpu_cap, u64 core_mask, u32 home_cpu) {
    SysError rc = E_OK;
    auto vcpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_CONTROL, &rc);
    if (!vcpu) return rc;
    if (core_mask == 0) return E_INVALID;
    vcpu->sched.core_mask = core_mask;
    vcpu->sched.home_cpu = home_cpu;
    if (SchedContext* ctx = sched::sched_context_by_id(vcpu->sched.sched_context_id)) {
        ctx->core_mask = core_mask;
        ctx->home_cpu = home_cpu;
    }
    return E_OK;
}

SysError vcpu_kick(Process& caller, CapHandle vcpu_cap, u32 reason) {
    SysError rc = E_OK;
    auto vcpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_CONTROL, &rc);
    if (!vcpu) return rc;
    vcpu->sched.wake_reason = reason;
    wake_bound_runner(*vcpu, WAKE_IRQ, IPC_HINT_LATENCY);
    return E_OK;
}

SysError vcpu_inject_irq(Process& caller, CapHandle vcpu_cap, u32 irq) {
    SysError rc = E_OK;
    auto vcpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_CONTROL, &rc);
    if (!vcpu) return rc;
    if (!irq_in_range(irq)) return E_INVALID;
    vgic_bit_set(vcpu->vgic.pending, irq);
    vcpu->sched.virtual_irq_pending = true;
    wake_bound_runner(*vcpu, WAKE_IRQ, IPC_HINT_LATENCY);
    return E_OK;
}

SysError vcpu_block_until_irq_or_timer(Process& caller, CapHandle vcpu_cap) {
    SysError rc = E_OK;
    auto vcpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_CONTROL, &rc);
    if (!vcpu) return rc;
    if (vcpu_has_pending_virtual_irq(*vcpu)) return E_BUSY;
    record_guest_timer_source(*vcpu);
    vcpu->state = VCPU_BLOCKED_WFI;
    vcpu->sched.blocked_wfi = true;
    block_runner_for_vcpu(*vcpu, VCPU_BLOCK_GUEST_IDLE);
    return E_OK;
}

void poll_vcpu_timer(Vcpu& vcpu) {
    if (vcpu.state != VCPU_BLOCKED_WFI || !vcpu.sched.timer_armed ||
        vcpu.sched.next_guest_timer_cnt == 0) {
        return;
    }
    if (read_cntpct() < vcpu.sched.next_guest_timer_cnt) return;
    vcpu.sched.timer_armed = false;
    update_timer_lines(vcpu);
    if (vgic_peek_irq(vcpu.vgic) != kGicSpuriousIrq) {
        wake_bound_runner(vcpu, WAKE_IRQ, IPC_HINT_LATENCY);
    }
}

namespace {

constexpr u64 kVcpuRunInvalidState        = 0x44414e474f010000ull; // DANGO reason 1
constexpr u64 kVcpuRunInvalidNoOwner      = 0x44414e474f020000ull;
constexpr u64 kVcpuRunInvalidStage2Built  = 0x44414e474f030000ull;
constexpr u64 kVcpuRunInvalidStage2Empty  = 0x44414e474f040000ull;
constexpr u64 kVcpuRunInvalidStage2Active = 0x44414e474f050000ull;

SysError fail_vcpu_run_invalid(Vcpu& vcpu, VcpuRunResult* out, u64 detail) {
    note_error();
    vcpu.last_exit_reason = detail;
    if (out) {
        out->exit_reason = static_cast<u32>(VCPU_EXIT_UNSUPPORTED_TRAP);
        out->exit_batch_count = 0;
        out->last_esr = detail;
        out->used_ns = 0;
    }
    return E_INVALID;
}

} // namespace

SysError run_vcpu(Process& caller, CapHandle vcpu_cap, const VcpuRunBudget& budget, VcpuRunResult* out) {
    if (out) libk::memset(out, 0, sizeof(*out));

    SysError rc = E_OK;
    auto vcpu = vcpu_from_cap(caller, vcpu_cap, RIGHT_VCPU_CONTROL, &rc);
    if (!vcpu) return rc;

    if (vcpu->state != VCPU_READY && vcpu->state != VCPU_CONFIGURED) {
        libk::printf("dango vcpu run invalid: state=%u\n", static_cast<unsigned>(vcpu->state));
        return fail_vcpu_run_invalid(*vcpu, out, kVcpuRunInvalidState | static_cast<u64>(vcpu->state));
    }

    Thread* runner = sched::current();
    if (runner) {
        vcpu->sched.runner_tid = runner->tid;
        if (vcpu->sched.sched_context_id) {
            (void)sched::bind_current_to_sched_context_id(vcpu->sched.sched_context_id);
        }
    }

    metric_inc(VM_DEBUG_VCPU_RUN_ATTEMPTS);

    const u32 host_cpu = sched::current_cpu_id();
    if (!monitor_ready(host_cpu)) {
        libk::printf("dango vcpu run unavailable: cpu%u entered EL%llu without a monitor\n",
                     host_cpu, static_cast<unsigned long long>(entry_level(host_cpu)));
        metric_inc(VM_DEBUG_VCPU_RUN_NOTIMPL);
        vcpu->last_exit_reason = static_cast<u64>(E_NOTIMPL);
        if (out) {
            out->exit_reason = static_cast<u32>(VCPU_EXIT_UNSUPPORTED_TRAP);
            out->last_esr = static_cast<u64>(E_NOTIMPL);
        }
        return E_NOTIMPL;
    }

    Vm* owner = owner_for_vcpu(vcpu);
    if (!owner) {
        libk::printf("dango vcpu run invalid: owner vm missing id=%u vcpu=%u\n",
                     static_cast<unsigned>(vcpu->owner_vm_id),
                     static_cast<unsigned>(vcpu->vcpu_id));
        return fail_vcpu_run_invalid(*vcpu, out, kVcpuRunInvalidNoOwner | static_cast<u64>(vcpu->owner_vm_id));
    }
    if (vcpu->owner_vm_id == 0) {
        vcpu->owner_vm_id = owner->vm_id;
    }
    if (!owner->stage2.built) {
        libk::printf("dango vcpu run invalid: stage2 not built vm=%u\n",
                     static_cast<unsigned>(owner->vm_id));
        return fail_vcpu_run_invalid(*vcpu, out, kVcpuRunInvalidStage2Built | static_cast<u64>(owner->vm_id));
    }
    if (owner->stage2.mapped_pages == 0) {
        libk::printf("dango vcpu run invalid: stage2 empty vm=%u\n",
                     static_cast<unsigned>(owner->vm_id));
        return fail_vcpu_run_invalid(*vcpu, out, kVcpuRunInvalidStage2Empty | static_cast<u64>(owner->vm_id));
    }

    if (!owner->stage2.active) {
        rc = stage2_activate(&owner->stage2);
        if (rc != E_OK) {
            libk::printf("dango vcpu run invalid: stage2 activate rc=%d vm=%u built=%u root=%llx mapped=%llu\n",
                         static_cast<int>(rc),
                         static_cast<unsigned>(owner->vm_id),
                         owner->stage2.built ? 1u : 0u,
                         static_cast<unsigned long long>(owner->stage2.root_table_phys),
                         static_cast<unsigned long long>(owner->stage2.mapped_pages));
            return fail_vcpu_run_invalid(*vcpu, out, kVcpuRunInvalidStage2Active | static_cast<u64>(owner->vm_id));
        }
    }

    init_el2_state(*vcpu, *owner);

    vcpu->state = VCPU_RUNNING;
    owner->state = VM_RUNNING;
    vcpu->sched.last_entry_ns = sched::sched_clock_ns();
    vcpu->sched.exit_batch_count = 0;

    const u64 start_ns = vcpu->sched.last_entry_ns;
    const u32 max_exits = budget.max_exits ? budget.max_exits : 1;

    for (u32 exits = 0; exits < max_exits; ++exits) {
        prepare_guest_irq_state(*vcpu);

        const u64 esr = vm_el2_run(&vcpu->el2);
        ++vcpu->run_count;
        ++vcpu->sched.exit_batch_count;

        const u64 used_ns = sched::sched_clock_ns() - start_ns;

        if (vcpu->el2.exit_kind == 1) {
            metric_inc(VM_DEBUG_VCPU_IRQ_EXITS);
            irq_dispatch();
            if (sched::switch_pending() ||
                sched::principal_throttled(runner ? runner->principal_id : caller.principal_id) ||
                run_budget_expired(budget, start_ns, exits + 1)) {
                finish_vcpu_run(*vcpu, *owner, VCPU_READY, VCPU_EXIT_HOST_PREEMPTED, esr, used_ns, out);
                return E_OK;
            }
            continue;
        }

        if (vcpu->el2.exit_kind != 0) {
            finish_vcpu_run(*vcpu, *owner, VCPU_READY, VCPU_EXIT_HOST_PREEMPTED,
                            vcpu->el2.exit_kind, used_ns, out);
            metric_inc(VM_DEBUG_VCPU_IRQ_EXITS);
            return E_OK;
        }

        const u64 ec = esr_ec(esr);

        if (ec == kEcHvc64 && channel_hvc(*vcpu, *owner)) {
            if (run_budget_expired(budget, start_ns, exits + 1)) break;
            continue;
        }
        if (ec == kEcHvc64 || ec == kEcSmc64) {
            const PsciAction psci = emulate_psci(*vcpu, *owner, ec);
            if (psci == PsciAction::Continue) {
                if (run_budget_expired(budget, start_ns, exits + 1)) break;
                continue;
            }
            if (psci == PsciAction::StopCurrent) {
                finish_vcpu_run(*vcpu, *owner, VCPU_BLOCKED_OFF, VCPU_EXIT_WFI_BLOCKED, esr, used_ns, out);
                vcpu->sched.blocked_wfi = false;
                block_runner_for_vcpu(*vcpu, VCPU_BLOCK_GUEST_OFF);
                return E_OK;
            }
            if (psci == PsciAction::SuspendCurrent) {
                record_guest_timer_source(*vcpu);
                finish_vcpu_run(*vcpu, *owner, VCPU_BLOCKED_WFI, VCPU_EXIT_WFI_BLOCKED, esr, used_ns, out);
                vcpu->sched.blocked_wfi = true;
                block_runner_for_vcpu(*vcpu, VCPU_BLOCK_GUEST_IDLE);
                return E_OK;
            }
            if (psci == PsciAction::ShutdownVm) {
                finish_vcpu_run(*vcpu, *owner, VCPU_DEAD, VCPU_EXIT_SHUTDOWN, esr, used_ns, out);
                return E_OK;
            }
        }

        if (esr == 0) {
            finish_vcpu_run(*vcpu, *owner, VCPU_READY, VCPU_EXIT_HOST_PREEMPTED, esr, used_ns, out);
            metric_inc(VM_DEBUG_VCPU_IRQ_EXITS);
            return E_OK;
        }

        if (ec == kEcDataAbortLower || ec == kEcDataAbortCurrent) {
            const u64 ipa = data_abort_ipa(vcpu->el2);
            if (emulate_mmio(*vcpu, *owner, ipa, esr)) {
                if (run_budget_expired(budget, start_ns, exits + 1)) break;
                continue;
            }
        }

        if (ec == kEcWfx) {
            vcpu->el2.pc += 4;
            metric_inc(VM_DEBUG_VCPU_WFX_EXITS);

            if (wfx_is_wfe(esr)) {
                // Treat WFE as a guest yield.  Blocking here is too conservative:
                // Linux can use WFE in short wait loops where no timer is armed,
                // and turning it into a timer sleep makes early boot crawl.
                if (run_budget_expired(budget, start_ns, exits + 1)) break;
                continue;
            }

            const bool pending_irq = vcpu_has_pending_virtual_irq(*vcpu);
            if (pending_irq) {
                finish_vcpu_run(*vcpu, *owner, VCPU_READY, VCPU_EXIT_VIRTUAL_IRQ_PENDING, esr, used_ns, out);
                return E_OK;
            }
            record_guest_timer_source(*vcpu);
            finish_vcpu_run(*vcpu, *owner, VCPU_BLOCKED_WFI, VCPU_EXIT_WFI_BLOCKED, esr, used_ns, out);
            vcpu->sched.blocked_wfi = true;
            block_runner_for_vcpu(*vcpu, VCPU_BLOCK_GUEST_IDLE);
            return E_OK;
        }

        if (ec == kEcSysReg) {
            log_sysreg_trap(*vcpu, esr);
        }

        if (ec == kEcInstrAbortLower) {
            const u64 ipa = fault_ipa(vcpu->el2, ec);
            libk::printf("dango vm exit instr-abort-lower pc=%llx ipa=%llx esr=%llx\n",
                         static_cast<unsigned long long>(vcpu->el2.pc),
                         static_cast<unsigned long long>(ipa),
                         static_cast<unsigned long long>(esr));
        }

        const u64 reason = esr ? esr : kHcrExitUnsupported;
        if (vcpu->last_exit_reason != reason) {
            log_unsupported_exit(*vcpu, *owner, esr, ec);
        }
        finish_vcpu_run(*vcpu, *owner, VCPU_READY, VCPU_EXIT_UNSUPPORTED_TRAP, reason, used_ns, out);
        metric_inc(VM_DEBUG_VCPU_UNSUPPORTED_EXITS);
        metric_inc(VM_DEBUG_VCPU_RUN_NOTIMPL);
        return E_NOTIMPL;
    }

    const u64 used_ns = sched::sched_clock_ns() - start_ns;
    finish_vcpu_run(*vcpu, *owner, VCPU_READY, VCPU_EXIT_QUANTUM_EXPIRED,
                    vcpu->el2.exit_esr, used_ns, out);
    return E_OK;
}

void dispose_vcpu(void* target) {
    auto* cpu = static_cast<Vcpu*>(target);
    auto* owner = cpu->owner_lifetime;
    // Appliance-death propagation: a dying vCPU must not leave any channel
    // pointing at its storage (or a stale vGIC pending bit) behind.
    channel_detach_vcpu(*cpu);
    object::release(cpu->sched.context_lifetime);
    mm::slab_free(cpu, sizeof(Vcpu), alignof(Vcpu));
    object::release(owner);
}

void vgic_pending_set(Vcpu& vcpu, u32 irq) {
    vgic_bit_set(vcpu.vgic.pending, irq);
}

void vgic_pending_clear(Vcpu& vcpu, u32 irq) {
    vgic_bit_clear(vcpu.vgic.pending, irq);
}

bool vgic_pending_test(const Vcpu& vcpu, u32 irq) {
    return vgic_bit_test(vcpu.vgic.pending, irq);
}

void vcpu_wake_irq(Vcpu& vcpu) {
    wake_bound_runner(vcpu, WAKE_IRQ, IPC_HINT_LATENCY);
}

} // namespace vm
