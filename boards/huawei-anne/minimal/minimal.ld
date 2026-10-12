OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS {
    PROVIDE(ANNE_IMAGE_BASE = 0x00480000);
    . = ANNE_IMAGE_BASE;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss ALIGN(8) (NOLOAD) : {
        __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(8); __bss_end = .;
    }
    .stack ALIGN(16) (NOLOAD) : {
        . += 0x4000; stack_end = .; . += 0x4000; fault_stack_end = .;
    }
    image_end = .;
    .init_array : { *(.init_array .init_array.*) }
    ASSERT(SIZEOF(.init_array) == 0, "minimal entry cannot depend on constructors")
    ASSERT(image_end - _start < 0x100000, "minimal entry exceeds reserved low RAM")
    /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
