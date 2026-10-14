#include "../boards/huawei-anne/power.h"
#include <cassert>
#include <cstdio>

int main() {
    alignas(8) unsigned char pmic[4096]{}, gpio[4096]{}, sctrl[4096]{};
    const libk::Mmio pmic_regs{pmic, sizeof(pmic)}, gpio_regs{gpio, sizeof(gpio)},
                      sctrl_regs{sctrl, sizeof(sctrl)};
    for (auto& byte : pmic) byte = 0xa5;
    assert(anne::power::set_reason(pmic_regs, anne::power::kReasonFastboot));
    for (unsigned i = 0; i < sizeof(pmic); ++i)
        assert(pmic[i] == (i == 0x62c ? 1 : 0xa5));
    assert(anne::power::set_reason(pmic_regs, anne::power::kReasonNormal));
    assert(pmic[0x62c] == 0x10);
    assert(anne::power::set_reason(pmic_regs, anne::power::kReasonOff));
    assert(pmic[0x62c] == 0);
    assert(gpio_regs.write32(0x400, 0x85));
    assert(gpio_regs.write32(0x40, ~0u));
    assert(anne::power::lower_powerhold(gpio_regs));
    unsigned observed = 0;
    assert(gpio_regs.read32(0x400, &observed) && observed == 0x95);
    assert(gpio_regs.read32(0x40, &observed) && observed == 0);
    assert(sctrl_regs.write32(0x510, 0xab30));
    assert(anne::power::pulse_reset(sctrl_regs));
    assert(sctrl_regs.read32(0x510, &observed) && observed == 0xab30);
    assert(!anne::power::set_reason({pmic, 0x62c}, 1));
    assert(!anne::power::lower_powerhold({gpio, 0x400}));
    assert(!anne::power::pulse_reset({sctrl, 0x510}));
    assert(!pmic_regs.read8(4096, nullptr));
    assert(!pmic_regs.write8(4096, 0));
    std::puts("Anne power registers: byte lanes, GPIO mask and NMI preserved");
}
