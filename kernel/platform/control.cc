// Cold board mechanisms. A shared clock register is never mapped into an
// individual driver. Hardware masks are fixed here; userland owns sequence.
#include "anko/platform_control.h"
#include "anko/log.h"
#include "anko/spinlock.h"
#include "libk/mmio.h"
#include "libk/dma.h"
#include "midix/abi/platform.h"
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
#include "../../boards/huawei-anne/display_diagnostics.h"
#endif
namespace platform {
namespace {
Control domains[] = {{Domain::UsbPhy, 0, 0},
                     {Domain::UsbController, 0, 0},
                     {Domain::I2c, 0, 0},
                     {Domain::Display, 0, 0}};
kernel::SpinLock domain_lock;
} // namespace
Control* board_control(Domain domain) { return &domains[static_cast<u32>(domain)]; }
SysError control(Control& object, u32 op, u64 arg, u64 owner) {
    kernel::LockGuard guard(domain_lock);
    if (!owner) {
        return E_INVALID;
    }
    if (op == MX_PLATFORM_CLAIM) {
        if (arg || (object.owner && object.owner != owner)) {
            return E_BUSY;
        }
        object.owner = owner;
        return E_OK;
    }
    if (object.owner != owner) {
        return E_PERM;
    }
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    libk::Mmio crg{reinterpret_cast<volatile u8*>(0xfff35000), 4096};
    if (object.domain == Domain::UsbPhy) {
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
            // nibble 1; AP master 1 requests value 3. One attempt per syscall:
            // contention returns to the PHY service's bounded deadline loop.
            constexpr u32 command = 3u << 4;
            pctrl.write32(0x40c, command);
            libk::dma::order_mmio();
            u32 lock_status = 0;
            pctrl.read32(0x414, &lock_status);
            if (((lock_status >> 4) & 15u) != 3u) {
                return E_BUSY;
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
                          "i2c4 pins pmx=%08x/%08x cfg=%08x/%08x stat2=%08x rst2=%08x", scl, sda,
                          cfg0, cfg1, stat2, rst2);
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
        if (object.owner == owner) {
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
            // Assert controller reset before cap/arena reclamation. PIO currently
            // forbids DMA, but the lifetime fence remains mandatory for future DMA.
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
