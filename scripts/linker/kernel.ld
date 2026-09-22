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
    . = ANKO_LOAD_BASE;

    .text : ALIGN(4096) {
        __text_start = .;
        KEEP(*(.text.entry))         /* entry.S must be first */
        *(.text*)
        __text_end = .;
    }

    .rodata : ALIGN(8) {
        *(.rodata*)
    }

    .data : ALIGN(8) {
        *(.data*)
    }

    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(.init_array*))
        __init_array_end = .;
    }

    .bss : ALIGN(16) {
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
