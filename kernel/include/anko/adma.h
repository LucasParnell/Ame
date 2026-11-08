#pragma once
#include "anko/types.h"
#include "anko/syscall.h"

// Arena Data Movement Agent. The initial backend is a synchronous CPU copy;
// SDHCI ADMA2 is storage DMA and cannot perform memory-to-memory migration.
// Completion is a real fence: callers may retire sources only after it returns.
namespace adma {
// Backends are kernel-owned and outlive their requests. All callbacks are
// nonblocking. E_OK from poll/cancel proves that DMA has stopped touching both
// buffers. On error, cancel must establish that fence before pages can retire.
struct Backend {
    void* context;
    bool coherent;
    SysError (*submit)(void*, PhysAddr, PhysAddr, usize, u64* cookie);
    SysError (*poll)(void*, u64 cookie);
    SysError (*cancel)(void*, u64 cookie);
};
struct Copy {
    const Backend* backend = nullptr;
    PhysAddr destination = 0, source = 0;
    u64 cookie = 0;
    SysError result = E_OK;
    bool active = false;
};
SysError install(const Backend*); // null selects CPU; refuses active transfers
SysError start(Copy&, PhysAddr destination, PhysAddr source);
SysError poll(Copy&);
SysError cancel(Copy&);
u64 active_count();
void publish_dma(PhysAddr, usize size, bool coherent);
void complete_dma(PhysAddr, usize size, bool coherent);
void copy_page(PhysAddr destination, PhysAddr source);
void fence();
}
