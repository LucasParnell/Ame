// boot/dtb/fdt.cpp
//
// Flattened Device Tree (FDT / DTB) parser.
//
// Spec: https://devicetree-specification.readthedocs.io/
//
// Responsibilities:
//   - Validate DTB magic (0xD00DFEED) and version
//   - Walk structure block tokens (BEGIN_NODE, END_NODE, PROP, NOP, END)
//   - Extract:
//       /memory@* reg properties  → physical RAM regions
//       /chosen stdout-path       → serial console path
//       /chosen linux,initrd-*    → initrd location (if any)
//       root #address-cells / #size-cells

#include "boot/dtb.hpp"

namespace boot::dtb {

bool validate(const void* /*dtb_ptr*/) {
    // TODO: check magic, version, totalsize sanity
    return false;
}

bool parse(const void* /*dtb_ptr*/, BootInfo& /*out*/) {
    // TODO: implement FDT structure block walker
    return false;
}

} // namespace boot::dtb
