#pragma once
// ANE-LX1: fixed mechanisms verified against the captured firmware DT and the
// Hi6250 vendor power driver. See POWER.md for sources and physical test status.
#include "libk/mmio.h"
#include "libk/dma.h"

namespace anne::power {
constexpr unsigned long long kPowerholdBase = 0xfff0e000;
constexpr unsigned kPowerholdBit = 4;
constexpr unsigned kReasonOffset = 0x18b * 4; // PMIC HRST_REG0 via byte-wide PMUSSI
constexpr unsigned char kReasonOff = 0x00;
constexpr unsigned char kReasonNormal = 0x10;
constexpr unsigned char kReasonFastboot = 0x01;
constexpr unsigned char kReasonCharger = 0x06;

inline bool set_reason(const libk::Mmio& pmic, unsigned char reason) {
    // A PMIC register occupies one byte at each four-byte bus stride. A word
    // write here would touch reserved bus lanes rather than preserve a register.
    if (!pmic.write8(kReasonOffset, reason)) return false;
    libk::dma::order_mmio();
    unsigned char observed = 0;
    return pmic.read8(kReasonOffset, &observed) && observed == reason;
}

inline bool lower_powerhold(const libk::Mmio& gpio) {
    unsigned direction = 0;
    if (!gpio.read32(0x400, &direction)) return false;
    // PL061 masked data addressing changes only GPIO25 bit 4. The captured
    // phone DT supplies no powerhold-protect register, so none is guessed.
    if (!gpio.write32(0x400, direction | (1u << kPowerholdBit))) return false;
    libk::dma::order_mmio();
    if (!gpio.write32(1u << (kPowerholdBit + 2), 0)) return false;
    libk::dma::order_mmio();
    return true;
}

inline bool pulse_reset(const libk::Mmio& sctrl) {
    unsigned value = 0;
    if (!sctrl.read32(0x510, &value)) return false;
    if (!sctrl.write32(0x510, value | 4u)) return false;
    libk::dma::order_mmio();
    if (!sctrl.write32(0x510, value & ~4u)) return false;
    libk::dma::order_mmio();
    return true;
}
}
