// Linux v4.4 fs/pstore/{ram.c,ram_core.c}: console ring header and placement.
#include "native_log.h"
#include "anko/fdt.h"
#include "libk/string.h"

namespace anne {
namespace {
constexpr PhysAddr kPstoreBase = 0x34800000;
constexpr u64 kPstoreSize = 0x100000;
constexpr u32 kConsoleSize = 0x80000;
constexpr u32 kPmsgSize = 0x20000;
constexpr PhysAddr kConsoleBase = kPstoreBase + kPstoreSize - kConsoleSize - kPmsgSize;
constexpr u32 kCapacity = kConsoleSize - 12;
volatile u32* header = nullptr;
volatile u8* bytes = nullptr;
u32 cursor = 0;
void sync() {
#if defined(__aarch64__)
    asm volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

struct Layout {
    u8 node[64]{};
    bool compatible = false;
    bool enabled = true;
    PhysAddr base = 0;
    u64 size = 0;
    u32 region_phandle = 0;
    u32 memory_region = 0;
    u64 console_size = 0;
    u64 pmsg_size = 0;
    u64 ftrace_size = 0;
    u64 record_size = 0;
    u64 ecc_size = 0;
};
bool begin(void* v, const char* name, u32 depth) {
    auto& l = *static_cast<Layout*>(v);
    if (depth >= 64) return false;
    u8 kind = 0;
    if (depth == 1 && libk::strcmp(name, "reserved-memory") == 0) kind = 1;
    if (depth == 2 && l.node[1] == 1 && libk::strcmp(name, "pstore-mem") == 0) kind = 2;
    if (depth == 1 && libk::strcmp(name, "ramoops") == 0) kind = 3;
    l.node[depth] = kind;
    return true;
}
void end(void*, u32) {}
u64 value(const u8* data, u32 length) {
    return length == 4 ? libk::be32(data) : length == 8 ? libk::be64(data) : ~u64{0};
}
void prop(void* v, const char* name, const u8* data, u32 length, u32 depth) {
    auto& l = *static_cast<Layout*>(v);
    if ((l.node[depth] == 2 || l.node[depth] == 3) && libk::strcmp(name, "status") == 0) {
        if (!((length == 3 && libk::memcmp(data, "ok", 3) == 0) ||
              (length == 5 && libk::memcmp(data, "okay", 5) == 0))) l.enabled = false;
    }
    if (l.node[depth] == 2) {
        if (libk::strcmp(name, "reg") == 0 && length == 16) {
            l.base = libk::be64(data); l.size = libk::be64(data + 8);
        } else if (libk::strcmp(name, "phandle") == 0 && length == 4) {
            l.region_phandle = libk::be32(data);
        }
    } else if (l.node[depth] == 3) {
        if (libk::strcmp(name, "compatible") == 0 && length == 8 &&
            libk::memcmp(data, "ramoops", 8) == 0) l.compatible = true;
        else if (libk::strcmp(name, "memory-region") == 0 && length == 4) l.memory_region = libk::be32(data);
        else if (libk::strcmp(name, "console-size") == 0) l.console_size = value(data, length);
        else if (libk::strcmp(name, "pmsg-size") == 0) l.pmsg_size = value(data, length);
        else if (libk::strcmp(name, "ftrace-size") == 0) l.ftrace_size = value(data, length);
        else if (libk::strcmp(name, "record-size") == 0) l.record_size = value(data, length);
        else if (libk::strcmp(name, "ecc-size") == 0) l.ecc_size = value(data, length);
    }
}
}

bool native_log_layout(PhysAddr dtb, PhysAddr* console_base) {
    if (!dtb || (dtb & 7) || !console_base) return false;
    Layout layout{};
    if (!fdt::walk(reinterpret_cast<const u8*>(dtb), {&layout, begin, end, prop}) ||
        !layout.compatible || !layout.enabled || layout.base != kPstoreBase || layout.size != kPstoreSize ||
        !layout.region_phandle || layout.region_phandle != layout.memory_region ||
        layout.console_size != kConsoleSize || layout.pmsg_size != kPmsgSize ||
        layout.ftrace_size || layout.ecc_size || layout.record_size != 0x20000) return false;
    *console_base = kConsoleBase;
    return true;
}
bool native_log_init(PhysAddr dtb) {
    header = nullptr; bytes = nullptr; cursor = 0;
    PhysAddr base = 0;
    if (!native_log_layout(dtb, &base)) return false;
    header = reinterpret_cast<volatile u32*>(base);
    bytes = reinterpret_cast<volatile u8*>(base + 12);
    header[0] = 0x43474244; // persistent_ram signature, console has signature XOR 0.
    header[1] = 0;
    header[2] = 0;
    sync();
    return true;
}

void native_log_putchar(char c) {
    // First boot is one-core. Preserve a linear early log without wraparound.
    if (!header || cursor >= kCapacity) return;
    bytes[cursor++] = static_cast<u8>(c);
    sync();
    header[1] = cursor;
    header[2] = cursor;
    sync();
}
}
