// userland/dango/auth_probe/main.cc — negative VM-root authority test.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "dango/vm.h"

constexpr uint32_t kRingOrder = 6;
constexpr uint64_t kVaConsoleRing = 0x5000'0000ull;

constexpr CapHandle cap_slot(uint32_t idx) {
    return (static_cast<CapHandle>(1) << 32) | idx;
}

constexpr CapHandle kConsoleRing = cap_slot(0);
constexpr CapHandle kConsoleDoorbell = cap_slot(1);
constexpr CapHandle kRestrictedVmRoot = cap_slot(2);
constexpr CapHandle kAuthGateDoorbell = cap_slot(3);

template <typename T>
static T* wait_map(CapHandle cap, int64_t expected_type, uint64_t va, uint32_t rights) {
    while (true) {
        if (sys_cap_identify(cap) == expected_type) {
            if (sys_map_page(cap, va, rights) == 0) return reinterpret_cast<T*>(va);
        }
        sys_yield();
    }
}

static void wait_cap(CapHandle cap, int64_t expected_type) {
    while (sys_cap_identify(cap) != expected_type) {
        sys_yield();
    }
}

static void console_log(const char* msg) {
    mochi::log_raw_marker(msg);
}

[[noreturn]] static void fail(const char* msg) {
    console_log(msg);
    while (true) sys_yield();
}

extern "C" [[noreturn]] void _start() {
    auto* log_ring = wait_map<RingPage>(kConsoleRing, dango::CAP_SHARED_RING,
                                        kVaConsoleRing,
                                        dango::RIGHT_READ | dango::RIGHT_WRITE);
    wait_cap(kConsoleDoorbell, dango::CAP_DOORBELL);
    mochi::log_set_sink(log_ring, kRingOrder, kConsoleDoorbell, "dango.auth_probe");
    wait_cap(kRestrictedVmRoot, dango::CAP_VM_ROOT);
    wait_cap(kAuthGateDoorbell, dango::CAP_DOORBELL);

    const int64_t rc = sys_vm_create(kRestrictedVmRoot, 0);
    if (rc != dango::E_PERM) fail("dango: authority check failed\n");

    console_log("dango: authority check passed\n");
    (void)sys_notify(kAuthGateDoorbell);
    while (true) sys_yield();
}
