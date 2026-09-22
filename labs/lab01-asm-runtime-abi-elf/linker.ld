OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

PHDRS
{
    text   PT_LOAD FLAGS(5); /* R + X */
    rodata PT_LOAD FLAGS(4); /* R     */
    data   PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text : ALIGN(16) {
        KEEP(*(.text.init))
        *(.text .text.*)
    } > RAM :text

    .lab_blob : ALIGN(16) {
        __lab_blob_start = .;
        KEEP(*(.lab_blob))
        __lab_blob_end = .;
    } > RAM :text

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    } > RAM :rodata

    .eh_frame : ALIGN(8) {
        KEEP(*(.eh_frame))
    } > RAM :rodata

    .data : ALIGN(16) {
        *(.data .data.*)
    } > RAM :data

    .sdata : ALIGN(16) {
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
    } > RAM :data

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM :data

    .stack (NOLOAD) : ALIGN(16) {
        __stack_bottom = .;
        . += 16K;
        __stack_top = .;
    } > RAM :data

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }

    ASSERT(__lab_blob_end - __lab_blob_start <= 256,
           "lab blob exceeds the 256-byte copy buffer")
    ASSERT((__stack_top & 15) == 0, "stack is not 16-byte aligned")
}
