/*
 * scripts/linker/user.ld — linker script for Mochi userspace binaries.
 *
 * Each service is a statically-linked ET_EXEC ELF64 at a fixed VA.
 * The kernel ELF loader reads PT_LOAD segments and maps them into a
 * fresh address space. No dynamic linking in V0.
 *
 * User VA space: lower half, 0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF.
 * Each service binary is linked at a conventional base; the kernel maps
 * it wherever fits in the process VA space for V0.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

USER_LOAD_BASE = 0x400000; /* 4 MiB — above null guard page */

SECTIONS {
    . = USER_LOAD_BASE;

    .text : {
        KEEP(*(.text.entry)) /* _start must be at entry point */
        *(.text*)
    }

    . = ALIGN(4096);
    .rodata : ALIGN(4096) { *(.rodata*) }

    . = ALIGN(4096);
    .data : ALIGN(4096) { *(.data*) }

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
        *(.gnu*)
    }
}
