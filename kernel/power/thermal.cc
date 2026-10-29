// Hi6250 uses the documented EL3 read-temperature service, not direct reads
// from secure sensor MMIO. Never invoke this from a scheduler/IRQ hook.
#include "anko/power.h"
#include "anko/fdt.h"
#include "anko/spinlock.h"
#include "libk/string.h"
#include "libk/str.h"
#include "anne/thermal.h"
namespace power {
namespace {
struct SensorConfig {
    u32 start, end, registers[3];
    bool supported;
} config;
struct Node {
    SensorConfig sensor;
    u32 seen;
    bool compatible, disabled;
};
struct Probe {
    Node nodes[24];
    SensorConfig found;
    u32 matches;
};
bool wanted(const char* name, u32 length) {
    static constexpr libk::str::Literal names[] = {
        "compatible",
        "status",
        "hisi,tsensor_adc_start_value",
        "hisi,tsensor_adc_end_value",
        "hisi,detect_cluster0_regno",
        "hisi,detect_cluster1_regno",
        "hisi,detect_gpu_regno",
    };
    return libk::str::one_of(name, length, names);
}
bool begin(void* ctx, const char*, u32 depth) {
    auto& p = *static_cast<Probe*>(ctx);
    if (depth >= 24)
        return false;
    p.nodes[depth] = {};
    return true;
}
void end(void* ctx, u32 depth) {
    auto& p = *static_cast<Probe*>(ctx);
    const auto& n = p.nodes[depth];
    if (n.compatible && !n.disabled && n.seen == 31) {
        p.found = n.sensor;
        p.found.supported = true;
        ++p.matches;
    }
}
void property(void* ctx, const char* name, const u8* data, u32 len, u32 depth) {
    auto& p = *static_cast<Probe*>(ctx);
    auto& n = p.nodes[depth];
    if (!libk::strcmp(name, "compatible") && len == sizeof("hisi,tsens") &&
        !libk::memcmp(data, "hisi,tsens", len))
        n.compatible = true;
    if (!libk::strcmp(name, "status"))
        n.disabled = !((len == 3 && !libk::memcmp(data, "ok", 3)) ||
                       (len == 5 && !libk::memcmp(data, "okay", 5)));
    if (len != 4)
        return;
    const u32 value = libk::be32(data);
    if (!libk::strcmp(name, "hisi,tsensor_adc_start_value")) {
        n.sensor.start = value;
        n.seen |= 1;
    }
    if (!libk::strcmp(name, "hisi,tsensor_adc_end_value")) {
        n.sensor.end = value;
        n.seen |= 2;
    }
    const char* names[] = {"hisi,detect_cluster0_regno", "hisi,detect_cluster1_regno",
                           "hisi,detect_gpu_regno"};
    for (u32 i = 0; i < 3; ++i)
        if (!libk::strcmp(name, names[i])) {
            n.sensor.registers[i] = value;
            n.seen |= 4u << i;
        }
}
kernel::SpinLock sample_lock;
u64 last_attempt;
bool attempted;
} // namespace
void thermal_init(PhysAddr dtb) {
    Probe p{};
    config = {};
    attempted = false;
    if (!dtb ||
        !fdt::walk(reinterpret_cast<const u8*>(dtb), {&p, begin, end, property, nullptr, wanted}))
        return;
    // This backend only implements the captured Hi6250 protocol and calibration.
    if (p.matches == 1 && p.found.supported && p.found.start == 116 && p.found.end == 922 &&
        p.found.registers[0] == 0 && p.found.registers[1] == 1 && p.found.registers[2] == 2)
        config = p.found;
}
void thermal_sample(u64 now) {
    kernel::LockGuard guard(sample_lock);
    if (!config.supported || (attempted && now >= last_attempt && now - last_attempt < 1000000000))
        return;
    attempted = true;
    last_attempt = now;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    extern u64 read_sensor(u32 index);
    for (u32 i = 0; i < 3; ++i) {
        const u64 adc = read_sensor(config.registers[i]);
        std::int32_t temperature = 0;
        const bool valid = anne::thermal::millicelsius(adc, config.start, config.end, temperature);
        (void)thermal_publish(i, temperature, valid, now);
    }
#endif
}
} // namespace power
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
extern "C" u64 psci_smc_call(u64, u64, u64, u64);
namespace power {
u64 read_sensor(u32 index) {
    return psci_smc_call(0xc5009900, index, 0, 0);
}
} // namespace power
#endif
