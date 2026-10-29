#pragma once
#include "anko/types.h"
#include "rpmb.h"
#include "midix/abi/diagnostics.h"

namespace anne::rpmb_diag {
// The GIC calls this only after acknowledging and EOI'ing the stock SGI 5.
// IRQ context only records the event; it never calls firmware or takes a lock.
void notify_request(u32 cpu);
bool producer_allowed(u64 fid);

// Existing research operations share this gate. An armed lifecycle owns it
// exclusively until all firmware calls return and the request is completed.
class ResearchAccess {
public:
    ResearchAccess();
    ~ResearchAccess();
    ResearchAccess(const ResearchAccess&) = delete;
    ResearchAccess& operator=(const ResearchAccess&) = delete;
    explicit operator bool() const { return acquired_; }
private:
    bool acquired_ = false;
};

using Smc = u64 (*)(u64 fid, u64 x1, u64 x2, u64 x3);
struct Storage {
    bool (*prepare)(rpmb::ConfigInfo&);
    u64 (*service)(volatile u8*);
    u64 (*provision_key)(volatile u8*);
    bool (*write_pending)();
    rpmb::KeyInfo (*key_info)() = nullptr;
    void (*delay_ms)(u32) = nullptr;
};
// Request mapping and firmware transport are injected by the native dispatcher;
// host tests exercise the same lifecycle with a byte buffer and firmware shim.
void execute(u32 action, MxDiagEl3Rpmb& out, volatile u8* request, Smc call,
             const Storage* storage = nullptr);
}
