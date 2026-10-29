// Bounded DT discovery and cold frequency policy. No heap, virtual dispatch,
// shared RMWs on dispatch, or firmware calls from scheduler accounting.
#include "anko/power.h"
#include "anko/fdt.h"
#include "anko/spinlock.h"
#include "libk/string.h"
#include "libk/mmio.h"
#include "libk/integer.h"
#include "libk/str.h"
#include "anko/sched.h"
#include "anko/log.h"
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
#include "anne/dvfs.h"
#endif
namespace power {
namespace {
constexpr u32 kOpps = 16;
struct Domain {
    u64 cpus;
    u32 clock, count, khz[kOpps], firmware_id;
    bool anne_observer;
};
struct alignas(64) Policy {
    u64 sampled_ns, online, last_high_ns, idle[MX_POWER_CPUS];
    u32 min, max, thermal, sensor_cap, target, load, applied, transitions, failures;
    SysError status;
    bool load_valid, baseline_valid;
};
Domain domains[MX_POWER_DOMAINS]{};
Policy policies[MX_POWER_DOMAINS]{};
u32 domain_count;
FrequencySet backend;
kernel::SpinLock lock;
struct Node {
    u64 mpidr;
    u32 clock, count, khz[kOpps], id, offset, command;
    bool cpus, cpu, reg, enabled, xfreq, bad;
};
struct Visitor {
    Node nodes[32];
    Domain result[MX_POWER_DOMAINS];
    u32 count;
    const smp::CpuTopology* topology;
    bool clocks;
};
// Boot-only scratch avoids consuming a kernel stack with the bounded DT table.
Visitor visitor;
bool wanted(const char* name, u32 length) {
    static constexpr libk::str::Literal names[] = {
        "status",
        "phandle",
        "compatible",
        "reg",
        "clocks",
        "operating-points",
        "hisilicon,hi3xxx-xfreq-devid",
        "hisilicon,hi3xxx-xfreq-scbakdata",
        "hisilicon,set-rate-ipc-cmd",
    };
    return libk::str::one_of(name, length, names);
}
bool equal(const u8* data, u32 len, const char* value) {
    const auto n = libk::strlen(value) + 1;
    return len == n && !libk::memcmp(data, value, n);
}
bool begin(void* context, const char* name, u32 depth) {
    auto& v = *static_cast<Visitor*>(context);
    if (depth >= 32)
        return false;
    auto& n = v.nodes[depth];
    n = {};
    n.enabled = true;
    n.id = ~0u;
    n.cpus = depth == 1 && !libk::strcmp(name, "cpus");
    n.cpu = depth == 2 && v.nodes[1].cpus && !libk::strncmp(name, "cpu@", 4);
    return true;
}
void property(void* context, const char* name, const u8* data, u32 len, u32 depth) {
    auto& v = *static_cast<Visitor*>(context);
    auto& n = v.nodes[depth];
    if (!libk::strcmp(name, "status"))
        n.enabled = equal(data, len, "ok") || equal(data, len, "okay");
    if (!libk::strcmp(name, "phandle") && len == 4 && v.clocks)
        n.clock = libk::be32(data);
    if (!libk::strcmp(name, "compatible"))
        n.xfreq = equal(data, len, "hisilicon,hi3xxx-xfreq-clk");
    if (!libk::strcmp(name, "hisilicon,hi3xxx-xfreq-devid") && len == 4)
        n.id = libk::be32(data);
    if (!libk::strcmp(name, "hisilicon,hi3xxx-xfreq-scbakdata") && len == 4)
        n.offset = libk::be32(data);
    if (!libk::strcmp(name, "hisilicon,set-rate-ipc-cmd") && len == 8)
        n.command = libk::be32(data);
    if (!n.cpu || v.clocks)
        return;
    if (!libk::strcmp(name, "reg") && (len == 4 || len == 8)) {
        n.mpidr = libk::be32(data);
        if (len == 8)
            n.mpidr = (n.mpidr << 32) | libk::be32(data + 4);
        n.reg = true;
    }
    if (!libk::strcmp(name, "clocks") && (len == 4 || (len == 8 && !libk::be32(data + 4))))
        n.clock = libk::be32(data);
    if (!libk::strcmp(name, "operating-points")) {
        if (!len || len % 8 || len / 8 > kOpps) {
            n.bad = true;
            return;
        }
        n.count = len / 8;
        for (u32 i = 0; i < n.count; ++i) {
            n.khz[i] = libk::be32(data + i * 8);
            if (!n.khz[i] || !libk::be32(data + i * 8 + 4) || (i && n.khz[i] <= n.khz[i - 1]))
                n.bad = true;
        }
    }
}
void end(void* context, u32 depth) {
    auto& v = *static_cast<Visitor*>(context);
    const auto& n = v.nodes[depth];
    if (!n.enabled)
        return;
    if (v.clocks) {
        // Explicitly gate the board observer on the captured clock protocol.
        if (n.xfreq && n.id < 2 && n.offset == 0x41c && n.command == ((n.id + 1) << 16 | 0x30a)) {
            for (u32 d = 0; d < v.count; ++d)
                if (v.result[d].clock == n.clock) {
                    v.result[d].firmware_id = n.id;
                    v.result[d].anne_observer = true;
                }
        }
        return;
    }
    if (!n.cpu || !n.reg || !n.clock || !n.count || n.bad)
        return;
    u64 mask = 0;
    for (u32 i = 0; i < v.topology->count && i < MX_POWER_CPUS; ++i)
        if (n.mpidr == v.topology->mpidr[i])
            mask |= u64{1} << i;
    if (!mask)
        return;
    u32 d = 0;
    while (d < v.count && v.result[d].clock != n.clock)
        ++d;
    if (d == v.count) {
        if (v.count == MX_POWER_DOMAINS)
            return;
        ++v.count;
        v.result[d].clock = n.clock;
        v.result[d].count = n.count;
        for (u32 i = 0; i < n.count; ++i)
            v.result[d].khz[i] = n.khz[i];
    } else {
        if (v.result[d].count != n.count) {
            v.result[d].count = 0;
            return;
        }
        for (u32 i = 0; i < n.count; ++i)
            if (v.result[d].khz[i] != n.khz[i]) {
                v.result[d].count = 0;
                return;
            }
    }
    v.result[d].cpus |= mask;
}
u32 choose(u32 d, u32 requested) {
    const auto& domain = domains[d];
    const auto& p = policies[d];
    u32 ceiling = p.max;
    if (p.thermal && p.thermal < ceiling)
        ceiling = p.thermal;
    if (p.sensor_cap && p.sensor_cap < ceiling)
        ceiling = p.sensor_cap;
    if (requested < p.min)
        requested = p.min;
    if (requested > ceiling)
        requested = ceiling;
    u32 best = domain.khz[0];
    for (u32 i = 0; i < domain.count && domain.khz[i] <= ceiling; ++i) {
        best = domain.khz[i];
        if (best >= requested)
            break;
    }
    return best;
}
void sample_load(u32 d, u64 now, u64 online) {
    const auto& domain = domains[d];
    auto& p = policies[d];
    const u64 elapsed = now >= p.sampled_ns ? now - p.sampled_ns : 0;
    if (p.sampled_ns && p.load_valid && elapsed < 10000000)
        return;
    u32 busiest = 0;
    bool valid = p.sampled_ns && elapsed && p.baseline_valid && p.online == (domain.cpus & online);
    bool any = false, complete = true;
    for (u32 cpu = 0; cpu < MX_POWER_CPUS; ++cpu)
        if ((domain.cpus & online) & (u64{1} << cpu)) {
            MxPowerCpu sample{};
            any = true;
            if (!cpu_idle(cpu, now, sample)) {
                valid = false;
                complete = false;
                continue;
            }
            if (sample.idle_ns < p.idle[cpu])
                valid = false;
            const u64 idle = sample.idle_ns >= p.idle[cpu] ? sample.idle_ns - p.idle[cpu] : 0;
            p.idle[cpu] = sample.idle_ns;
            const u32 load =
                libk::ratio_per_mille(elapsed - (idle < elapsed ? idle : elapsed), elapsed);
            if (load > busiest)
                busiest = load;
        }
    p.online = domain.cpus & online;
    p.baseline_valid = complete;
    p.sampled_ns = now;
    p.load = busiest;
    p.load_valid = valid && any;
    if (p.load_valid) {
        const u32 target =
            choose(d, static_cast<u32>(static_cast<u64>(domain.khz[domain.count - 1]) *
                                       (busiest > 800 ? 1000 : busiest * 5 / 4) / 1000));
        // Up immediately; wait 80 ms before reducing clocks after a burst.
        if (target >= p.target) {
            p.target = target;
            p.last_high_ns = now;
        } else if (now >= p.last_high_ns && now - p.last_high_ns >= 80000000)
            p.target = target;
    }
}
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
SysError native_request(u32 d, u32 khz) {
    if (d >= domain_count || !domains[d].anne_observer)
        return E_NOTIMPL;
    for (u32 i = 0; i < domains[d].count; ++i)
        if (domains[d].khz[i] == khz)
            return anne::dvfs::request(domains[d].firmware_id, khz, i, sched::sched_clock_ns());
    return E_INVALID;
}
#endif
void apply(u32 d) {
    auto& p = policies[d];
    if (!backend || (p.applied == p.target && p.status != E_BUSY) || p.status == E_FAULT)
        return;
    const auto previous = p.status;
    p.status = backend(d, p.target);
    if (p.status == E_OK) {
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
        if (!p.applied)
            ANKO_LOGF_INFO("kernel/power", "dvfs domain=%u confirmed_khz=%u", d, p.target);
#endif
        if (p.applied != p.target)
            ++p.transitions;
        p.applied = p.target;
    } else if (p.status != E_BUSY && previous != p.status) {
        ++p.failures;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
        ANKO_LOGF_ERROR("kernel/power", "dvfs domain=%u status=%d target=%u", d, p.status,
                        p.target);
#endif
    }
}

} // namespace
void frequency_init(PhysAddr dtb, const smp::CpuTopology& topology) {
    domain_count = 0;
    backend = nullptr;
    visitor = {};
    visitor.topology = &topology;
    if (!dtb || !fdt::walk(reinterpret_cast<const u8*>(dtb),
                           {&visitor, begin, end, property, nullptr, wanted}))
        return;
    visitor.clocks = true;
    if (!fdt::walk(reinterpret_cast<const u8*>(dtb),
                   {&visitor, begin, end, property, nullptr, wanted}))
        return;
    for (u32 d = 0; d < visitor.count; ++d)
        if (visitor.result[d].count) {
            auto& domain = domains[domain_count];
            domain = visitor.result[d];
            auto& p = policies[domain_count++];
            p = {};
            p.min = domain.khz[0];
            p.max = domain.khz[domain.count - 1];
            p.target = p.max;
            p.sampled_ns = activity_started_ns();
            p.baseline_valid = p.sampled_ns != 0;
            p.online = domain.cpus & 1;
        }
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    if (anne::dvfs::init(dtb)) {
        bool supported = domain_count != 0;
        for (u32 d = 0; d < domain_count; ++d)
            supported &= domains[d].anne_observer;
        if (supported) {
            backend = native_request;
            ANKO_LOG_INFO("kernel/power", "LPM3 DVFS channel ready");
        }
    }
#endif
}
bool frequency_backend(FrequencySet set) {
    backend = set;
    return domain_count != 0;
}
SysError frequency_limits(u32 d, u32 min, u32 max) {
    kernel::LockGuard guard(lock);
    if (d >= domain_count || !min || min > max)
        return E_INVALID;
    bool allowed = false;
    for (u32 i = 0; i < domains[d].count; ++i)
        if (domains[d].khz[i] >= min && domains[d].khz[i] <= max)
            allowed = true;
    if (!allowed)
        return E_INVALID;
    policies[d].min = min;
    policies[d].max = max;
    policies[d].target = choose(d, policies[d].target);
    return E_OK;
}
SysError frequency_thermal_cap(u32 d, u32 max) {
    kernel::LockGuard guard(lock);
    if (d >= domain_count || (max && max < domains[d].khz[0]))
        return E_INVALID;
    policies[d].thermal = max;
    policies[d].target = choose(d, policies[d].target);
    if (!backend)
        return E_NOTIMPL;
    apply(d);
    return policies[d].status;
}
SysError frequency_request(u32 d, u32 khz) {
    kernel::LockGuard guard(lock);
    if (d >= domain_count || !khz)
        return E_INVALID;
    if (!backend)
        return E_NOTIMPL;
    const auto target = choose(d, khz);
    policies[d].target = target;
    apply(d);
    return policies[d].status;
}
void frequency_tick(u64 now, u64 online) {
    if (!backend)
        return;
    MxPowerThermal sensors[4]{};
    const bool valid_sensors = thermal_snapshot(now, sensors);
    kernel::LockGuard guard(lock);
    for (u32 d = 0; d < domain_count; ++d) {
        auto& p = policies[d];
        const auto& domain = domains[d];
        if (!(domain.cpus & online))
            continue;
        sample_load(d, now, online);
        if (domain.anne_observer) {
            const u32 sensor = domain.firmware_id;
            if (!valid_sensors || !sensors[sensor].valid || !sensors[2].valid)
                p.sensor_cap = domain.khz[0]; // failed/stale sensors cannot authorize boost
            else {
                const i32 temperature = sensors[sensor].millicelsius > sensors[2].millicelsius
                                            ? sensors[sensor].millicelsius
                                            : sensors[2].millicelsius;
                // DT passive region: 60 C threshold, 80 C target, 1 C hysteresis.
                if (temperature >= 60000) {
                    const u32 remaining = temperature < 80000 ? 80000 - temperature : 0;
                    p.sensor_cap = domain.khz[remaining * (domain.count - 1) / 20000];
                } else if (temperature < 59000)
                    p.sensor_cap = 0;
            }
        }
        p.target = choose(d, p.target);
        apply(d);
    }
}
void frequency_snapshot(u64 now, u64 online, MxPowerDomain* out, u32& count) {
    kernel::LockGuard guard(lock);
    count = domain_count;
    for (u32 d = 0; d < count; ++d) {
        const auto& domain = domains[d];
        auto& p = policies[d];
        auto& row = out[d];
        row = {};
        row.cpu_mask = domain.cpus;
        row.opp_count = domain.count;
        row.min_khz = p.min;
        row.max_khz = p.max;
        row.thermal_cap_khz =
            p.sensor_cap && (!p.thermal || p.sensor_cap < p.thermal) ? p.sensor_cap : p.thermal;
        if (!backend)
            sample_load(d, now, online);
        row.applied_khz = p.applied;
        row.transitions = p.transitions;
        row.failures = p.failures;
        row.control_status = p.status;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
        if (domain.anne_observer) {
            u32 failures = 0;
            anne::dvfs::statistics(domain.firmware_id, row.transitions, failures);
            if (failures > row.failures)
                row.failures = failures;
        }
#endif
        if (p.sensor_cap)
            row.flags |= MX_FREQ_THERMAL;
        row.target_khz = p.target;
        row.busy_permille = p.load;
        if (p.load_valid)
            row.flags |= MX_FREQ_LOAD;
        if (backend)
            row.flags |= MX_FREQ_CONTROL;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
        u32 observed = 0;
        const libk::Mmio sctrl{reinterpret_cast<volatile u8*>(0xfff0a000), 4096};
        if (domain.anne_observer && sctrl.read32(0x41c, &observed)) {
            const u32 index = (observed >> (domain.firmware_id * 4)) & 15;
            if (index < domain.count) {
                row.observed_khz = domain.khz[index];
                row.flags |= MX_FREQ_OBSERVED;
            }
        }
#endif
    }
}
} // namespace power
