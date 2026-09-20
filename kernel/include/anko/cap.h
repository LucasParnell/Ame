#pragma once
// anko/cap.h — capability data layout.
//
// Spec ref: §5.5 Capability Tables
//
// Handle format:
//   bits  0..31  slot index into the per-process cap table
//   bits 32..63  generation counter (revocation check)

#include "anko/types.h"
#include "anko/object.h"
#define MIDIX_ABI_NO_COMPAT_NAMES 1
#include "midix/abi/cap.h"
#undef MIDIX_ABI_NO_COMPAT_NAMES

// ---------------------------------------------------------------------------
// Handle encoding (spec §5.5)
// ---------------------------------------------------------------------------
using CapHandle = u64;

constexpr CapHandle kNullCap = 0;

constexpr u32 cap_index(CapHandle h) {
    return static_cast<u32>(h & 0xFFFF'FFFFu);
}
constexpr u32 cap_generation(CapHandle h) {
    return static_cast<u32>(h >> 32);
}
constexpr CapHandle cap_make(u32 index, u32 gen) {
    return (static_cast<u64>(gen) << 32) | index;
}

// ---------------------------------------------------------------------------
// Cap types (spec §5.5)
// ---------------------------------------------------------------------------
enum CapType : u16 {
    CAP_NULL = MX_CAP_NULL,
    CAP_THREAD = MX_CAP_THREAD,
    CAP_PROCESS = MX_CAP_PROCESS,
    CAP_PAGE = MX_CAP_PAGE,
    CAP_SHARED_RING = MX_CAP_SHARED_RING,
    CAP_DOORBELL = MX_CAP_DOORBELL,
    CAP_ARENA = MX_CAP_ARENA,
    CAP_NAMESERVICE = MX_CAP_NAMESERVICE,
    CAP_SUPERVISOR = MX_CAP_SUPERVISOR,
    CAP_UART = MX_CAP_UART,
    CAP_TIMER = MX_CAP_TIMER,
    CAP_VM_ROOT = MX_CAP_VM_ROOT,
    CAP_VM = MX_CAP_VM,
    CAP_VCPU = MX_CAP_VCPU,
    CAP_VM_MEMORY = MX_CAP_VM_MEMORY,
    CAP_VM_CHANNEL = MX_CAP_VM_CHANNEL,
    CAP_SCHED_CONTEXT = MX_CAP_SCHED_CONTEXT,
    CAP_MMIO = MX_CAP_MMIO, // device MMIO range object
    CAP_IRQ = MX_CAP_IRQ,   // IRQ line object
};

static_assert(static_cast<u16>(CAP_PROCESS) == static_cast<u16>(MX_CAP_PROCESS),
              "kernel cap type drift: PROCESS");
static_assert(static_cast<u16>(CAP_SHARED_RING) == static_cast<u16>(MX_CAP_SHARED_RING),
              "kernel cap type drift: SHARED_RING");
static_assert(static_cast<u16>(CAP_DOORBELL) == static_cast<u16>(MX_CAP_DOORBELL),
              "kernel cap type drift: DOORBELL");
static_assert(static_cast<u16>(CAP_ARENA) == static_cast<u16>(MX_CAP_ARENA),
              "kernel cap type drift: ARENA");
static_assert(static_cast<u16>(CAP_UART) == static_cast<u16>(MX_CAP_UART),
              "kernel cap type drift: UART");
static_assert(static_cast<u16>(CAP_VM_ROOT) == static_cast<u16>(MX_CAP_VM_ROOT),
              "kernel cap type drift: VM_ROOT");
static_assert(static_cast<u16>(CAP_SCHED_CONTEXT) == static_cast<u16>(MX_CAP_SCHED_CONTEXT),
              "kernel cap type drift: SCHED_CONTEXT");
static_assert(static_cast<u16>(CAP_MMIO) == static_cast<u16>(MX_CAP_MMIO),
              "kernel cap type drift: MMIO");
static_assert(static_cast<u16>(CAP_IRQ) == static_cast<u16>(MX_CAP_IRQ),
              "kernel cap type drift: IRQ");

// ---------------------------------------------------------------------------
// MMIO memory attribute classes (determines page-table memory type)
// ---------------------------------------------------------------------------
enum MxMmioAttr : u32 {
    MX_MMIO_DEVICE_nGnRnE = 1, // strongly ordered (xHCI registers)
    MX_MMIO_DEVICE_nGnRE = 2,  // device, early write ack (fw_cfg)
    MX_MMIO_NORMAL_NC = 3,     // normal noncacheable (framebuffer pixels)
    MX_MMIO_WRITE_COMBINE = 4, // write-combining (framebuffer if supported)
};

// Kernel object referenced by a CAP_MMIO entry.
struct MmioRange {
    PhysAddr phys_base;
    u64 size;
    u32 attr; // MxMmioAttr — determines page table memory type
    u32 flags;
    u32 device_hint; // DTB phandle or zero (informational)
    u32 region_index;
};

// Kernel object referenced by a CAP_IRQ entry.
struct IrqLine {
    u32 irq_id;  // SPI/LPI/MSI number
    u32 trigger; // 0=level, 1=edge
    u32 flags;
    u32 device_hint;
    // Bound doorbell — set by SYS_IRQ_BIND_DOORBELL, initially zero.
    u64 doorbell_cap;
};

// ---------------------------------------------------------------------------
// Rights bits (spec §5.5)
// ---------------------------------------------------------------------------
enum CapRights : u32 {
    RIGHT_READ = MX_RIGHT_READ,
    RIGHT_WRITE = MX_RIGHT_WRITE,
    RIGHT_MAP = MX_RIGHT_MAP,
    RIGHT_EXEC = MX_RIGHT_EXEC,
    RIGHT_NOTIFY = MX_RIGHT_NOTIFY,
    RIGHT_WAIT = MX_RIGHT_WAIT,
    RIGHT_DUP = MX_RIGHT_DUP,
    RIGHT_SPAWN = MX_RIGHT_SPAWN,
    RIGHT_KILL = MX_RIGHT_KILL,
    RIGHT_REGISTER = MX_RIGHT_REGISTER,
    RIGHT_THREAD_CONTROL = MX_RIGHT_THREAD_CONTROL,
    RIGHT_VM_CONTROL = MX_RIGHT_VM_CONTROL,
    RIGHT_VM_MAP = MX_RIGHT_VM_MAP,
    RIGHT_VM_INSPECT = MX_RIGHT_VM_INSPECT,
    RIGHT_VM_DESTROY = MX_RIGHT_VM_DESTROY,
    RIGHT_VCPU_CONTROL = MX_RIGHT_VCPU_CONTROL,
    RIGHT_VCPU_INSPECT = MX_RIGHT_VCPU_INSPECT,
    RIGHT_VM_MEMORY_MAP = MX_RIGHT_VM_MEMORY_MAP,
    RIGHT_VM_MEMORY_INSPECT = MX_RIGHT_VM_MEMORY_INSPECT,
    RIGHT_VM_CHANNEL_SEND = MX_RIGHT_VM_CHANNEL_SEND,
    RIGHT_VM_CHANNEL_RECV = MX_RIGHT_VM_CHANNEL_RECV,
    RIGHT_VM_CHANNEL_NOTIFY = MX_RIGHT_VM_CHANNEL_NOTIFY,
    RIGHT_VM_CHANNEL_INSPECT = MX_RIGHT_VM_CHANNEL_INSPECT,
    RIGHT_VM_CREATE = MX_RIGHT_VM_CREATE,
    RIGHT_SCHED_CONTEXT_CONTROL = MX_RIGHT_SCHED_CONTEXT_CONTROL,
};

static_assert(static_cast<u32>(RIGHT_READ) == static_cast<u32>(MX_RIGHT_READ),
              "kernel right drift: READ");
static_assert(static_cast<u32>(RIGHT_WRITE) == static_cast<u32>(MX_RIGHT_WRITE),
              "kernel right drift: WRITE");
static_assert(static_cast<u32>(RIGHT_MAP) == static_cast<u32>(MX_RIGHT_MAP),
              "kernel right drift: MAP");
static_assert(static_cast<u32>(RIGHT_NOTIFY) == static_cast<u32>(MX_RIGHT_NOTIFY),
              "kernel right drift: NOTIFY");
static_assert(static_cast<u32>(RIGHT_WAIT) == static_cast<u32>(MX_RIGHT_WAIT),
              "kernel right drift: WAIT");
static_assert(static_cast<u32>(RIGHT_DUP) == static_cast<u32>(MX_RIGHT_DUP),
              "kernel right drift: DUP");
static_assert(static_cast<u32>(RIGHT_VM_CREATE) == static_cast<u32>(MX_RIGHT_VM_CREATE),
              "kernel right drift: VM_CREATE");

// ---------------------------------------------------------------------------
// CapEntry — one slot in the per-process capability table.
// Exact layout from spec §5.5.  Must be exactly 64 bytes.
// ---------------------------------------------------------------------------
struct CapEntry {
    void* target; // pointer to the kernel object (Thread*, Ring*, …)
    object::Record* object;
    u32 rights_mask; // CapRights bitmask
    u32 generation;  // matches the generation in the handle; mismatch = revoked
    u16 type_tag;    // CapType
    u16 flags;       // reserved for future use
    u8 reserved[36];
};
static_assert(sizeof(CapEntry) == 64, "CapEntry must be 64 bytes (spec §5.5)");

// ---------------------------------------------------------------------------
// Per-process cap table.
// V0: dense flat array of CapEntry[kCapTableSize].
// Lookup is O(1): extract index from handle, check generation.
// ---------------------------------------------------------------------------
constexpr u32 kCapTableSize = 1024; // spec §7: flat-table threshold; 64 KB per process

struct CapTable {
    CapEntry entries[kCapTableSize];
    u32 next_free; // bump pointer into entries[]
    u32 lock;
};

namespace cap {

class Ref {
    CapEntry entry_{};

  public:
    Ref() = default;
    Ref(decltype(nullptr)) {}
    explicit Ref(const CapEntry& entry) : entry_(entry) {
        object::retain(entry_.object);
    }
    Ref(const Ref& other) : Ref(other.entry_) {}
    Ref(Ref&& other) : entry_(other.entry_) {
        other.entry_ = {};
    }
    ~Ref() {
        object::release(entry_.object);
    }
    Ref& operator=(const Ref&) = delete;
    const CapEntry* operator->() const {
        return &entry_;
    }
    operator const CapEntry*() const {
        return entry_.target ? &entry_ : nullptr;
    }
};

template <class T> class Borrow {
    Ref reference_;

  public:
    Borrow(decltype(nullptr)) {}
    explicit Borrow(const Ref& ref) : reference_(ref) {}
    T* get() const {
        return reference_ ? static_cast<T*>(reference_->target) : nullptr;
    }
    operator T*() const {
        return get();
    }
    T* operator->() const {
        return get();
    }
    T& operator*() const {
        return *get();
    }
};

void init();
CapHandle alloc(CapTable& table, CapType type, void* target, u32 rights);
Ref lookup(CapTable& table, CapHandle handle, CapType expected_type);
Ref lookup_any(CapTable& table, CapHandle handle);
void revoke(CapTable& table, CapHandle handle);
void clear(CapTable& table);
void revoke_target(CapTable& table, void* target);
bool reserve_slots(CapTable& table, u32 count);
CapHandle dup(CapTable& table, CapHandle src, u32 rights_mask);

} // namespace cap
