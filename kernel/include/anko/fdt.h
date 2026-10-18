#pragma once
// anko/fdt.h — Flattened Device Tree (FDT/DTB) constants and generic walker.
// Shared between smp.cc (CPU/PSCI discovery) and init/boot_archive.cc (initrd discovery).
// Arch §5: DTB-to-hardware-graph translation is a kernel responsibility.

#include "anko/types.h"

namespace fdt {

// Token types (FDT spec §5.4).
constexpr u32 kMagic     = 0xD00D'FEEDu;
constexpr u32 kBeginNode = 1;
constexpr u32 kEndNode   = 2;
constexpr u32 kProp      = 3;
constexpr u32 kNop       = 4;
constexpr u32 kEnd       = 9;

// Frequently consumed properties are classified once per strings-block offset,
// rather than compared against the same literals for every node and consumer.
// Unknown properties still carry their original name (notably /aliases keys).
enum class Property : u8 {
    Unknown, Compatible, Reg, Ranges, Status, Width, Height, Stride, Format, StdoutPath,
};

// Callbacks for fdt::walk(). begin_node/end_node and one property callback
// must be non-null. property, when supplied, replaces prop.
// begin_node: called when entering a node.  depth = node nesting level (0 = root).
//             Return false to abort the walk (causes walk() to return false).
// end_node:   called when leaving a node at the same depth as its begin_node.
// prop:       called for each property.  depth = depth of the containing node.
struct Callbacks {
    void* ctx;
    bool (*begin_node)(void* ctx, const char* name, u32 depth);
    void (*end_node)(void* ctx, u32 depth);
    void (*prop)(void* ctx, const char* name, const u8* data, u32 len, u32 depth);
    void (*property)(void* ctx, Property id, const char* name, const u8* data, u32 len,
                     u32 depth) = nullptr;
};

// Walk the structure section of a DTB blob, delivering events to cb.
// Returns false if the blob is malformed, magic is wrong, or a callback aborts.
bool walk(const u8* dtb, const Callbacks& cb, usize available = 0);

// Walk the DTB memory reservation block.  The callback receives each non-zero
// [address, size] pair and may return false to abort the walk.
bool walk_memreserve(const u8* dtb, void* ctx, bool (*entry)(void* ctx, u64 address, u64 size), usize available = 0);

} // namespace fdt
