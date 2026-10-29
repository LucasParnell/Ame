// Cold board mechanisms. A shared clock register is never mapped into an
// individual driver. Hardware masks are fixed here; userland owns sequence.
#include "anko/platform_control.h"
#include "anko/power.h"
#include "anko/log.h"
#include "anko/spinlock.h"
#include "libk/mmio.h"
#include "libk/dma.h"
#include "midix/abi/platform.h"
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
#include "../../boards/huawei-anne/display_diagnostics.h"
#include "../../boards/huawei-anne/emmc.h"
#include "../power/anne/pll.h"
#endif
namespace platform {
namespace {
Control domains[] = {{Domain::UsbPhy, 0, 0},  {Domain::UsbController, 0, 0}, {Domain::I2c, 0, 0},
                     {Domain::Display, 0, 0}, {Domain::System, 0, 0},        {Domain::Emmc, 0, 0},
                     {Domain::Charger, 0, 0}};
kernel::SpinLock domain_lock;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
u32 ref_logged;
#endif
} // namespace
Control* board_control(Domain domain) {
    const u32 index = static_cast<u32>(domain);
    return index < sizeof(domains) / sizeof(domains[0]) ? &domains[index] : nullptr;
}
SysError control(Control& object, u32 op, u64 arg, u64 owner) {
    if (object.domain == Domain::System && op != MX_PLATFORM_CLAIM) {
        {
            kernel::LockGuard guard(domain_lock);
            if (!owner)
                return E_INVALID;
            if (object.owner != owner)
                return E_PERM;
        }
        if (op == MX_PLATFORM_CPU_FREQUENCY)
            return power::frequency_request(static_cast<u32>(arg >> 32), static_cast<u32>(arg));
        if (op == MX_PLATFORM_SYSTEM_CHECK) {
            if (arg > 0xffffffffu)
                return E_INVALID;
            return power::system_power_check(static_cast<u32>(arg));
        }
        if (arg)
            return E_INVALID;
        const auto supported = power::system_power_check(op);
        if (supported != E_OK)
            return supported;
        // No global platform lock across firmware/hardware transitions. A
        // refused operation must return with IRQ state and services recoverable.
        ANKO_LOGF_INFO("kernel/platform", "system power requested op=%u", op);
        if (op == MX_PLATFORM_SYSTEM_OFF)
            return power::system_off();
        return power::system_reset(op == MX_PLATFORM_SYSTEM_FASTBOOT);
    }
    kernel::LockGuard guard(domain_lock);
    if (!owner) {
        return E_INVALID;
    }
    if (op == MX_PLATFORM_CLAIM) {
        if (arg || (object.owner && object.owner != owner)) {
            return E_BUSY;
        }
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
        if (object.domain == Domain::Emmc && !anne::emmc::claim_user(owner))
            return E_BUSY;
#endif
        object.owner = owner;
        return E_OK;
    }
    if (object.owner != owner) {
        return E_PERM;
    }
    if (object.domain == Domain::Charger) {
        if (op == MX_PLATFORM_CHARGER_SAMPLE)
            return power::charger_publish(arg);
        if (op == MX_PLATFORM_CHARGER_INVALIDATE && !arg) {
            power::charger_invalidate();
            return E_OK;
        }
        return E_INVALID;
    }
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    libk::Mmio crg{reinterpret_cast<volatile u8*>(0xfff35000), 4096};
    if (object.domain == Domain::Emmc && op == MX_PLATFORM_EMMC_CLOCK && !arg) {
        // Captured Anne DT: muxsys selects clkin_sys/6 or muxpll/div.
        // Read the selected PLL's actual dividers rather than its nominal DT rate.
        u32 selection = 0, gate = 0, ahb_gate = 0, reset = 0, divider_gate = 0;
        crg.read32(0xb4, &selection);
        crg.read32(0x4c, &gate);
        crg.read32(0x0c, &ahb_gate);
        crg.read32(0x98, &reset);
        crg.read32(0xf4, &divider_gate);

        u64 parent = 0;
        if (selection & 4u) {
            const bool pll3 = selection & 1u;
            libk::Mmio pll{reinterpret_cast<volatile u8*>(pll3 ? 0xfff31000 : 0xfff0a000), 4096};
            u32 control0 = 0, control1 = 0, locked = 0;
            pll.read32(pll3 ? 0x48 : 0x100, &control0);
            pll.read32(pll3 ? 0x4c : 0x104, &control1);
            if (pll3)
                locked = (control0 >> 26) & 1u;
            else
                pll.read32(0x10c, &locked);

            parent = anne::clocks::pll_rate(control0, control1);
            anko::log::writef(anko::log::Level::Info, "kernel/platform",
                              "emmc pll%u control=%x/%x locked=%u parent_hz=%llu", pll3 ? 3u : 0u,
                              control0, control1, locked & 1u,
                              static_cast<unsigned long long>(parent));
            if (!(locked & 1u) || !parent)
                return E_DEAD;
        }

        const u64 input = selection & 4u ? parent / (((selection >> 3) & 15u) + 1) : 19200000u / 6;
        if (!input || input > 400000000u)
            return E_NOTIMPL;
        anko::log::writef(anko::log::Level::Info, "kernel/platform",
                          "emmc mux=%x gate=%x ahb=%x reset=%x divgate=%x input_hz=%u", selection,
                          gate, ahb_gate, reset, divider_gate, static_cast<u32>(input));

        if (!(gate & (1u << 15)) || !(ahb_gate & (1u << 13)) || (reset & (1u << 16)) ||
            ((selection & 4u) && !(divider_gate & 2u)))
            return E_DEAD;

        return static_cast<SysError>(input);
    } else if (object.domain == Domain::UsbPhy) {
        if (op == MX_PLATFORM_USB_CLOCK_ON && !arg) {
            crg.write32(0x40, 6);
        } else if (op == MX_PLATFORM_USB_CLOCK_OFF && !arg) {
            crg.write32(0x44, 6);
        } else if ((op == MX_PLATFORM_RESET_ASSERT || op == MX_PLATFORM_RESET_RELEASE) && arg &&
                   !(arg & ~u64{MX_ANNE_USB_RESETS})) {
            crg.write32(op == MX_PLATFORM_RESET_ASSERT ? 0x90 : 0x94, static_cast<u32>(arg));
        } else if (op == MX_PLATFORM_USB_REFERENCE && !arg) {
            libk::Mmio pmic{reinterpret_cast<volatile u8*>(0xfff34000), 4096};
            libk::Mmio pctrl{reinterpret_cast<volatile u8*>(0xe8a09000), 4096};
            // PMIC ABB is shared with firmware. Hi6250 lock 9 is group 1,
            // nibble 1; AP master 1 requests value 3.
            // Best-effort during bring-up: the raw lock status is logged and a
            // busy lock must not wedge the PHY service; the transition proceeds
            // with the firmware-provided ABB state.
            constexpr u32 command = 3u << 4;
            u32 before = 0, after = 0;
            pctrl.read32(0x414, &before);
            pctrl.write32(0x40c, command);
            libk::dma::order_mmio();
            pctrl.read32(0x414, &after);
            const bool acquired = ((after >> 4) & 15u) == 3u;
            if (ref_logged < 3) {
                ++ref_logged;
                anko::log::writef(anko::log::Level::Info, "kernel/platform",
                                  "usb ref status=%x/%x acquired=%u", before, after,
                                  acquired ? 1u : 0u);
            }
            if (!acquired) {
                return E_OK;
            }
            u32 gate = 0;
            pmic.read32(0x43c, &gate);
            pmic.write32(0x43c, gate | 1u);
            libk::dma::order_mmio();
            pctrl.write32(0x410, command);
            libk::dma::order_mmio();
            u32 selection = 0;
            pctrl.read32(0x64, &selection);
            pctrl.write32(0x64, (selection & ~(7u << 24)) | (5u << 24));
        } else {
            return E_INVALID;
        }
    } else if (object.domain == Domain::I2c && op == MX_PLATFORM_I2C_CLOCK_ON && !arg) {
        crg.write32(0x20, 1u << 27);
        crg.write32(0x7c, 1u << 27);
    } else if (object.domain == Domain::I2c && op == MX_PLATFORM_I2C_PINS && !arg) {
        // i2c4 (fdf0d000) SCL/SDA are GPIO_037/038. Values mirror the deployed
        // vendor DT: pmx function = 1, cfg = 0. Read-modify-write the 3-bit
        // function field; readbacks (pads + clock/reset status) feed the log.
        libk::Mmio pads{reinterpret_cast<volatile u8*>(0xe896c000), 4096};
        u32 v = 0;
        (void)pads.read32(0x88, &v);
        pads.write32(0x88, (v & ~7u) | 1u);
        (void)pads.read32(0x8c, &v);
        pads.write32(0x8c, (v & ~7u) | 1u);
        pads.write32(0x894, 0);
        pads.write32(0x898, 0);
        libk::dma::order_mmio();
        u32 scl = 0, sda = 0, cfg0 = 0, cfg1 = 0, stat2 = 0, rst2 = 0;
        (void)pads.read32(0x88, &scl);
        (void)pads.read32(0x8c, &sda);
        (void)pads.read32(0x894, &cfg0);
        (void)pads.read32(0x898, &cfg1);
        crg.read32(0x2c, &stat2);
        crg.read32(0x80, &rst2);
        anko::log::writef(anko::log::Level::Info, "kernel/platform",
                          "i2c4 pins pmx=%x/%x cfg=%x/%x stat2=%x rst2=%x", scl, sda, cfg0, cfg1,
                          stat2, rst2);
    } else if (object.domain == Domain::Display && op == MX_PLATFORM_DISPLAY_HANDOFF && !arg) {
#ifdef ANKO_ANNE_FRAMEBUFFER
        anne::native_display::handoff_console();
#else
        return E_NOTIMPL;
#endif
    } else {
        return E_INVALID;
    }
    libk::dma::order_mmio();
    return E_OK;
#else
    (void)arg;
    return E_NOTIMPL;
#endif
}
void fence_owner(u64 owner) {
    kernel::LockGuard guard(domain_lock);
    for (auto& object : domains) {
        if (owner && object.owner == owner) {
            if (object.domain == Domain::Charger)
                power::charger_invalidate();
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
            if (object.domain == Domain::Emmc)
                anne::emmc::release_user(owner);
            // eMMC release_user fences ADMA and retains board ownership if reset
            // fails. USB likewise requires reset before cap/arena reclamation.
            if (object.domain == Domain::UsbController || object.domain == Domain::UsbPhy) {
                libk::Mmio crg{reinterpret_cast<volatile u8*>(0xfff35000), 4096};
                crg.write32(0x90, 1u << 9);
                libk::dma::order_mmio();
            }
#endif
            object.owner = 0;
        }
    }
}
} // namespace platform
