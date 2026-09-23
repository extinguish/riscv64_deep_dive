OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text : ALIGN(16) {
        KEEP(*(.text.init))
        KEEP(*(.text.flow))
        KEEP(*(.text.exercise))
        *(.text .text.*)
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

    .io_tables (NOLOAD) : ALIGN(4096) {
        __io_tables_start = .;
        *(.io_tables)
        . = ALIGN(4096);
        __io_tables_end = .;
    } > RAM :data

    .bss (NOLOAD) : ALIGN(64) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(64);
        __bss_end = .;
    } > RAM :data

    .stack (NOLOAD) : ALIGN(4096) {
        __stack_bottom = .;
        . += 16384;
        __stack_top = .;
    } > RAM :data

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }
}
