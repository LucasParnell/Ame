/*
 * scripts/linker/boot.ld
 *
 * Linker script for ame-boot.
 *
 * Assumes U-Boot loads the image to BOOT_LOAD_ADDR (0x4008_0000 is the
 * standard QEMU virt machine load address for a secondary payload).
 * Adjust for real hardware.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

BOOT_LOAD_ADDR = 0x40080000;
BOOT_STACK_SIZE = 0x8000;   /* 32 KiB early stack */

SECTIONS {
    . = BOOT_LOAD_ADDR;

    /*
     * .text.boot must be first so _start lands at the load address.
     */
    .text : {
        KEEP(*(.text.boot))
        KEEP(*(.text.mmu))
        *(.text*)
    }

    .rodata : ALIGN(8) {
        *(.rodata*)
    }

    .data : ALIGN(8) {
        *(.data*)
    }

    /*
     * Page tables will be allocated by early_alloc_pages() from the bump
     * region that starts after BSS. Keep them 4KB-aligned.
     */
    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /*
     * Early boot stack — grows downward from __boot_stack_top.
     */
    . = ALIGN(16);
    __boot_stack_bottom = .;
    . += BOOT_STACK_SIZE;
    __boot_stack_top = .;

    /*
     * Bump allocator region — everything from here is free physical RAM
     * (subject to actual DRAM size from DTB).
     */
    __early_alloc_start = ALIGN(4096);

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}
