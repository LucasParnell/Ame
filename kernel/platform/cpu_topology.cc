#include "anko/cpu_topology.h"
#include "anko/fdt.h"
#include "libk/string.h"
namespace smp {
namespace {
bool eq(const u8* value, u32 len, const char* s) {
    const usize n = libk::strlen(s);
    return len == n + 1 && !libk::memcmp(value, s, n + 1);
}
bool prefix(const char* s, const char* p) {
    while (*p) if (*s++ != *p++) return false;
    return true;
}
struct Visitor {
    CpuTopology topology{};
    u8 kind[16]{};
    u32 cells = 1;
    u64 cpu_reg = 0;
    bool cpu_has_reg = false;
    bool cpu_enabled = true;
    bool cpu_psci = false;
    bool conduit = false;
};
bool begin(void* p, const char* name, u32 depth) {
    auto& v = *static_cast<Visitor*>(p);
    if (depth >= 16) return false;
    const u8 parent = depth ? v.kind[depth - 1] : 0;
    v.kind[depth] = depth == 1 && (libk::strcmp(name, "psci") == 0 || prefix(name, "psci@")) ? 1 :
                    depth == 1 && libk::strcmp(name, "cpus") == 0 ? 2 :
                    parent == 2 && prefix(name, "cpu@") ? 3 : 0;
    if (v.kind[depth] == 3) {
        v.cpu_reg = 0; v.cpu_has_reg = false; v.cpu_enabled = true; v.cpu_psci = false;
    }
    return true;
}
void end(void* p, u32 depth) {
    auto& v = *static_cast<Visitor*>(p);
    if (depth >= 16 || v.kind[depth] != 3 || !v.cpu_has_reg || !v.cpu_enabled || !v.cpu_psci) return;
    for (u32 i = 0; i < v.topology.count; ++i) if (v.topology.mpidr[i] == v.cpu_reg) return;
    if (v.topology.count < MX_HOST_MAX_CPUS) v.topology.mpidr[v.topology.count++] = v.cpu_reg;
}
void prop(void* p, const char* name, const u8* data, u32 len, u32 depth) {
    auto& v = *static_cast<Visitor*>(p);
    if (depth >= 16) return;
    const auto kind = v.kind[depth];
    if (kind == 1 && libk::strcmp(name, "method") == 0) {
        v.conduit = eq(data, len, "hvc") || eq(data, len, "smc");
        v.topology.psci_hvc = eq(data, len, "hvc");
    } else if (kind == 1 && libk::strcmp(name, "cpu_on") == 0 && len == 4) {
        v.topology.cpu_on = libk::be32(data);
    } else if (kind == 2 && libk::strcmp(name, "#address-cells") == 0 && len == 4) {
        v.cells = libk::be32(data);
    } else if (kind == 3 && libk::strcmp(name, "reg") == 0 &&
               (v.cells == 1 || v.cells == 2) && len == 4 * v.cells) {
        v.cpu_reg = libk::be32(data);
        if (v.cells == 2) v.cpu_reg = (v.cpu_reg << 32) | libk::be32(data + 4);
        v.cpu_reg &= kMpidrAffinityMask; v.cpu_has_reg = true;
    } else if (kind == 3 && libk::strcmp(name, "status") == 0) {
        v.cpu_enabled = eq(data, len, "okay") || eq(data, len, "ok");
    } else if (kind == 3 && libk::strcmp(name, "enable-method") == 0) {
        v.cpu_psci = eq(data, len, "psci");
    }
}
}
CpuTopology read_cpu_topology(PhysAddr dtb, u64 boot_mpidr) {
    Visitor v{}; v.topology.cpu_on = 0xc4000003ull;
    const u64 boot = boot_mpidr & kMpidrAffinityMask;
    if (!dtb || !fdt::walk(reinterpret_cast<const u8*>(dtb), {&v, begin, end, prop}))
        return {1, {boot}, false, false, 0xc4000003ull};
    auto& t = v.topology;
    // Logical CPU zero is always the executing boot core, irrespective of DT order.
    u32 boot_at = t.count;
    for (u32 i = 0; i < t.count; ++i) if (t.mpidr[i] == boot) boot_at = i;
    if (boot_at == t.count) {
        if (t.count < MX_HOST_MAX_CPUS) ++t.count;
        boot_at = t.count - 1;
        t.mpidr[boot_at] = boot;
    }
    t.mpidr[boot_at] = t.mpidr[0]; t.mpidr[0] = boot;
    for (u32 i = 2; i < t.count; ++i) {
        const u64 key = t.mpidr[i]; u32 j = i;
        while (j > 1 && t.mpidr[j - 1] > key) { t.mpidr[j] = t.mpidr[j - 1]; --j; }
        t.mpidr[j] = key;
    }
    t.psci_available = v.conduit && t.cpu_on;
    return t;
}
}
