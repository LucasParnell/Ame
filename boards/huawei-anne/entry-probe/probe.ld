OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS {
    . = 0x00480000;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
    ASSERT(SIZEOF(.bss) == 0, "entry probe must not depend on BSS initialization")
    .stack ALIGN(16) (NOLOAD) : { . += 0x4000; stack_end = .; }
    image_end = .;
    /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
