#pragma once
// mochi/va_layout.h — the userland service VA plan.
//
// Every service is its own address space and maps its capability pages at fixed
// VAs it chooses. Those choices belong in one place, not scattered as literals
// per file, so the whole plan is visible at once and the windows can be checked
// against each other. `mochi/va_layout.h` is that place.
//
// Console's windows are *precomputed* with Planner (bump + align), so its
// disjointness is structural. The other services' windows are listed as explicit
// values because their spacing is deliberate (an order-10 ring needs a 0x11000
// window, so windows sit 0x20000 apart) — recomputing them would move addresses
// for no benefit; listing them here still keeps one source of truth.
//
// These are mapping *targets* in a private address space, not device addresses.

#include "libk/integer.h"
#include "midix/abi/dango_guest_layout.h"   // DANGO_INITRD_VA_SPAN (init's layout)
#include "midix/abi/ring.h"

namespace mochi::va {

constexpr uint64_t kPage = 0x1000;

// Windows every service maps the same way.
constexpr uint64_t kBootInfo = 0x1000'0000ull; // BootInfo arena (cap slot 0)
constexpr uint64_t kLogRing  = 0x1800'0000ull; // this service's log ring

// Compile-time window planner: reserve(size) returns the next page-aligned
// window and advances past it, so reserved windows never overlap.
class Planner {
public:
    constexpr explicit Planner(uint64_t base) : next_(base) {}
    constexpr uint64_t reserve(uint64_t size, uint64_t align = kPage) {
        const uint64_t at = libk::align_up(next_, align);
        next_ = at + size;
        return at;
    }

private:
    uint64_t next_;
};

// --- io.console ------------------------------------------------------------
namespace console {

constexpr uint32_t kLogRingOrder  = 6;
constexpr uint64_t kLogRingStride = 0x1'0000ull;
constexpr uint64_t kLogRingCount  = 32; // == ConsoleBootConfig::kMaxRings
constexpr uint64_t kLogRingSpan   = kLogRingStride * kLogRingCount;
static_assert(kLogRingStride >= mx_ring_page_bytes(kLogRingOrder),
              "log ring VA stride must hold one ring page");

constexpr uint64_t kConfigBase = 0x3000'0000ull;

struct Windows {
    uint64_t uart;
    uint64_t log_ring_base;
    uint64_t mirror_ring;
    constexpr uint64_t log_ring(uint32_t i) const { return log_ring_base + i * kLogRingStride; }
};

constexpr Windows plan() {
    Planner p{0x2000'0000ull};
    Windows w{};
    w.uart          = p.reserve(kPage);
    w.log_ring_base = p.reserve(kLogRingSpan);
    w.mirror_ring   = p.reserve(kPage);
    return w;
}
constexpr Windows kWindows = plan();

} // namespace console

// --- nameservice -----------------------------------------------------------
namespace nameservice {
constexpr uint64_t kVaLogRing = 0x2002'0000ull;
}

// --- init ------------------------------------------------------------------
// init's layout grows after the mapped initrd, so a large rootfs cannot run
// through the fixed mappings above it.
namespace init {
constexpr uint64_t kVaManifest       = 0x2000'0000ull;
constexpr uint64_t kVaInitrd         = 0x2400'0000ull;
constexpr uint64_t kVaInitLogRing    = kVaInitrd + DANGO_INITRD_VA_SPAN;
constexpr uint64_t kVaConsoleConfig  = kVaInitLogRing + 0x2'0000ull;
constexpr uint64_t kVaSvcCtlReqRing  = kVaInitLogRing + 0x7'0000ull;
constexpr uint64_t kVaSvcCtlReplyRing = kVaInitLogRing + 0x8'0000ull;
constexpr uint64_t kVaBootInfoBase   = kVaInitLogRing + 0x10'0000'0ull;
constexpr uint64_t kVaSvcRegistry    = kVaInitLogRing + 0x20'0000'0ull;
constexpr uint64_t kVaSessions       = kVaSvcRegistry + 0x1'0000ull;
}

// --- tty.fb0 ---------------------------------------------------------------
namespace tty {
constexpr uint64_t kVaBootInfo        = kBootInfo;
constexpr uint64_t kVaLogRing         = kLogRing;
constexpr uint64_t kVaFbCmdRing       = 0x1900'0000ull;
constexpr uint64_t kVaFbReplyRing     = 0x1902'0000ull; // clear of the order-10 cmd ring
constexpr uint64_t kVaMirrorRing      = 0x1904'0000ull;
constexpr uint64_t kVaInputRing       = 0x1906'0000ull;
constexpr uint64_t kVaShellInputRing  = 0x1908'0000ull; // tty -> msh
constexpr uint64_t kVaShellOutputRing = 0x190A'0000ull; // msh -> tty
}

// --- msh -------------------------------------------------------------------
namespace msh {
constexpr uint64_t kVaBootInfo    = kBootInfo;
constexpr uint64_t kVaLogRing     = kLogRing;
constexpr uint64_t kVaTermInput   = 0x2000'0000ull;
constexpr uint64_t kVaTermOutput  = 0x2002'0000ull;
constexpr uint64_t kVaFsCmdRing   = 0x2004'0000ull;
constexpr uint64_t kVaFsReplyRing = 0x2006'0000ull;
constexpr uint64_t kVaSvcRegistry = 0x2010'0000ull;
}

// --- io.fs -----------------------------------------------------------------
namespace fs {
constexpr uint64_t kVaBootInfo     = kBootInfo;
constexpr uint64_t kVaLogRing      = kLogRing;
constexpr uint64_t kVaBlkReqRing   = 0x2002'0000ull;
constexpr uint64_t kVaBlkReplyRing = 0x2003'0000ull;
constexpr uint64_t kVaBlkDma       = 0x2100'0000ull;
}

// --- io.framebuffer --------------------------------------------------------
namespace framebuffer {
constexpr uint64_t kVaBootInfo     = kBootInfo;
constexpr uint64_t kVaLogRing      = kLogRing;
constexpr uint64_t kVaFbPixels     = 0x2000'0000ull;
constexpr uint64_t kVaCmdRing      = 0x2100'0000ull;
constexpr uint64_t kVaReplyRing    = 0x2102'0000ull; // clear of the order-10 cmd ring
constexpr uint64_t kVaGpuReqRing   = 0x2200'0000ull;
constexpr uint64_t kVaGpuReplyRing = 0x2201'0000ull;
}

// --- drv.blk.virtio --------------------------------------------------------
namespace blk {
constexpr uint64_t kVaBootInfo    = kBootInfo;
constexpr uint64_t kVaLogRing     = kLogRing;
constexpr uint64_t kVaReqRing     = 0x2000'0000ull;
constexpr uint64_t kVaReplyRing   = 0x2001'0000ull;
constexpr uint64_t kVaVirtioMmio  = 0x2100'0000ull;
constexpr uint64_t kVaVirtioDma   = 0x2200'0000ull;
}

// --- drv.usb.xhci ----------------------------------------------------------
namespace xhci {
constexpr uint64_t kVaBootInfo  = kBootInfo;
constexpr uint64_t kVaLogRing   = kLogRing;
constexpr uint64_t kVaHidRing   = 0x1900'0000ull;
constexpr uint64_t kVaXhciMmio  = 0x2000'0000ull;
constexpr uint64_t kVaXhciDma   = 0x2100'0000ull;
}

// --- drv.usb.hid -----------------------------------------------------------
namespace hid {
constexpr uint64_t kVaBootInfo  = kBootInfo;
constexpr uint64_t kVaLogRing   = kLogRing;
constexpr uint64_t kVaXhciRing  = 0x1900'0000ull;
constexpr uint64_t kVaInputRing = 0x1901'0000ull;
}

// --- ui.input --------------------------------------------------------------
namespace input {
constexpr uint64_t kVaBootInfo = kBootInfo;
constexpr uint64_t kVaLogRing  = kLogRing;
constexpr uint64_t kVaHidRing  = 0x1900'0000ull;
constexpr uint64_t kVaTtyRing  = 0x1901'0000ull;
}

// --- drv.pci ---------------------------------------------------------------
namespace pci {
constexpr uint64_t kVaBootInfo = kBootInfo;
constexpr uint64_t kVaLogRing  = kLogRing;
}

// --- dango.manager ---------------------------------------------------------
namespace dango_manager {
constexpr uint64_t kVaConsoleRing  = 0x2000'0000ull;
constexpr uint64_t kVaBootInitrd   = 0x2400'0000ull;
constexpr uint64_t kVaGpuReqRing   = 0x3100'0000ull;
constexpr uint64_t kVaGpuReplyRing = 0x3101'0000ull;
constexpr uint64_t kVaFbWindow     = 0x3102'0000ull;
}

// --- dango.auth_probe ------------------------------------------------------
namespace dango_auth_probe {
constexpr uint64_t kVaConsoleRing = 0x2000'0000ull;
}

// --- demo.ping / demo.pong -------------------------------------------------
namespace demo_ping {
constexpr uint64_t kVaSendRing      = 0x2000'0000ull;
constexpr uint64_t kVaRecvRing      = 0x2001'0000ull;
constexpr uint64_t kVaConsoleRing   = 0x2002'0000ull;
constexpr uint64_t kVaNsRequestRing = 0x2003'0000ull;
constexpr uint64_t kVaNsReplyRing   = 0x2004'0000ull;
}
namespace demo_pong {
constexpr uint64_t kVaRecvRing = 0x2000'0000ull;
constexpr uint64_t kVaSendRing = 0x2001'0000ull;
constexpr uint64_t kVaLogRing  = 0x2002'0000ull;
}

// --- app.hello -------------------------------------------------------------
namespace app_hello {
constexpr uint64_t kVaLogRing = 0x1800'0000ull;
constexpr uint64_t kVaNsTx    = 0x2A00'0000ull;
constexpr uint64_t kVaNsRx    = 0x2A01'0000ull;
constexpr uint64_t kVaFsTx    = 0x2A02'0000ull;
constexpr uint64_t kVaFsRx    = 0x2A03'0000ull;
}

} // namespace mochi::va
