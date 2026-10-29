// boot/entry/boot_main.cpp
//
// C++ entry point called from start.S after early stack is live.
// Responsible for:
//   1. Parsing the DTB passed by U-Boot
//   2. Building the physical memory map
//   3. Setting up early page tables and enabling the MMU
//   4. Handing off to kernel_main()
//
// NOTE: No global constructors have run yet at this point.
//       Avoid non-trivial static/global objects in boot code.

#include "boot/boot_info.hpp"
#include "boot/dtb.hpp"
#include "boot/memory.hpp"
#include "boot/mmu.hpp"

// Defined in the kernel; declared here for the handoff.
// TODO: move this declaration to a shared header once the kernel exists.
extern "C" [[noreturn]] void kernel_main(const boot::BootInfo& info);

extern "C" [[noreturn]] void boot_main(uint64_t dtb_phys) {
    boot::BootInfo info{};
    info.dtb_phys = dtb_phys;

    // TODO: step 1 — validate and parse DTB
    // TODO: step 2 — build physical memory map (reserve DTB, boot image)
    // TODO: step 3 — enable MMU (identity map + kernel window)
    // TODO: step 4 — call kernel_main(info)

    // Unreachable placeholder
    while (true) {}
}
