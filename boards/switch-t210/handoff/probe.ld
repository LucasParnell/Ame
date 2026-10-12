ENTRY(_start)
SECTIONS {
    . = PROBE_LOAD_BASE;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .marker ALIGN(8) : { probe_end = .; QUAD(0); }
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
    ASSERT(SIZEOF(.bss) == 0, "handoff probe must not depend on BSS initialization")
    .stack ALIGN(16) (NOLOAD) : { . += 0x4000; stack_end = .; }
    /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
