/*
 * scripts/linker/kernel.ld
 *
 * Phase 0: MMU is off, kernel runs from the physical address QEMU/U-Boot
 * loaded it at.
 *
 * ANKO_LOAD_BASE is defined at link time via -Wl,--defsym,ANKO_LOAD_BASE=...
 * from CMake (board-specific; see boards/<board>/board.cmake).
 *   qemu-virt:   0x40080000  (DRAM 0x40000000 + 0x80000 image offset)
 *   switch-t210: 0x80080000  (DRAM 0x80000000 + 0x80000 image offset)
 *
 * Phase 1 will switch to a high-half virtual link address (TTBR1 window
 * at 0xFFFF000000000000) once the MMU is up and an identity-map trampoline
 * exists.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS {
    KERNEL_VA_BASE = DEFINED(ANKO_LINK_BASE) ? ANKO_LINK_BASE : ANKO_LOAD_BASE;
    KERNEL_VA_OFFSET = KERNEL_VA_BASE - ANKO_LOAD_BASE;
    . = KERNEL_VA_BASE;

    .text : AT(ADDR(.text) - KERNEL_VA_OFFSET) {
        __text_start = .;
        KEEP(*(.text.entry))         /* entry.S must be first */
        *(.text*)
        __text_end = .;
    }

    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - KERNEL_VA_OFFSET) {
        __rodata_start = .;
        *(.rodata*)
        KEEP(*(.kaslr))
        __rodata_end = .;
    }

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - KERNEL_VA_OFFSET) {
        *(.data*)
    }

    . = ALIGN(8);
    .init_array : AT(ADDR(.init_array) - KERNEL_VA_OFFSET) {
        __init_array_start = .;
        KEEP(*(.init_array*))
        __init_array_end = .;
    }

    . = ALIGN(16);
    .bss : AT(ADDR(.bss) - KERNEL_VA_OFFSET) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}
