// userland/dango/auth_probe/main.cc — negative VM-root authority test.

#include "mochi/syscall.h"
#include "mochi/handle.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "dango/vm.h"
#include "mochi/va_layout.h"

constexpr uint32_t kRingOrder = MX_DEFAULT_RING_ORDER;
using namespace mochi::va::dango_auth_probe;


static void console_log(const char* msg) {
    mochi::log_raw_marker(msg);
}

[[noreturn]] static void fail(const char* msg) {
    console_log(msg);
    while (true) sys_yield();
}

extern "C" [[noreturn]] void _start() {
    const MxBootInfo* boot = mochi::boot::map();
    const CapHandle kConsoleRing = mochi::boot::require_handle(boot, "log.ring");
    const CapHandle kConsoleDoorbell = mochi::boot::require_handle(boot, "log.doorbell");
    const CapHandle kRestrictedVmRoot = mochi::boot::require_handle(boot, "vm.root.restricted");
    const CapHandle kAuthGateDoorbell = mochi::boot::require_handle(boot, "dango.auth_gate.db");

    auto* log_ring = mochi::cap_map_when<RingPage>(kConsoleRing, dango::CAP_SHARED_RING,
                                        kVaConsoleRing,
                                        dango::RIGHT_READ | dango::RIGHT_WRITE);
    mochi::cap_wait_type(kConsoleDoorbell, dango::CAP_DOORBELL);
    mochi::log_set_sink(log_ring, kRingOrder, kConsoleDoorbell, "dango.auth_probe");
    mochi::cap_wait_type(kRestrictedVmRoot, dango::CAP_VM_ROOT);
    mochi::cap_wait_type(kAuthGateDoorbell, dango::CAP_DOORBELL);

    const int64_t rc = sys_vm_create(kRestrictedVmRoot, 0);
    if (rc != dango::E_PERM) fail("authority check failed\n");

    console_log("authority check passed\n");
    (void)sys_notify(kAuthGateDoorbell);
    mochi::boot::ready(boot);
    while (true) sys_yield();
}
